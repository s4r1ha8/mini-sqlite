CREATE TABLE departments (id INTEGER PRIMARY KEY, dept_name TEXT, budget REAL);
CREATE TABLE employees (id INTEGER PRIMARY KEY, name TEXT, dept_id INTEGER, salary INTEGER, active INTEGER NOT NULL);
CREATE INDEX idx_emp_dept ON employees (dept_id);
.tables
.schema departments
.schema employees
INSERT INTO departments VALUES (10, 'Engineering', 250000.0);
INSERT INTO departments VALUES (20, 'Product', 180000.0);
INSERT INTO departments VALUES (30, 'Design', 120000.0);
INSERT INTO employees VALUES (1, 'Alice', 10, 95000, 1);
INSERT INTO employees VALUES (2, 'Bob', 10, 88000, 1);
INSERT INTO employees VALUES (3, 'Charlie', 20, 82000, 1);
INSERT INTO employees VALUES (4, 'Diana', 20, 91000, 1);
INSERT INTO employees VALUES (5, 'Evan', 30, 75000, 1);
INSERT INTO employees VALUES (6, 'Fiona', 10, 105000, 1);
INSERT INTO employees VALUES (7, 'George', 30, 72000, 0);
SELECT * FROM departments;
SELECT id, name, salary FROM employees WHERE salary > 85000;
SELECT * FROM employees WHERE dept_id = 10;
SELECT name, salary FROM employees ORDER BY salary DESC LIMIT 3;
SELECT employees.name, departments.dept_name, employees.salary FROM employees JOIN departments ON employees.dept_id = departments.id;
SELECT COUNT(*) FROM employees;
SELECT dept_id, COUNT(*), SUM(salary), AVG(salary), MIN(salary), MAX(salary) FROM employees GROUP BY dept_id;
SELECT dept_id, COUNT(*) FROM employees GROUP BY dept_id HAVING COUNT(*) > 2;
UPDATE employees SET salary = 98000 WHERE id = 1;
SELECT id, name, salary FROM employees WHERE id = 1;
DELETE FROM employees WHERE active = 0;
SELECT COUNT(*) FROM employees;
BEGIN;
INSERT INTO employees VALUES (8, 'Hannah', 20, 84000, 1);
UPDATE departments SET budget = 190000.0 WHERE id = 20;
COMMIT;
SELECT id, name, dept_id FROM employees WHERE id = 8;
BEGIN;
DELETE FROM employees WHERE dept_id = 30;
SELECT COUNT(*) FROM employees WHERE dept_id = 30;
ROLLBACK;
SELECT COUNT(*) FROM employees WHERE dept_id = 30;
.exit
