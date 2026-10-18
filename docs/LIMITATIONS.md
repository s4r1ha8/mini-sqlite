# Mini-SQLite Known Limitations & Design Boundaries

Mini-SQLite is designed as an educational, SQLite-inspired relational database management system implemented from scratch in C++17. To keep the codebase comprehensible and focused on core relational algorithms, specific trade-offs, scope boundaries, and architectural simplifications were intentionally made.

This document classifies every known limitation into one of three categories:
- **[Unsupported Functionality]**: Features not implemented in the current scope.
- **[Architectural Simplification]**: Deliberate design decisions prioritizing clarity and reliability over complexity.
- **[Performance Limitation]**: Implementation choices that prioritize algorithmic simplicity over maximum throughput.

---

## 1. Concurrency & Multi-Tenancy

| Area | Description | Classification |
| :--- | :--- | :--- |
| **Exclusive Process Locking** | The database file and WAL log are opened with exclusive operating system file locks (`_SH_DENYRW` on Windows). Only one process may access the database at a time. | **[Architectural Simplification]** |
| **No Concurrent Readers/Writers** | Concurrent read transactions while a write transaction is active are not supported. | **[Architectural Simplification]** |
| **No MVCC** | Multi-Version Concurrency Control is not implemented; transactions operate via exclusive single-writer locks and physical before-images. | **[Unsupported Functionality]** |

---

## 2. Transactions & Atomicity

| Area | Description | Classification |
| :--- | :--- | :--- |
| **No Savepoints or Nested Blocks** | Nested transactions (`SAVEPOINT`, `RELEASE`, `ROLLBACK TO`) are not supported. Only a single flat transaction context exists. | **[Unsupported Functionality]** |
| **Whole-Transaction Abort** | When a statement encounters an execution or storage exception (e.g. duplicate key or page capacity limit), the transaction transitions to `ABORTED`. Partial statement rollback with continued execution is not supported; the user must issue `ROLLBACK`. | **[Architectural Simplification]** |
| **O(N) Page Snapshotting** | Starting a transaction initializes page tracking across active pages. Under large page counts, memory usage grows proportionally with modified page count. | **[Performance Limitation]** |

---

## 3. Storage & B+ Tree Management

| Area | Description | Classification |
| :--- | :--- | :--- |
| **No Underflow Rebalancing / Merging** | While node splitting is fully implemented for B+ tree growth, underflow rebalancing (borrowing keys or merging adjacent underflowed nodes) is not implemented. | **[Architectural Simplification]** |
| **No Page Reclamation on Delete** | When records are deleted, their slots are freed within the page, but empty leaf pages remain in the sibling pointer chain and are not deallocated or returned to a free list. | **[Architectural Simplification]** |
| **In-Place UPDATE Expansion Limit** | Updating a record with longer text data rewrites the cell in place. If the expanded row exceeds the remaining space on that leaf page, the engine throws a storage exception and aborts the transaction rather than performing a page split on update. | **[Architectural Simplification]** |
| **Page Cache Eviction** | The `Pager` caches pages in memory without an LRU or clock eviction policy. The working set resides in memory until process termination or checkpoint. | **[Performance Limitation]** |

---

## 4. Indexing & Query Execution

| Area | Description | Classification |
| :--- | :--- | :--- |
| **Secondary Index String Hash Encoding** | String values indexed in secondary B+ trees are encoded using 64-bit FNV hashes. Point equality lookups (`WHERE col = 'text'`) use `IndexScan`, but lexical range lookups (`WHERE col > 'text'`) are not supported via index and fall back to sequential scans. | **[Architectural Simplification]** |
| **In-Memory Nested-Loop Join** | `NestedLoopJoin` buffers rows from the outer relation in memory during execution. Hash joins and sort-merge joins are not implemented. | **[Performance Limitation]** |
| **In-Memory Sort & Aggregate** | `ORDER BY`, `GROUP BY`, and aggregation buffer matching tuples in memory. Spill-to-disk external merge sort is not implemented. | **[Performance Limitation]** |
| **No OFFSET Clause** | While `LIMIT` is fully supported, the SQL dialect does not implement `OFFSET`. | **[Unsupported Functionality]** |
| **No Arithmetic Expressions** | Column expressions do not support arithmetic operators (e.g., `SELECT price * quantity` or `WHERE age + 1 > 20`). | **[Unsupported Functionality]** |

---

## 5. Schema & System Limits

| Area | Description | Classification |
| :--- | :--- | :--- |
| **Single-Page Catalog (Page 0)** | All table definitions, column types, and index metadata are serialized into Page 0 (4096 bytes). Large schemas exceeding ~30-40 tables and indexes will exceed this page limit. | **[Architectural Simplification]** |
| **Line-Oriented REPL Input** | The interactive terminal driver (`main.cpp`) reads input line-by-line (`std::getline`). Multiline SQL statements must be entered as single lines or fed through the programmatic C++ API. | **[Architectural Simplification]** |
| **Platform-Specific Locking Primitives** | Process locking currently uses Windows `_fsopen` and `_SH_DENYRW` primitives. Cross-platform POSIX `fcntl` locking is not yet abstracted. | **[Platform Constraint]** |
