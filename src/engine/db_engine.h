#pragma once

// ============================================================================
// db_engine.h  -  Top-level database session.
//
// Owner: Sarthak (engine / integration layer)
//
// DatabaseEngine is the single object that owns:
//   - Pager      (disk I/O)
//   - WAL        (write-ahead log)
//   - TxnManager (transaction coordination)
//   - Catalog    (in-memory schema registry)
//
// On construction it:
//   1. Opens Pager (which opens the db file)
//   2. Reserves page 0 for the catalog on a fresh database
//   3. Runs Recovery (no-op when WAL is empty)
//   4. Loads the Catalog from page 0
//
// The REPL calls execute(sql) for every SQL statement, and
// printTables() / printSchema() for dot commands.
//
// Transaction model (matching SQLite autocommit semantics):
//   - If the user hasn't issued BEGIN, each write statement is
//     automatically wrapped in its own BEGIN/COMMIT (autocommit mode).
//   - If the user typed BEGIN, all subsequent writes share that
//     transaction until COMMIT or ROLLBACK.
// ============================================================================

#include <optional>
#include <string>

#include "storage/pager.h"
#include "storage/wal.h"
#include "storage/txn_manager.h"
#include "storage/recovery.h"
#include "storage/btree.h"
#include "storage/cursor.h"
#include "storage/index_manager.h"
#include "frontend/catalog.h"
#include "frontend/sema.h"
#include "frontend/planner.h"
#include "common/ast.h"

class DatabaseEngine {
public:
    // Open (or create) the database at db_path.
    // db_path:  path to the main database file (e.g. "mini_sqlite.db")
    // wal_path: path to the WAL file (defaults to db_path + ".wal")
    explicit DatabaseEngine(
        const std::string& db_path,
        const std::string& wal_path = "");

    ~DatabaseEngine() = default;

    // Execute one SQL statement (tokenize -> parse -> sema -> dispatch).
    // Prints results / acknowledgement to stdout.
    // Throws on parse/semantic/runtime errors (caller should catch and print).
    void execute(const std::string& sql);

    // Dot commands
    void printTables() const;
    void printSchema(const std::string& table_name) const;

private:
    // --- owned storage resources -------------------------------------------
    Pager      pager_;
    WAL        wal_;
    TxnManager txn_mgr_;
    Catalog    catalog_;

    // --- transaction state --------------------------------------------------
    // Non-empty when any transaction (explicit or auto) is active.
    std::optional<TxnId> active_txn_;
    // true when the user typed BEGIN explicitly; false for auto-transactions.
    bool explicit_txn_ = false;

    // --- startup helpers ----------------------------------------------------
    void bootstrap_if_fresh();   // reserve page 0 on a brand-new database
    void recover_and_load();     // run Recovery, then load catalog from page 0

    // --- catalog persistence ------------------------------------------------
    void serialize_catalog();    // write catalog to page 0 memory
    void load_catalog();         // read catalog from page 0

    // --- transaction helpers ------------------------------------------------
    // If not in an explicit transaction, begin one automatically.
    // Returns the active transaction id.
    TxnId ensure_txn();

    // If we auto-began a transaction (not an explicit BEGIN), commit it now.
    void auto_commit();

    // --- statement dispatch -------------------------------------------------
    void executeParsed(const Statement& stmt);
    void executeCreateTable(const CreateTableStmt& stmt);
    void executeCreateIndex(const CreateIndexStmt& stmt);
    void executeInsert(const InsertStmt& stmt);
    void executeDelete(const DeleteStmt& stmt);
    void executeUpdate(const UpdateStmt& stmt);
    void executeSelect(const SelectStmt& stmt);
    void executeTxn(const TxnStmt& stmt);

    // --- internal helpers ---------------------------------------------------
    // Convert an AST Literal to a runtime Value.
    static Value literalToValue(const Literal& lit);

    // Print one row as   col1 | col2 | col3
    static void printRow(const Row& row);

    // Print a column header line given schema column names.
    static void printHeader(const std::vector<std::string>& names);
};
