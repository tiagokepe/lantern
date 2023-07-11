
#include "postgres.h"

#include "external_index.h"

#include <assert.h>

#include "access/generic_xlog.h"  // GenericXLog
#include "common/relpath.h"
#include "pg_config.h"       // BLCKSZ
#include "storage/bufmgr.h"  // Buffer
#include "usearch.h"
#include "utils/relcache.h"

static inline void  fill_blockno_mapping();
static BlockNumber *wal_retriever_block_numbers = NULL;

static char *extract_node(char               *data,
                          int                 progress,
                          int                 dim,
                          usearch_metadata_t *metadata,
                          /*->>output*/ int  *node_size,
                          int                *level)
{
    const int NODE_HEAD_BYTES = sizeof(usearch_label_t) + 4 /*sizeof dim */ + 4 /*sizeof level*/;
    const int VECTOR_BYTES = dim * sizeof(float);
    int       node_bytes = 0;
    char     *tape = data + progress;
    int       read_dim_bytes = -1;
    memcpy(&read_dim_bytes, tape + sizeof(usearch_label_t),
           4);  //+sizeof(label)
    memcpy(level, tape + sizeof(usearch_label_t) + 4,
           4);  //+sizeof(label)+sizeof(dim)
    assert(VECTOR_BYTES == read_dim_bytes);
    node_bytes += NODE_HEAD_BYTES + metadata->neighbors_base_bytes;
    node_bytes += metadata->neighbors_bytes * *level;
    node_bytes += VECTOR_BYTES;
    *node_size = node_bytes;
    return tape;
}

void StoreExternalIndex(Relation        index,
                        usearch_index_t external_index,
                        ForkNumber      forkNum,
                        char           *data,
                        size_t          external_index_size,
                        int             dimension,
                        size_t          num_added_vectors)
{
    char  *node = 0;
    int    node_size = 0;
    int    node_level = 0;
    int    progress = 64;  // usearch header size
    uint32 predicted_next_block = InvalidBlockNumber;
    uint32 last_block = -1;
    uint32 block = 1;
    uint32 blockno_index_start = -1;

    HnswIndexPage *bufferpage = palloc(BLCKSZ);

    usearch_metadata_t metadata = usearch_metadata(external_index, NULL);

    // header page is created twice. it is always at block=0 so the second time just overrides it
    // it is added here to make sure a data block does not get to block=0.
    // after somem sleep I will prob find a better way to do this
    CreateHeaderPage(index, data, forkNum, dimension, num_added_vectors, -1, -1, false);

    /* Add all the vectors to the WAL */
    for(int node_id = 0; node_id < num_added_vectors;) {
        // 1. create HnswIndexPage

        // 2. fill header and special

        // 3. while available space is larger than node_size
        //     3a. add node to page

        // 4. commit buffer

        Buffer                     buf;
        Page                       page;
        GenericXLogState          *state;
        HnswIndexPageSpecialBlock *special;

        buf = ReadBufferExtended(index, forkNum, P_NEW, RBM_NORMAL, NULL);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

        state = GenericXLogStart(index);
        page = GenericXLogRegisterBuffer(state, buf, GENERIC_XLOG_FULL_IMAGE);
        PageInit(page, BufferGetPageSize(buf), sizeof(HnswIndexPageSpecialBlock));

        if(predicted_next_block != InvalidBlockNumber) {
            if(predicted_next_block != BufferGetBlockNumber(buf)) {
                elog(ERROR,
                     "my block number hypothesis failed. "
                     "predicted block number %d does not match "
                     "actual %d",
                     predicted_next_block,
                     BufferGetBlockNumber(buf));
            }
        }

        special = (HnswIndexPageSpecialBlock *)PageGetSpecialPointer(page);
        special->firstId = node_id;
        special->nextblockno = InvalidBlockNumber;

        // note: even if the condition is true, nodepage may be too large
        // as the condition does not take into account the flexible array component
        while(PageGetFreeSpace(page) > sizeof(HnswIndexPage) + dimension * sizeof(float)) {
            if(node_id >= num_added_vectors) break;
            memset(bufferpage, 0, BLCKSZ);
            /************* extract node from usearch index************/

            node = extract_node(data,
                                progress,
                                dimension,
                                &metadata,
                                /*->>output*/ &node_size,
                                &node_level);
            bufferpage->id = node_id;
            bufferpage->level = node_level;
            bufferpage->size = node_size;
            if(node_level > 100) {
                elog(ERROR,
                     "node level is too large. at id %d"
                     "this is likely a bug in usearch. "
                     "level: %d",
                     node_id,
                     node_level);
            }

            // node should not be larger than the 8k bufferpage
            // invariant holds because of dimension <2000 check in index creation
            // once quantization is enabled, we can allow larger overall dims
            assert(bufferpage + offsetof(HnswIndexPage, node) + node_size < bufferpage + BLCKSZ);
            memcpy(bufferpage->node, node, node_size);

            if(PageAddItem(page, (Item)bufferpage, sizeof(HnswIndexPage) + node_size, InvalidOffsetNumber, false, false)
               == InvalidOffsetNumber) {
                // break to get a fresh page
                // todo:: properly test this case
                break;
            }

            // we successfully recorded the node. move to the next one
            progress += node_size;
            node_id++;
        }
        if(node_id < num_added_vectors) {
            predicted_next_block = BufferGetBlockNumber(buf) + 1;
        } else {
            last_block = predicted_next_block;
            predicted_next_block = InvalidBlockNumber;
        }
        special->lastId = node_id - 1;
        special->nextblockno = predicted_next_block;
        MarkBufferDirty(buf);
        GenericXLogFinish(state);
        UnlockReleaseBuffer(buf);
    }

    HnswBlockmapPage *blockmap = palloc0(BLCKSZ);
    // I do update header page yet another time here to update num_vectors and the global
    // HEADER_FOR_EXTERNAL_RETRIEVER which is used by fill_blockno_mapping
    CreateHeaderPage(index, data, forkNum, dimension, num_added_vectors, blockno_index_start, last_block, true);
    wal_retriever_block_numbers = palloc0(sizeof(BlockNumber) * num_added_vectors);
    if(wal_retriever_block_numbers == NULL) elog(ERROR, "could not allocate wal_retriever_block_numbers");
    INDEX_RELATION_FOR_RETRIEVER = index;
    fill_blockno_mapping();

    for(int node_id = 0; node_id < num_added_vectors; node_id += HNSW_BLOCKMAP_BLOCKS_PER_PAGE) {
        Buffer                     buf;
        Page                       page;
        GenericXLogState          *state;
        HnswIndexPageSpecialBlock *special;

        buf = ReadBufferExtended(index, forkNum, P_NEW, RBM_NORMAL, NULL);
        if(blockno_index_start == -1) {
            blockno_index_start = BufferGetBlockNumber(buf);
        }
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

        state = GenericXLogStart(index);
        page = GenericXLogRegisterBuffer(state, buf, GENERIC_XLOG_FULL_IMAGE);
        PageInit(page, BufferGetPageSize(buf), sizeof(HnswIndexPageSpecialBlock));

#if 0
        /* I was trying to figure out how postgres allocates blocks
         * It seems it always gives n+1 block after block n and have not been
         * able to find a counterexample
         */
        if(predicted_next_block != InvalidBlockNumber) {
            if(predicted_next_block != BufferGetBlockNumber(buf)) {
                elog(ERROR,
                     "my block number hypothesis failed. "
                     "predicted block number %d does not match "
                     "actual %d",
                     predicted_next_block,
                     BufferGetBlockNumber(buf));
            }
        }
#endif

        special = (HnswIndexPageSpecialBlock *)PageGetSpecialPointer(page);
        special->firstId = node_id;
        special->nextblockno = InvalidBlockNumber;

        memset(blockmap, 0, BLCKSZ);
        blockmap->first_id = node_id;
        memcpy(blockmap->blocknos,
               wal_retriever_block_numbers + node_id,
               sizeof(BlockNumber) * HNSW_BLOCKMAP_BLOCKS_PER_PAGE);
        if(PageAddItem(page, (Item)blockmap, sizeof(HnswBlockmapPage), InvalidOffsetNumber, false, false)
           == InvalidOffsetNumber) {
            // we always add a single Blockmap page per Index page which has a fixed size that
            // always fits in postgres wal page. So this should never happen
            // (Assumes 8k BLKSZ. we can make HnswBlockmapPage size configurable by BLCKSZ)
            elog(ERROR, "could not add blockmap to page");
        }

        if(node_id < num_added_vectors) {
            predicted_next_block = BufferGetBlockNumber(buf) + 1;
        } else {
            last_block = predicted_next_block;
            predicted_next_block = InvalidBlockNumber;
        }
        special->lastId = node_id - 1;
        special->nextblockno = predicted_next_block;
        MarkBufferDirty(buf);
        GenericXLogFinish(state);
        UnlockReleaseBuffer(buf);
    }

    pfree(wal_retriever_block_numbers);
    wal_retriever_block_numbers = NULL;
    pfree(blockmap);
    CreateHeaderPage(index, data, forkNum, dimension, num_added_vectors, blockno_index_start, last_block, true);

    pfree(bufferpage);
}

void CreateHeaderPage(Relation   index,
                      char      *usearchHeader64,
                      ForkNumber forkNum,
                      int        vector_dim,
                      int        num_vectors,
                      uint32     blockno_index_start,
                      uint32     num_blocks,
                      bool       update)
{
    Buffer               buf;
    Page                 page;
    GenericXLogState    *state;
    HnswIndexHeaderPage *headerp;
    BlockNumber          headerblockno = 0;

    if(update) {
        headerblockno = 0;
    } else {
        headerblockno = P_NEW;
    }
    buf = ReadBufferExtended(index, forkNum, headerblockno, RBM_NORMAL, NULL);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

    state = GenericXLogStart(index);
    page = GenericXLogRegisterBuffer(state, buf, GENERIC_XLOG_FULL_IMAGE);
    PageInit(page, BufferGetPageSize(buf), 0);

    headerp = (HnswIndexHeaderPage *)PageGetContents(page);
    headerp->magicNumber = LDB_WAL_MAGIC_NUMBER;
    headerp->version = LDB_WAL_VERSION_NUMBER;
    headerp->vector_dim = vector_dim;
    headerp->num_vectors = num_vectors;
    headerp->blockno_index_start = blockno_index_start;
    headerp->num_blocks = num_blocks;
    memcpy(headerp->usearch_header, usearchHeader64, 64);
    ((PageHeader)page)->pd_lower = ((char *)headerp + sizeof(HnswIndexHeaderPage)) - (char *)page;

    HEADER_FOR_EXTERNAL_RETRIEVER = *headerp;
    assert(BufferGetBlockNumber(buf) == 0);

    MarkBufferDirty(buf);
    GenericXLogFinish(state);
    UnlockReleaseBuffer(buf);
}

Relation INDEX_RELATION_FOR_RETRIEVER;
#if LANTERNDB_COPYNODES
static char *wal_retriever_area = NULL;
static int   wal_retriever_area_size = 0;
static int   wal_retriever_area_offset = 0;
#endif

#define TAKENBUFFERS_MAX 100
static Buffer       takenbuffers[ TAKENBUFFERS_MAX ];
static int          takenbuffers_next = 0;
HnswIndexHeaderPage HEADER_FOR_EXTERNAL_RETRIEVER;

void ldb_wal_retriever_area_init(int size)
{
#if LANTERNDB_COPYNODES
    wal_retriever_area = palloc(size);
    if(wal_retriever_area == NULL) elog(ERROR, "could not allocate wal_retriever_area");
    wal_retriever_area_size = size;
    wal_retriever_area_offset = 0;
#endif

    if(HEADER_FOR_EXTERNAL_RETRIEVER.num_vectors <= 0) {
        elog(ERROR, "ldb_wal_retriever_area_init called with num_vectors <= 0");
    }
    /* fill in a buffer with blockno index information, before spilling it to disk */
    // I copy into wal_retriever_block_numbers from blocknos which is array of 2000.
    // in case the array ends early, this makes sure we do not overwrite mempry
    // needless to say - yet another hack that shall be removed asap!
    wal_retriever_block_numbers = palloc(sizeof(BlockNumber) * HEADER_FOR_EXTERNAL_RETRIEVER.num_vectors + 2000);
    if(wal_retriever_block_numbers == NULL) elog(ERROR, "could not allocate wal_retriever_block_numbers");
    if(takenbuffers_next > 0) {
        elog(ERROR, "takenbuffers_next > 0 %d", takenbuffers_next);
    }
    for(int i = 0; i < HEADER_FOR_EXTERNAL_RETRIEVER.num_vectors; i++) {
        wal_retriever_block_numbers[ i ] = InvalidBlockNumber;
    }
}

void ldb_wal_retriever_area_reset()
{
#if LANTERNDB_COPYNODES
    wal_retriever_area_offset = 0;
#else
    for(int i = 0; i < TAKENBUFFERS_MAX; i++) {
        if(takenbuffers[ i ] == InvalidBuffer) {
            continue;
        }
        UnlockReleaseBuffer(takenbuffers[ i ]);
        takenbuffers[ i ] = InvalidBuffer;
    }
    takenbuffers_next = 0;
#endif
}

void ldb_wal_retriever_area_free()
{
    pfree(wal_retriever_block_numbers);
    wal_retriever_block_numbers = NULL;
#if LANTERNDB_COPYNODES
    pfree(wal_retriever_area);
    wal_retriever_area = NULL;
    wal_retriever_area_size = 0;
    wal_retriever_area_offset = 0;
#endif
    for(int i = 0; i < TAKENBUFFERS_MAX; i++) {
        if(takenbuffers[ i ] == InvalidBuffer) {
            continue;
        }
        UnlockReleaseBuffer(takenbuffers[ i ]);
        takenbuffers[ i ] = InvalidBuffer;
    }
    takenbuffers_next = 0;
}

static inline void *wal_index_node_retriever_sequential(int id)
{
    BlockNumber                blockno = 1;
    Buffer                     buf;
    Page                       page;
    HnswIndexPageSpecialBlock *special_block;
    HnswIndexPage             *nodepage;
    OffsetNumber               offset, max_offset;
    // static cnt = 0;

    elog(INFO, "Hnsw_disk: ldb_wal_index_node_retriever called with id %d", id);

    while(BlockNumberIsValid(blockno)) {
        buf = ReadBufferExtended(INDEX_RELATION_FOR_RETRIEVER, MAIN_FORKNUM, blockno, RBM_NORMAL, NULL);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);
        max_offset = PageGetMaxOffsetNumber(page);

        for(offset = FirstOffsetNumber; offset <= max_offset; offset = OffsetNumberNext(offset)) {
            nodepage = (HnswIndexPage *)PageGetItem(page, PageGetItemId(page, offset));
            if(nodepage->id == id) {
#if LANTERNDB_COPYNODES
                if(wal_retriever_area == NULL
                   || wal_retriever_area_offset + nodepage->size > wal_retriever_area_size) {
                    elog(ERROR,
                         "ERROR: wal_retriever_area "
                         "is NULL or full");
                }
                memcpy(wal_retriever_area + wal_retriever_area_offset, nodepage->node, nodepage->size);
                wal_retriever_area_offset += nodepage->size;
                UnlockReleaseBuffer(buf);
                return wal_retriever_area + wal_retriever_area_offset - nodepage->size;
#else
                UnlockReleaseBuffer(buf);
                elog(ERROR, "Nocopy is unimplemented for sequential index node retriever");
#endif
            }
        }
        special_block = (HnswIndexPageSpecialBlock *)PageGetSpecialPointer(page);
        blockno = special_block->nextblockno;
        UnlockReleaseBuffer(buf);
    }
    elog(ERROR, "reached end of retriever without finding node %d", id);
}

static void *wal_index_node_retriever_binary(int id)
{
    BlockNumber                lo = 1;
    BlockNumber                hi = HEADER_FOR_EXTERNAL_RETRIEVER.num_blocks;
    Buffer                     buf;
    Page                       page;
    HnswIndexPageSpecialBlock *special_block;
    HnswIndexPage             *nodepage;
    OffsetNumber               offset, max_offset;
    if(id > HEADER_FOR_EXTERNAL_RETRIEVER.num_vectors) {
        elog(ERROR, "id %d is out of range(0, %d)", id, HEADER_FOR_EXTERNAL_RETRIEVER.num_vectors);
    }
    // static cnt = 0;

    // elog(INFO,
    //      "Hnsw_disk: ldb_wal_index_node_retriever called with id %ld %d %d", id,
    //      cnt++, HEADER_FOR_EXTERNAL_RETRIEVER.num_blocks);

    while(lo <= hi) {
        // elog(INFO, "binary searching for %ld %d %d", id, lo , hi);
        BlockNumber mid = (hi + lo) / 2;
        buf = ReadBufferExtended(INDEX_RELATION_FOR_RETRIEVER, MAIN_FORKNUM, mid, RBM_NORMAL, NULL);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);
        special_block = (HnswIndexPageSpecialBlock *)PageGetSpecialPointer(page);

        //clang-format off...ahh does not work
        if(!(special_block->firstId <= id && id <= special_block->lastId)) {
            if(special_block->firstId > id) {
                /* ---lo--------************mid*************------hi--- */
                /* --------id---[firstId-------------lastId]----------- */

                hi = mid - 1;
            } else if(special_block->lastId < id) {
                /* ---lo--------************mid*************------hi--- */
                /* -------------[firstId-------------lastId]--id------- */
                lo = mid + 1;
            } else {
                elog(ERROR, "ERROR: should be unreachable");
            }
            UnlockReleaseBuffer(buf);
            continue;
        }
        /* ---lo--------************mid*************------hi--- */
        /* -------------[firstId-------id----lastId]----------- */

        max_offset = PageGetMaxOffsetNumber(page);

        for(offset = FirstOffsetNumber; offset <= max_offset; offset = OffsetNumberNext(offset)) {
            nodepage = (HnswIndexPage *)PageGetItem(page, PageGetItemId(page, offset));
            if(nodepage->id == id) {
#if LANTEERNDB_COPYNODES
                if(wal_retriever_area == NULL || wal_retriever_area_offset + nodepage->size > wal_retriever_area_size) {
                    elog(ERROR,
                         "ERROR: wal_retriever_area "
                         "is NULL or full");
                }
                memcpy(wal_retriever_area + wal_retriever_area_offset, nodepage->node, nodepage->size);
                wal_retriever_area_offset += nodepage->size;
                UnlockReleaseBuffer(buf);
                return wal_retriever_area + wal_retriever_area_offset - nodepage->size;
#else
                UnlockReleaseBuffer(buf);
                elog(ERROR, "Nocopy is unimplemented for sequential index node retriever");
#endif
            }
        }
        UnlockReleaseBuffer(buf);
        elog(ERROR, "AAAA found a candidate block but id %d was not in there", id);
    }
    elog(ERROR, "reached end of retriever without finding node %d", id);
}

static inline void *wal_index_node_retriever_exact(int id)
{
    HnswBlockmapPage *blockmap_page;
    BlockNumber    blockmapno = HEADER_FOR_EXTERNAL_RETRIEVER.blockno_index_start + id / HNSW_BLOCKMAP_BLOCKS_PER_PAGE;
    BlockNumber    blockno;
    Buffer         buf;
    Page           page;
    HnswIndexPage *nodepage;
    OffsetNumber   offset, max_offset;
#if LANTERNDB_USEARCH_LEVEL_DISTRIBUTION
    static levels[ 20 ] = {0};
    static cnt = 0;

    // clang-format off
    if (cnt % 100 == 0) {
        elog(INFO, "levels0 %d %d %d %d %d %d %d %d %d %d", levels[0], levels[1], levels[2], levels[3], levels[4],
        levels[5], levels[6], levels[7], levels[8], levels[9]);
        elog(INFO, "levels1 %d %d %d %d %d %d %d %d %d %d",
        levels[10], levels[11], levels[12], levels[13], levels[14], levels[15], levels[16], levels[17], levels[18],
        levels[19]);
    }
#endif

    if(wal_retriever_block_numbers[ id ] != InvalidBlockNumber) {
        blockno = wal_retriever_block_numbers[ id ];
    } else {
        int id_offset = (id / HNSW_BLOCKMAP_BLOCKS_PER_PAGE) * HNSW_BLOCKMAP_BLOCKS_PER_PAGE;
        int write_size = sizeof(blockmap_page->blocknos);
        if(id_offset + HNSW_BLOCKMAP_BLOCKS_PER_PAGE > HEADER_FOR_EXTERNAL_RETRIEVER.num_vectors) {
            write_size = (HEADER_FOR_EXTERNAL_RETRIEVER.num_vectors - id_offset) * sizeof(blockmap_page->blocknos[ 0 ]);
        }
        buf = ReadBufferExtended(INDEX_RELATION_FOR_RETRIEVER, MAIN_FORKNUM, blockmapno, RBM_NORMAL, NULL);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);
        max_offset = PageGetMaxOffsetNumber(page);

        if(max_offset != FirstOffsetNumber) {
            elog(ERROR, "ERROR: Blockmap max_offset is %d but was supposed to be %d", max_offset, FirstOffsetNumber);
        }
        blockmap_page = (HnswBlockmapPage *)PageGetItem(page, PageGetItemId(page, FirstOffsetNumber));
        blockno = blockmap_page->blocknos[ id % HNSW_BLOCKMAP_BLOCKS_PER_PAGE ];
        memcpy(wal_retriever_block_numbers + (id / HNSW_BLOCKMAP_BLOCKS_PER_PAGE) * HNSW_BLOCKMAP_BLOCKS_PER_PAGE,
               blockmap_page->blocknos,
               write_size);
        UnlockReleaseBuffer(buf);
    }

    buf = ReadBufferExtended(INDEX_RELATION_FOR_RETRIEVER, MAIN_FORKNUM, blockno, RBM_NORMAL, NULL);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);
    max_offset = PageGetMaxOffsetNumber(page);

    for(offset = FirstOffsetNumber; offset <= max_offset; offset = OffsetNumberNext(offset)) {
        nodepage = (HnswIndexPage *)PageGetItem(page, PageGetItemId(page, offset));
        if(nodepage->id == id) {
#if LANTERNDB_USEARCH_LEVEL_DISTRIBUTION
            levels[ nodepage->level ]++;
#endif
#if LANTERNDB_COPYNODES
            if(wal_retriever_area == NULL || wal_retriever_area_offset + nodepage->size > wal_retriever_area_size) {
                elog(ERROR,
                     "ERROR: wal_retriever_area "
                     "is NULL or full");
            }
            memcpy(wal_retriever_area + wal_retriever_area_offset, nodepage->node, nodepage->size);
            wal_retriever_area_offset += nodepage->size;
            UnlockReleaseBuffer(buf);
            return wal_retriever_area + wal_retriever_area_offset - nodepage->size;
#else
            if(takenbuffers[ takenbuffers_next ] != InvalidBuffer) {
                UnlockReleaseBuffer(takenbuffers[ takenbuffers_next ]);
                takenbuffers[ takenbuffers_next ] = InvalidBuffer;
            }
            takenbuffers[ takenbuffers_next ] = buf;
            takenbuffers_next++;

            if(takenbuffers_next == TAKENBUFFERS_MAX) {
                if(takenbuffers[ 0 ] != InvalidBuffer) {
                    UnlockReleaseBuffer(takenbuffers[ 0 ]);
                    takenbuffers[ 0 ] = InvalidBuffer;
                }
                takenbuffers_next = 0;
            }
            return nodepage->node;
#endif
        }
    }
    UnlockReleaseBuffer(buf);
    elog(ERROR, "reached end of retriever without finding node %d", id);
}

void *ldb_wal_index_node_retriever(int id)
{
    return wal_index_node_retriever_exact(id);
    // return wal_index_node_retriever_sequential(id);
    // return wal_index_node_retriever_binary(id);
}

static inline void fill_blockno_mapping()
{
    BlockNumber                blockno = 1;
    Buffer                     buf;
    Page                       page;
    HnswIndexPageSpecialBlock *special_block;
    HnswIndexPage             *nodepage;
    OffsetNumber               offset, max_offset;

    while(BlockNumberIsValid(blockno)) {
        buf = ReadBufferExtended(INDEX_RELATION_FOR_RETRIEVER, MAIN_FORKNUM, blockno, RBM_NORMAL, NULL);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        page = BufferGetPage(buf);
        max_offset = PageGetMaxOffsetNumber(page);
        // todo:: do I set special_block->nextblockno of the last block correctly?

        for(offset = FirstOffsetNumber; offset <= max_offset; offset = OffsetNumberNext(offset)) {
            nodepage = (HnswIndexPage *)PageGetItem(page, PageGetItemId(page, offset));
            if(wal_retriever_block_numbers[ nodepage->id ] != 0) {
                elog(ERROR,
                     "ldb_wal_retriever_area_init: duplicate id %d at offset %d. prev. blockno: %d, new blockno: %d",
                     nodepage->id,
                     offset,
                     wal_retriever_block_numbers[ nodepage->id ],
                     blockno);
            }
            wal_retriever_block_numbers[ nodepage->id ] = blockno;
        }
        special_block = (HnswIndexPageSpecialBlock *)PageGetSpecialPointer(page);
        blockno = special_block->nextblockno;
        UnlockReleaseBuffer(buf);
    }
}