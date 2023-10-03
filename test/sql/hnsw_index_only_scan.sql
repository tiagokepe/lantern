CREATE TABLE small_world (id integer, vector real[3]);
INSERT INTO small_world (id, vector) VALUES (0, '{0,0,0}'), (1, '{0,0,1}');
CREATE INDEX ON small_world USING hnsw (vector);
SET enable_seqscan = false;
-- Testing Only Scan as Usual
EXPLAIN SELECT id, vector FROM small_world ORDER BY vector <-> ARRAY[0,0,0];
-- Case1: Testing only-index scan with only key-column index
EXPLAIN SELECT vector FROM small_world ORDER BY vector <-> ARRAY[0,0,0];
