-- F4.2 Statement Atomicity
-- F4.2 Statement Atomicity
CREATE TABLE t1 (id INTEGER PRIMARY KEY, val INTEGER);
INSERT INTO t1 VALUES (1, 10);
INSERT INTO t1 VALUES (2, 20);
INSERT INTO t1 VALUES (3, 30);

-- This should fail on the second row due to PK conflict, and roll back the whole statement
-- It will try to update (1,10)->(99,10) which succeeds, then (2,20)->(99,20) which fails
UPDATE t1 SET id = 99;
SELECT * FROM t1;

-- F4.3 Explicit Transactions (multi-statement restore)
BEGIN;
UPDATE t1 SET val = 100 WHERE id = 1;
DELETE FROM t1 WHERE id = 2;
ROLLBACK;
SELECT * FROM t1; -- Should be (1,10), (2,20), (3,30)

BEGIN;
UPDATE t1 SET val = 100 WHERE id = 1;
DELETE FROM t1 WHERE id = 2;
COMMIT;
SELECT * FROM t1; -- Should be (1,100), (3,30)

-- F4.5 PK Update Transactions
BEGIN;
UPDATE t1 SET id = 99 WHERE id = 3;
ROLLBACK;
SELECT * FROM t1; -- Should be (1,100), (3,30)

BEGIN;
UPDATE t1 SET id = 99 WHERE id = 3;
COMMIT;
SELECT * FROM t1; -- Should be (1,100), (99,30)

-- Failure mid-transaction marks transaction aborted; commands rejected until ROLLBACK
BEGIN;
INSERT INTO t1 VALUES (5, 50);
INSERT INTO t1 VALUES (99, 999); -- Fails due to PK 99
INSERT INTO t1 VALUES (6, 60); -- Rejected: txn is aborted
COMMIT; -- Rejected: cannot commit aborted txn
ROLLBACK;
SELECT * FROM t1; -- Should be (1,100), (99,30)

-- F4.6 Multi-statement index consistency
CREATE TABLE t2 (id INTEGER PRIMARY KEY, category INTEGER, age INTEGER);
CREATE INDEX idx_cat ON t2(category);
INSERT INTO t2 VALUES (1, 10, 20);
INSERT INTO t2 VALUES (2, 10, 25);
INSERT INTO t2 VALUES (3, 20, 30);

BEGIN;
UPDATE t2 SET category = 30 WHERE id = 1;
DELETE FROM t2 WHERE id = 2;
UPDATE t2 SET category = 10 WHERE id = 3;
ROLLBACK;
SELECT * FROM t2; -- Should be (1,10,20), (2,10,25), (3,20,30)
-- Re-query via index to verify index is restored
SELECT * FROM t2 WHERE category = 10; -- Should return rows 1 and 2

BEGIN;
UPDATE t2 SET category = 30 WHERE id = 1;
DELETE FROM t2 WHERE id = 2;
UPDATE t2 SET category = 10 WHERE id = 3;
COMMIT;
SELECT * FROM t2; -- Should be (1,30,20), (3,10,30)
SELECT * FROM t2 WHERE category = 10; -- Should return row 3
SELECT * FROM t2 WHERE category = 30; -- Should return row 1

.exit
