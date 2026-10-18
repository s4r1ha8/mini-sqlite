CREATE TABLE del_test (id INTEGER, val INTEGER, name TEXT);
CREATE INDEX del_test_val ON del_test (val);

INSERT INTO del_test VALUES (1, 10, 'A');
INSERT INTO del_test VALUES (2, 20, 'B');
INSERT INTO del_test VALUES (3, 20, 'C');
INSERT INTO del_test VALUES (4, 30, 'D');

-- delete one row
DELETE FROM del_test WHERE id = 1;
SELECT * FROM del_test;
-- verify index
SELECT * FROM del_test WHERE val = 10;

-- delete multiple rows
DELETE FROM del_test WHERE val = 20;
SELECT * FROM del_test;
-- verify index
SELECT * FROM del_test WHERE val = 20;

-- delete no rows
DELETE FROM del_test WHERE id = 99;
SELECT * FROM del_test;

-- BEGIN + DELETE + ROLLBACK
BEGIN;
DELETE FROM del_test WHERE id = 4;
SELECT * FROM del_test;
ROLLBACK;
SELECT * FROM del_test;

-- BEGIN + DELETE + COMMIT
BEGIN;
DELETE FROM del_test WHERE id = 4;
COMMIT;
SELECT * FROM del_test;

-- delete all rows
DELETE FROM del_test;
SELECT * FROM del_test;

-- delete across multiple leaves / causing empty leaves
CREATE TABLE big_del (id INTEGER, val INTEGER);
CREATE INDEX big_del_val ON big_del (val);

-- insert enough rows to create multiple leaves (assuming leaf size is small, e.g., 3-4 keys)
-- actually, we just insert manually 10-15 rows
INSERT INTO big_del VALUES (1, 10);
INSERT INTO big_del VALUES (2, 20);
INSERT INTO big_del VALUES (3, 30);
INSERT INTO big_del VALUES (4, 40);
INSERT INTO big_del VALUES (5, 50);
INSERT INTO big_del VALUES (6, 60);
INSERT INTO big_del VALUES (7, 70);
INSERT INTO big_del VALUES (8, 80);
INSERT INTO big_del VALUES (9, 90);
INSERT INTO big_del VALUES (10, 100);

DELETE FROM big_del WHERE val > 40;
SELECT * FROM big_del;
SELECT * FROM big_del WHERE val = 100;

DELETE FROM big_del;
SELECT * FROM big_del;
