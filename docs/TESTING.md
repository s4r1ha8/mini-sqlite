# Mini-SQLite Automated Testing & Verification Suite

Mini-SQLite incorporates a multi-tiered automated test suite covering unit tests, query-plan runtime tests, crash-recovery and durability regressions, process locking isolation, and end-to-end SQL integration scripts.

---

## 1. Verified Release Test Baseline

Every test suite has been executed and verified clean on the release baseline:

| Test Suite | Scope | Target Executable / Script | Passed | Pass Rate |
| :--- | :--- | :--- | :---: | :---: |
| **CTest Unit Suite** | Core subsystems & frontend | `ctest` | **7 / 7** | 100% |
| **Durability & Abort** | WAL crash recovery & transaction state | `test_durability.exe` | **15 / 15** | 100% |
| **Runtime Integration** | Volcano operator plans | `integration_runtime_test.exe` | **6 / 6** | 100% |
| **SQL Integration** | End-to-end SQL scripts | `tests/run_integration.ps1` | **9 / 9** | 100% |
| **Process Locking** | OS file locking & crash safety | `test_locking.exe` / `run_locking_test.ps1` | **5 / 5** | 100% |

*Note: These suites provide overlapping verification across different architectural layers (for example, CTest invokes `test_durability`). They represent distinct validation perspectives rather than mutually exclusive test cases.*

---

## 2. Test Suite Details

### A. CTest Unit Suite (7/7)
Managed through CMake's CTest framework:
1. **`test_tokenizer` (`tests/test_tokenizer.cpp`):**
   - Validates lexical scanning, keyword recognition, numeric/string literals, operators, and comment stripping.
2. **`test_parser` (`tests/test_parser.cpp`):**
   - Tests recursive-descent parsing into AST structures across DDL, DML, joins, aggregates, expressions, and transactions.
3. **`test_catalog` (`tests/test_catalog.cpp`):**
   - Verifies in-memory catalog registration, duplicate table/index prevention, column metadata, and Page 0 serialization/deserialization.
4. **`test_sema` (`tests/test_sema.cpp`):**
   - Tests semantic validation passes: table/column existence checks, type compatibility, primary key enforcement, and invalid aggregate placement.
5. **`test_planner` (`tests/test_planner.cpp`):**
   - Verifies physical plan generation: index selection on equality predicates, join tree formation, aggregate wrapping, and sorting/limiting.
6. **`test_storage` (`tests/test_storage.cpp`):**
   - Tests lower-level storage: 4KB page allocation, slotted-page headers, B+ tree node splitting, row serialization, and table cursor traversal.
7. **`test_durability` (`tests/test_durability.cpp`):**
   - Executes the 15 specialized transaction durability and crash-recovery scenarios.

---

### B. Durability & Transaction Abort Regression (15/15)
Executed directly via `test_durability.exe`:
1. `test_insert_recovery`: Replays committed `INSERT` operations from the WAL onto clean database pages after simulated crash.
2. `test_update_recovery`: Replays committed `UPDATE` operations from the WAL.
3. `test_delete_recovery`: Replays committed `DELETE` operations from the WAL.
4. `test_uncommitted_update_undo`: Verifies that uncommitted transactions are never written to the database file (NO-STEAL policy) and cleanly discarded during recovery.
5. `test_multi_statement_transaction`: Verifies atomicity across transactions containing multiple heterogeneous statements.
6. `test_indexed_delete_recovery`: Ensures secondary index B+ trees are correctly updated and restored across deletions and recovery.
7. `test_btree_root_split_recovery`: Verifies table B+ tree page splits and root allocation under heavy insertion during recovery.
8. `test_indexed_split_recovery`: Verifies secondary index B+ tree splits during recovery.
9. `test_indexed_update_crash_test`: Verifies crash recovery on secondary index updates with duplicate keys.
10. `test_explicit_txn_abort_reproduction`: Verifies the explicit transaction state machine transitions to `ABORTED` upon statement error.
11. `test_auto_txn_failure_preserves_state`: Verifies automatic single-statement rollback when an error occurs outside `BEGIN`.
12. `test_explicit_txn_success_commit`: Verifies healthy explicit transactions commit cleanly.
13. `test_explicit_txn_failing_update_and_rollback`: Verifies that issuing `ROLLBACK` after a failed statement restores the exact before-image snapshot.
14. `test_explicit_txn_failing_update_attempt_commit`: Verifies that attempting to `COMMIT` an aborted transaction is strictly rejected.
15. `test_explicit_txn_failing_update_attempt_select_then_rollback`: Verifies that subsequent queries are blocked in an aborted transaction until `ROLLBACK` is issued.

---

### C. Runtime Plan Integration (6/6)
Executed via `integration_runtime_test.exe`:
1. `SELECT * FROM users WHERE name = 'Bob'` → Validates `IndexScan` execution against secondary index.
2. `SELECT COUNT(*) FROM users` → Validates whole-table `COUNT(*)` aggregation.
3. `SELECT COUNT(id) FROM users` → Validates non-null column aggregation.
4. `SELECT SUM(score), AVG(score), MIN(score), MAX(score) FROM users` → Validates multi-aggregate vector computation.
5. `SELECT name, COUNT(*) FROM users GROUP BY name` → Validates `GROUP BY` hashing and grouping.
6. `SELECT user_id, COUNT(*) FROM orders GROUP BY user_id HAVING COUNT(*) > 1` → Validates `GROUP BY` with post-aggregate `HAVING` filtering.

---

### D. End-to-End SQL Integration Scripts (9/9)
Pipes raw SQL scripts through `mini_sqlite.exe` in isolated test directories:
1. `ddl.sql`: Creates tables, secondary indexes, verifies `.tables` and `.schema`.
2. `select_basic.sql`: Validates single-table projections, index equality scans, and sequential filters.
3. `select_join.sql`: Validates multi-table inner joins.
4. `select_agg.sql`: Validates complex aggregation, `GROUP BY`, and `HAVING`.
5. `errors.sql`: Verifies that syntax, semantic, duplicate table, and missing column errors are handled gracefully without engine crash.
6. `delete.sql`: Validates record deletions and secondary index synchronization.
7. `update.sql`: Validates single/multi-column updates and duplicate key indexing.
8. `txn.sql`: Validates statement atomicity rollback on duplicate primary key conflict and transaction commit/rollback.
9. `select_order_limit.sql`: Validates `ORDER BY` sorting and `LIMIT` record count restriction.

---

### E. Process Locking Regression (5/5)
Executed via `test_locking.exe`:
1. **Lock Acquisition:** First process acquires exclusive database file lock (`_SH_DENYRW`).
2. **Mutual Exclusion:** Second process attempting concurrent access receives `ERROR_LOCKED`.
3. **Crash Cleanup:** Forcible termination of holding process automatically releases OS file lock.
4. **Clean Reopening:** Subsequent process opens and initializes the database successfully.
5. **Partial Open Rollback:** If locking the WAL file fails after opening the DB file, the engine cleanly closes the DB file handle, leaving no orphaned locks.

---

## 3. Verified Build Environment

The test baseline was compiled and verified under:
- **Operating System:** Windows 11 x64
- **Compiler:** Microsoft Visual C++ Compiler (MSVC) 19.44.35228.0 (C++17)
- **IDE / Build Tools:** Visual Studio 2022 Build Tools (v170)
- **Windows SDK:** 10.0.26100.0
- **CMake Version:** 3.14+
