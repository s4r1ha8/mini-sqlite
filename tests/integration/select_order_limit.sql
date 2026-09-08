-- ============================================================
-- select_order_limit.sql
-- Expected: correctly ordered rows, limited rows
-- ============================================================

CREATE TABLE sort_test (id INTEGER PRIMARY KEY, val INTEGER);
INSERT INTO sort_test VALUES (1, 10);
INSERT INTO sort_test VALUES (2, 5);
INSERT INTO sort_test VALUES (3, 7);
INSERT INTO sort_test VALUES (4, 10);
INSERT INTO sort_test VALUES (5, 1);

-- Basic ASC
SELECT * FROM sort_test ORDER BY val;

-- Basic DESC
SELECT * FROM sort_test ORDER BY val DESC;

-- Multiple order by columns
SELECT * FROM sort_test ORDER BY val DESC, id ASC;

-- LIMIT basic
SELECT * FROM sort_test LIMIT 2;

-- LIMIT 0
SELECT * FROM sort_test LIMIT 0;

-- ORDER BY with LIMIT
SELECT * FROM sort_test ORDER BY val ASC LIMIT 3;


