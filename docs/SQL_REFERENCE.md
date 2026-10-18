# Mini-SQLite SQL Reference Manual

This document provides complete syntax specifications, explanations, examples, and limitations for the SQL dialect implemented by Mini-SQLite.

---

## Data Types

Mini-SQLite supports three core data types:

| Type | Description | Internal Representation |
| :--- | :--- | :--- |
| `INTEGER` | 64-bit signed integer | `int64_t` (8 bytes) |
| `REAL` | 64-bit IEEE-754 floating point number | `double` (8 bytes) |
| `TEXT` | UTF-8 encoded string literal | `std::string` (variable length) |
| `NULL` | Null literal representation | `std::monostate` |

---

## Data Definition Language (DDL)

### 1. `CREATE TABLE`
Creates a new relational table in the database catalog and allocates its initial clustered B+ tree root page.

- **Syntax:**
  ```sql
  CREATE TABLE table_name (
      column_name data_type [PRIMARY KEY] [NOT NULL]
      [, column_name data_type [PRIMARY KEY] [NOT NULL]]*
  );
  ```
- **Example:**
  ```sql
  CREATE TABLE users (
      id INTEGER PRIMARY KEY,
      name TEXT,
      age INTEGER NOT NULL
  );
  ```
- **Limitations:**
  - Exactly one column must be designated as `PRIMARY KEY`.
  - The `PRIMARY KEY` column must be of type `INTEGER`.
  - Table and column names must be alphanumeric identifiers.

---

### 2. `CREATE INDEX`
Creates a secondary B+ tree index on an existing table column.

- **Syntax:**
  ```sql
  CREATE INDEX index_name ON table_name (column_name);
  ```
- **Example:**
  ```sql
  CREATE INDEX idx_users_age ON users (age);
  ```
- **Limitations:**
  - Indexes are single-column only (composite index definitions are not supported).
  - String values in secondary indexes are hash-encoded, enabling point equality lookups (`col = 'val'`) but not lexical range scans (`col > 'val'`).

---

## Data Manipulation Language (DML)

### 3. `INSERT`
Inserts a new tuple into a table and updates associated secondary indexes.

- **Syntax:**
  ```sql
  INSERT INTO table_name VALUES (literal_1, literal_2, ...);
  ```
- **Example:**
  ```sql
  INSERT INTO users VALUES (1, 'Alice', 25);
  INSERT INTO users VALUES (2, 'Bob', 30);
  ```
- **Limitations:**
  - Positional value lists only; explicit column lists (e.g. `INSERT INTO t (col) VALUES (...)`) are not supported.
  - Primary key values must be unique. Duplicate keys throw an exception and roll back the statement.

---

### 4. `UPDATE`
Updates existing records in a table matching an optional predicate.

- **Syntax:**
  ```sql
  UPDATE table_name SET column_name = literal [, column_name = literal]* [WHERE condition];
  ```
- **Example:**
  ```sql
  UPDATE users SET age = 26 WHERE id = 1;
  UPDATE users SET name = 'Robert', age = 31 WHERE id = 2;
  ```
- **Limitations:**
  - Modifying primary keys (`SET id = ...`) is supported; if a duplicate primary key is generated, execution halts and the transaction aborts.
  - In-place row expansion that exceeds the leaf page's slot capacity raises a controlled error and aborts the transaction.

---

### 5. `DELETE`
Deletes records from a table matching an optional predicate and removes corresponding secondary index entries.

- **Syntax:**
  ```sql
  DELETE FROM table_name [WHERE condition];
  ```
- **Example:**
  ```sql
  DELETE FROM users WHERE id = 2;
  DELETE FROM users; -- Deletes all rows
  ```
- **Limitations:**
  - Deleting rows removes index keys, but empty leaf pages are not merged or reclaimed on disk.

---

## Query Language (DQL)

### 6. `SELECT`
Queries data from one or more tables using projection, filtering, joining, grouping, aggregation, sorting, and limiting.

- **Syntax:**
  ```sql
  SELECT [table.]column [, [table.]column]* | * | aggregate_expr [, ...]
  FROM table_name [alias]
  [JOIN table_name [alias] ON join_condition]*
  [WHERE filter_condition]
  [GROUP BY group_column [, group_column]*]
  [HAVING having_condition]
  [ORDER BY order_column [ASC | DESC]]
  [LIMIT count];
  ```

#### A. Basic Selection & Projection
```sql
SELECT * FROM users;
SELECT name, age FROM users;
SELECT users.id, users.name FROM users;
```

#### B. WHERE Filtering
Supports comparison operators (`=`, `!=`, `<`, `>`, `<=`, `>=`) and logical operators (`AND`, `OR`, `NOT`).
```sql
SELECT * FROM users WHERE age >= 21;
SELECT * FROM users WHERE age > 20 AND name = 'Alice';
SELECT * FROM users WHERE NOT (age < 18);
```
*Note: Equality lookups on indexed columns automatically utilize `IndexScan` instead of `SeqScan`.*

#### C. Multi-Table JOIN
Executes an inner nested-loop join across two tables.
```sql
SELECT users.name, orders.total
FROM users JOIN orders ON users.id = orders.user_id;
```
*Limitations: Only inner joins with explicit `ON` conditions are supported. Outer joins (`LEFT`, `RIGHT`, `FULL`) are not supported.*

#### D. Aggregation & GROUP BY
Supported aggregate functions: `COUNT(*)`, `COUNT(column)`, `SUM(column)`, `AVG(column)`, `MIN(column)`, `MAX(column)`.
```sql
SELECT COUNT(*) FROM users;
SELECT AVG(age), MIN(age), MAX(age) FROM users;
SELECT dept_id, COUNT(*), SUM(salary) FROM employees GROUP BY dept_id;
```
*Limitations: Non-aggregate expressions in the projection list must be present in the `GROUP BY` clause.*

#### E. HAVING Clause
Filters aggregated groups produced by `GROUP BY`.
```sql
SELECT dept_id, COUNT(*)
FROM employees
GROUP BY dept_id
HAVING COUNT(*) > 5;
```

#### F. ORDER BY
Sorts the result set by one or more expressions in ascending (`ASC`, default) or descending (`DESC`) order.
```sql
SELECT name, age FROM users ORDER BY age DESC;
SELECT name, age FROM users ORDER BY age ASC;
```

#### G. LIMIT
Restricts the maximum number of rows returned.
```sql
SELECT * FROM users ORDER BY age DESC LIMIT 5;
```
*Limitations: Negative limits are rejected at parse time. `OFFSET` is not supported.*

---

## Transaction Management

Mini-SQLite provides full ACID multi-statement transactions.

### 7. `BEGIN`
Begins an explicit multi-statement transaction block.
```sql
BEGIN;
```

### 8. `COMMIT`
Durably writes all accumulated changes across tables and indexes to the WAL and database file.
```sql
COMMIT;
```

### 9. `ROLLBACK`
Discards all modifications made since `BEGIN`, restoring table and index states from in-memory before-image snapshots.
```sql
ROLLBACK;
```

*Note on Transaction Abort: If any statement throws an error inside an explicit transaction, the transaction enters an `ABORTED` state. Subsequent statements and `COMMIT` are rejected until `ROLLBACK` is issued.*

---

## REPL Commands (Dot-Commands)

The interactive terminal supports special dot-commands for introspection:

| Command | Description | Example |
| :--- | :--- | :--- |
| `.tables` | Lists all tables registered in the database catalog. | `.tables` |
| `.schema <table_name>` | Displays column names, types, constraints, and indexes for a table. | `.schema users` |
| `.exit` | Closes open database handles and exits the REPL cleanly. | `.exit` |
