// ============================================================================
// db_engine.cpp  -  DatabaseEngine implementation
// ============================================================================

#include "engine/db_engine.h"
#include "engine/catalog_serializer.h"
#include "exec/executor_adapter.h"
#include "frontend/tokenizer.h"
#include "frontend/parser.h"
#include "common/eval.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <variant>

// ── Constructor ──────────────────────────────────────────────────────────────

DatabaseEngine::DatabaseEngine(
    const std::string& db_path,
    const std::string& wal_path)
    : pager_(db_path)
    , wal_((wal_path.empty() ? db_path + ".wal" : wal_path))
    , txn_mgr_(pager_, wal_)
{
    bootstrap_if_fresh();
    recover_and_load();
}

// ── Public: execute SQL ──────────────────────────────────────────────────────

void DatabaseEngine::execute(const std::string& sql) {
    Tokenizer tok(sql);
    auto tokens = tok.tokenize();

    Parser parser(std::move(tokens));
    Statement stmt = parser.parse();

    SemanticAnalyzer sema(catalog_);
    sema.analyze(stmt);

    executeParsed(stmt);
}

// ── Public: dot commands ─────────────────────────────────────────────────────

void DatabaseEngine::printTables() const {
    const auto& tables = catalog_.all_tables();
    if (tables.empty()) {
        std::cout << "  (no tables)\n";
        return;
    }
    for (const auto& [name, schema] : tables)
        std::cout << "  " << name << "\n";
}

void DatabaseEngine::printSchema(const std::string& table_name) const {
    const TableSchema* schema = catalog_.get_table(table_name);
    if (!schema) {
        std::cout << "  No table '" << table_name << "' in catalog.\n";
        return;
    }
    std::cout << "  Table: " << schema->name << "\n";
    for (const auto& col : schema->columns) {
        std::cout << "    [" << col.ordinal << "] "
                  << col.name << " "
                  << dataTypeToString(col.type);
        if (schema->primary_key == col.name) std::cout << " PRIMARY KEY";
        if (col.is_not_null)                 std::cout << " NOT NULL";
        std::cout << "\n";
    }
    if (!schema->indexes.empty()) {
        std::cout << "  Indexes:\n";
        for (const auto& idx : schema->indexes)
            std::cout << "    " << idx.name
                      << " ON " << idx.column
                      << " (root=" << idx.root_page_id << ")\n";
    }
}

// ── Private: startup ─────────────────────────────────────────────────────────

void DatabaseEngine::bootstrap_if_fresh() {
    // On a completely new database file, page_count() == 0.
    // Claim page 0 for the catalog so that the first BTree::create()
    // gets page 1 (not page 0).
    if (pager_.page_count() == 0) {
        PageId p0 = pager_.allocate_page(); // returns 0
        (void)p0;
        // Leave the page zeroed — deserialize() will see no magic and return
        // an empty Catalog, which is correct for a fresh database.
        pager_.flush_page(0);
    }
}

void DatabaseEngine::recover_and_load() {
    // Run WAL recovery (ARIES-style redo + undo).
    // This re-applies any committed B-tree page writes and undoes any
    // uncommitted ones.  Page 0 (catalog) is NOT WAL-logged — it is
    // written directly to disk after every commit, so it is always
    // consistent after crash (last complete flush wins).
    auto [redone, undone] = Recovery::run(pager_, wal_, /*verbose=*/false);
    if (redone > 0 || undone > 0) {
        std::cout << "[Recovery] Redone: " << redone
                  << " page writes, Undone: " << undone << " page writes\n";
    }
    // Read catalog from page 0 (always the persisted authoritative copy).
    load_catalog();
}

// ── Private: catalog persistence ─────────────────────────────────────────────
//
// Page 0 is NOT logged through the WAL.  Instead it is written directly to
// disk after every successful commit.  This is safe because:
//   1.  The WAL protects B-tree pages (pages 1+).
//   2.  If we crash after writing B-tree pages but before writing page 0,
//       Recovery will redo the B-tree writes, then load the old catalog from
//       page 0.  The old catalog will have the old root_page_id, which still
//       points to valid (now recovered) data.
//   3.  If we crash after writing page 0, the catalog reflects the committed
//       state and the B-tree pages were already flushed by pager_.flush_all()
//       inside txn_mgr_.commit().
// In both cases the database is consistent.

void DatabaseEngine::save_catalog() {
    auto bytes = CatalogSerializer::serialize(catalog_);

    Page* p0 = pager_.get_page(0);
    // Zero the page first so stale bytes from a previously larger catalog
    // don't get interpreted as garbage on next load.
    p0->data.fill(std::byte{0});
    for (size_t i = 0; i < bytes.size(); i++) {
        p0->data[i] = bytes[i];
    }
    pager_.mark_dirty(0);
    pager_.flush_page(0);  // direct flush, bypassing WAL
}

void DatabaseEngine::load_catalog() {
    Page* p0 = pager_.get_page(0);
    catalog_ = CatalogSerializer::deserialize(p0->data);
}

// ── Private: transaction helpers ─────────────────────────────────────────────

TxnId DatabaseEngine::ensure_txn() {
    if (!active_txn_.has_value()) {
        active_txn_ = txn_mgr_.begin();
        explicit_txn_ = false;  // auto-started
    }
    return *active_txn_;
}

void DatabaseEngine::auto_commit() {
    // Only commit if this is an auto-started transaction.
    // If the user typed BEGIN explicitly, we do NOT commit here —
    // they must type COMMIT or ROLLBACK themselves.
    if (active_txn_.has_value() && !explicit_txn_) {
        txn_mgr_.commit(*active_txn_);
        active_txn_ = std::nullopt;
        explicit_txn_ = false;
    }
}

// ── Private: dispatch ─────────────────────────────────────────────────────────

void DatabaseEngine::executeParsed(const Statement& stmt) {
    std::visit([this](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if      constexpr (std::is_same_v<T, CreateTableStmt>) executeCreateTable(s);
        else if constexpr (std::is_same_v<T, CreateIndexStmt>) executeCreateIndex(s);
        else if constexpr (std::is_same_v<T, InsertStmt>)      executeInsert(s);
        else if constexpr (std::is_same_v<T, DeleteStmt>)      executeDelete(s);
        else if constexpr (std::is_same_v<T, UpdateStmt>)      executeUpdate(s);
        else if constexpr (std::is_same_v<T, SelectStmt>)      executeSelect(s);
        else if constexpr (std::is_same_v<T, TxnStmt>)         executeTxn(s);
    }, stmt);
}

// ── CREATE TABLE ─────────────────────────────────────────────────────────────
//
// Execution order:
//   1. Begin auto-transaction
//   2. BTree::create() — allocates root page
//   3. Log root page via TxnManager (WAL)
//   4. Update in-memory catalog root_page_id
//   5. Commit (flushes B-tree page to disk via flush_all)
//   6. Save catalog to page 0 (direct flush, outside WAL)

void DatabaseEngine::executeCreateTable(const CreateTableStmt& stmt) {
    TableSchema* schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
        throw std::runtime_error(
            "CREATE TABLE: sema did not add schema for '" + stmt.table_name + "'");
    }

    // 1. Auto-begin
    TxnId tid = ensure_txn();

    // 2. Allocate B-tree root (gets page 1+ because page 0 is the catalog)
    PageId root = BTree::create(pager_);

    // 3. Log the root page via TxnManager
    Page* root_page = pager_.get_page(root);
    txn_mgr_.write_page(tid, root, root_page->data);

    // 4. Store root_page_id in catalog
    schema->root_page_id = static_cast<int>(root);

    // 5. Commit (WAL COMMIT record + pager flush_all)
    auto_commit();

    // 6. Save catalog to page 0 (direct, outside WAL)
    save_catalog();

    std::cout << "[DDL] CREATE TABLE " << stmt.table_name
              << " (root_page=" << root << ")\n";
}

// ── CREATE INDEX ──────────────────────────────────────────────────────────────

void DatabaseEngine::executeCreateIndex(const CreateIndexStmt& stmt) {
    TableSchema* schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
        throw std::runtime_error(
            "CREATE INDEX: table '" + stmt.table_name + "' not in catalog");
    }

    IndexInfo* idx_info = nullptr;
    for (auto& idx : schema->indexes) {
        if (idx.name == stmt.index_name) { idx_info = &idx; break; }
    }
    if (!idx_info) {
        throw std::runtime_error(
            "CREATE INDEX: index '" + stmt.index_name + "' not in catalog");
    }

    TxnId tid = ensure_txn();

    PageId idx_root = IndexManager::create(pager_);
    Page* idx_page = pager_.get_page(idx_root);
    txn_mgr_.write_page(tid, idx_root, idx_page->data);

    int col_ordinal = catalog_.column_ordinal(stmt.table_name, stmt.column_name);
    if (col_ordinal < 0) {
        throw std::runtime_error(
            "CREATE INDEX: column '" + stmt.column_name + "' not found");
    }

    // Backfill: scan all existing rows and insert into index
    if (schema->root_page_id >= 0) {
        BTree table_tree(pager_,
                         static_cast<PageId>(schema->root_page_id),
                         schema->columns.size());
        std::vector<Row> rows = table_tree.scan_all();

        for (const auto& row : rows) {
            int64_t pk = std::get<int64_t>(row[0]);
            const Value& indexed_val = row[static_cast<size_t>(col_ordinal)];
            idx_root = IndexManager::insert_entry(
                pager_, idx_root, indexed_val, pk);
        }
    }

    idx_info->root_page_id = static_cast<int>(idx_root);

    auto_commit();
    save_catalog();

    std::cout << "[DDL] CREATE INDEX " << stmt.index_name
              << " ON " << stmt.table_name
              << "(" << stmt.column_name << ")"
              << " (root_page=" << idx_root << ")\n";
}

// ── INSERT ────────────────────────────────────────────────────────────────────

void DatabaseEngine::executeInsert(const InsertStmt& stmt) {
    TableSchema* schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
        throw std::runtime_error(
            "INSERT: table '" + stmt.table_name + "' not in catalog");
    }
    if (schema->root_page_id < 0) {
        throw std::runtime_error(
            "INSERT: table '" + stmt.table_name + "' has no storage root");
    }

    Row row;
    row.reserve(stmt.values.size());
    for (const auto& lit : stmt.values) {
        row.push_back(literalToValue(lit));
    }

    TxnId tid = ensure_txn();

    // ── Pre-insert snapshot ──────────────────────────────────────────────────
    // We must log existing pages BEFORE the cursor.insert() modifies them so
    // that TxnManager captures the correct before-image for UNDO (ROLLBACK).
    // We snapshot every existing page in the B-tree range:  page 1 through
    // (page_count - 1).  Page 0 is the catalog (not WAL-managed).
    // This is conservative but correct for v1 (single table at a time).
    uint32_t pages_before = pager_.page_count();
    for (uint32_t pg = 1; pg < pages_before; pg++) {
        Page* p = pager_.get_page(pg);
        txn_mgr_.write_page(tid, pg, p->data);
    }

    // ── Do the insert ─────────────────────────────────────────────────────────
    BTreeCursor cursor(pager_,
                       static_cast<PageId>(schema->root_page_id),
                       schema->columns.size());
    cursor.insert(row);

    PageId new_root = cursor.root_page_id();
    if (static_cast<int>(new_root) != schema->root_page_id) {
        schema->root_page_id = static_cast<int>(new_root);
    }

    // ── Log newly allocated pages (splits) ────────────────────────────────────
    // These pages didn't exist before the insert, so their "before" image is
    // all-zeroes (which is what a fresh page looks like).  TxnManager.write_page
    // will capture the current (all-zeroes before, actual content after) state.
    uint32_t pages_after = pager_.page_count();
    for (uint32_t pg = pages_before; pg < pages_after; pg++) {
        Page* p = pager_.get_page(pg);
        txn_mgr_.write_page(tid, pg, p->data);
    }

    // Update indexes
    int64_t pk = std::get<int64_t>(row[0]);
    for (auto& idx : schema->indexes) {
        int col_ord = catalog_.column_ordinal(stmt.table_name, idx.column);
        if (col_ord < 0) continue;
        const Value& indexed_val = row[static_cast<size_t>(col_ord)];
        PageId new_idx_root = IndexManager::insert_entry(
            pager_,
            static_cast<PageId>(idx.root_page_id),
            indexed_val,
            pk);
        idx.root_page_id = static_cast<int>(new_idx_root);
    }

    auto_commit();
    save_catalog();

    std::cout << "[DML] INSERT INTO " << stmt.table_name << " -> OK\n";
}

// ── DELETE ───────────────────────────────────────────────────────────────────

void DatabaseEngine::executeDelete(const DeleteStmt& stmt) {
    TableSchema* schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
        throw std::runtime_error(
            "DELETE: table '" + stmt.table_name + "' not in catalog");
    }
    if (schema->root_page_id < 0) {
        throw std::runtime_error(
            "DELETE: table '" + stmt.table_name + "' has no storage root");
    }

    TxnId tid = ensure_txn();

    // 1. Snapshot ALL existing pages for rollback safety
    uint32_t pages_before = pager_.page_count();
    for (uint32_t pg = 1; pg < pages_before; pg++) {
        Page* p = pager_.get_page(pg);
        txn_mgr_.write_page(tid, pg, p->data);
    }

    // 2. Build dummy SelectStmt to reuse Planner/Executor for WHERE evaluation
    SelectStmt dummy_sel;
    dummy_sel.from.push_back({stmt.table_name, ""});
    dummy_sel.columns.push_back("*");
    if (stmt.where) {
        dummy_sel.where = cloneExpr(*stmt.where);
    }

    Planner planner(catalog_);
    std::unique_ptr<OperatorNode> plan = planner.plan(dummy_sel);
    ExecutorAdapter adapter(pager_, catalog_);
    OperatorPtr root = adapter.build(*plan);

    // 3. Scan Phase: Collect all candidate rows
    root->open();
    std::vector<Row> candidate_rows;
    while (auto row = root->next()) {
        candidate_rows.push_back(*row);
    }

    // 4. Mutation Phase
    for (const auto& row : candidate_rows) {
        int64_t pk = std::get<int64_t>(row[0]);

        // Remove secondary index entries
        for (const auto& idx : schema->indexes) {
            int col_ord = catalog_.column_ordinal(stmt.table_name, idx.column);
            if (col_ord < 0) continue;
            const Value& indexed_val = row[static_cast<size_t>(col_ord)];
            IndexManager::remove_entry(pager_, static_cast<PageId>(idx.root_page_id), indexed_val, pk);
        }

        // Delete main table row
        BTreeCursor cursor(pager_, static_cast<PageId>(schema->root_page_id), schema->columns.size());
        cursor.seek(pk);
        cursor.delete_current_row();
    }

    auto_commit();
    save_catalog();

    std::cout << "[DML] DELETE FROM " << stmt.table_name << " -> " << candidate_rows.size() << " rows deleted\n";
}

// ── UPDATE ────────────────────────────────────────────────────────────────────
//
// SET EXPRESSION SEMANTICS: All SET expressions are evaluated against the
// ORIGINAL row (before any assignment in this row is applied). This matches
// SQL standard snapshot semantics and means:
//
//   UPDATE t SET a = b, b = a;
//
// will swap the values correctly — each RHS is read from the pre-update row.
//
// KNOWN LIMITATION: WAL crash-recovery (REDO) is not correctly implemented
// in the underlying TxnManager. In-memory rollback (ROLLBACK statement) works
// correctly via the brute-force before-image snapshot, but crash durability
// for UPDATE is not guaranteed.

void DatabaseEngine::executeUpdate(const UpdateStmt& stmt) {
    TableSchema* schema = catalog_.get_table(stmt.table_name);
    if (!schema) {
        throw std::runtime_error(
            "UPDATE: table '" + stmt.table_name + "' not in catalog");
    }
    if (schema->root_page_id < 0) {
        throw std::runtime_error(
            "UPDATE: table '" + stmt.table_name + "' has no storage root");
    }

    TxnId tid = ensure_txn();

    // ── 1. Snapshot ALL existing pages before any mutation ────────────────────
    uint32_t pages_before = pager_.page_count();
    for (uint32_t pg = 1; pg < pages_before; pg++) {
        Page* p = pager_.get_page(pg);
        txn_mgr_.write_page(tid, pg, p->data);
    }

    // ── 2. Determine which column ordinals are being SET ──────────────────────
    // Build a map: col_name -> assignment ordinal in stmt.updates (for lookup)
    // Also detect if the primary key is being updated.
    const std::string& pk_col = schema->primary_key;
    bool pk_updated = false;
    int  pk_col_ord = catalog_.column_ordinal(stmt.table_name, pk_col);

    for (const auto& asgn : stmt.updates) {
        if (asgn.column == pk_col) {
            pk_updated = true;
            break;
        }
    }

    // Build schema structure for eval()
    // We need the TableSchema* for the eval call — the catalog already has it.

    // ── 3. Scan Phase: materialise matching rows ──────────────────────────────
    SelectStmt dummy_sel;
    dummy_sel.from.push_back({stmt.table_name, ""});
    dummy_sel.columns.push_back("*");
    if (stmt.where) {
        dummy_sel.where = cloneExpr(*stmt.where);
    }

    Planner planner(catalog_);
    std::unique_ptr<OperatorNode> plan = planner.plan(dummy_sel);
    ExecutorAdapter adapter(pager_, catalog_);
    OperatorPtr root = adapter.build(*plan);

    root->open();
    std::vector<Row> candidate_rows;
    while (auto row = root->next()) {
        candidate_rows.push_back(*row);
    }

    if (candidate_rows.empty()) {
        auto_commit();
        save_catalog();
        std::cout << "[DML] UPDATE " << stmt.table_name << " -> 0 rows updated\n";
        return;
    }

    // ── 4. For PK-update: pre-validate all new PKs for collisions ────────────
    //      (check before any mutation so we don't leave a half-updated state)
    if (pk_updated) {
        // Collect all proposed new PKs
        std::unordered_set<int64_t> proposed_pks;

        for (const auto& old_row : candidate_rows) {
            // Evaluate new PK expression against ORIGINAL row
            int64_t old_pk = std::get<int64_t>(old_row[0]);
            int64_t new_pk = old_pk;

            for (const auto& asgn : stmt.updates) {
                if (asgn.column == pk_col) {
                    Value v = eval(asgn.value, old_row, *schema);
                    if (!std::holds_alternative<int64_t>(v)) {
                        throw std::runtime_error(
                            "UPDATE: primary key must be INTEGER");
                    }
                    new_pk = std::get<int64_t>(v);
                    break;
                }
            }

            // Check for collision with existing rows that are NOT being updated
            if (new_pk != old_pk) {
                // Check against other proposed new PKs (duplicate in same stmt)
                if (!proposed_pks.insert(new_pk).second) {
                    throw std::runtime_error(
                        "UPDATE: primary key collision within UPDATE statement: pk=" +
                        std::to_string(new_pk));
                }

                // Check against rows NOT in candidate set
                BTreeCursor chk_cursor(pager_,
                    static_cast<PageId>(schema->root_page_id),
                    schema->columns.size());
                if (chk_cursor.seek(new_pk)) {
                    // It exists — check if it's also being updated (will be deleted)
                    Row existing = chk_cursor.current();
                    int64_t existing_pk = std::get<int64_t>(existing[0]);
                    bool is_also_candidate = false;
                    for (const auto& cr : candidate_rows) {
                        if (std::get<int64_t>(cr[0]) == existing_pk) {
                            is_also_candidate = true;
                            break;
                        }
                    }
                    if (!is_also_candidate) {
                        throw std::runtime_error(
                            "UPDATE: new primary key " + std::to_string(new_pk) +
                            " already exists");
                    }
                }
            }
        }
    }

    // ── 5. Mutation Phase ─────────────────────────────────────────────────────
    size_t rows_updated = 0;

    for (const auto& old_row : candidate_rows) {
        int64_t old_pk = std::get<int64_t>(old_row[0]);

        // 5a. Build new_row: start from old_row, apply each assignment
        //     ALL expressions evaluated against the ORIGINAL old_row (snapshot semantics)
        Row new_row = old_row;
        for (const auto& asgn : stmt.updates) {
            int col_ord = catalog_.column_ordinal(stmt.table_name, asgn.column);
            if (col_ord < 0) {
                throw std::runtime_error(
                    "UPDATE: column '" + asgn.column + "' not found in table '" +
                    stmt.table_name + "'");
            }
            Value new_val = eval(asgn.value, old_row, *schema);  // eval against OLD row
            new_row[static_cast<size_t>(col_ord)] = new_val;
        }

        int64_t new_pk = std::get<int64_t>(new_row[0]);

        if (new_pk == old_pk) {
            // ── 5b. Non-PK update path ────────────────────────────────────────
            // For each indexed column: if value changed, remove old + insert new entry
            for (auto& idx : schema->indexes) {
                int col_ord = catalog_.column_ordinal(stmt.table_name, idx.column);
                if (col_ord < 0) continue;
                const Value& old_val = old_row[static_cast<size_t>(col_ord)];
                const Value& new_val = new_row[static_cast<size_t>(col_ord)];

                // Only update index if value actually changed
                if (old_val != new_val) {
                    IndexManager::remove_entry(pager_,
                        static_cast<PageId>(idx.root_page_id), old_val, old_pk);

                    // Update table row first (so it exists with correct data)
                    // (We'll do the actual row write below, just prepare here)

                    PageId new_idx_root = IndexManager::insert_entry(pager_,
                        static_cast<PageId>(idx.root_page_id), new_val, old_pk);
                    idx.root_page_id = static_cast<int>(new_idx_root);
                }
            }

            // Update the table row in-place
            BTreeCursor upd_cursor(pager_,
                static_cast<PageId>(schema->root_page_id),
                schema->columns.size());
            if (!upd_cursor.seek(old_pk)) {
                throw std::runtime_error(
                    "UPDATE: internal error — row with pk=" +
                    std::to_string(old_pk) + " not found on re-seek");
            }
            upd_cursor.update_current_row(new_row);  // may throw if row too large

        } else {
            // ── 5c. PK-update path: DELETE old + INSERT new ───────────────────
            // Step 1: Remove all secondary index entries for old row
            for (auto& idx : schema->indexes) {
                int col_ord = catalog_.column_ordinal(stmt.table_name, idx.column);
                if (col_ord < 0) continue;
                const Value& old_val = old_row[static_cast<size_t>(col_ord)];
                IndexManager::remove_entry(pager_,
                    static_cast<PageId>(idx.root_page_id), old_val, old_pk);
            }

            // Step 2: Delete old table row
            BTreeCursor del_cursor(pager_,
                static_cast<PageId>(schema->root_page_id),
                schema->columns.size());
            if (!del_cursor.seek(old_pk)) {
                throw std::runtime_error(
                    "UPDATE(pk-change): internal error — old row pk=" +
                    std::to_string(old_pk) + " not found");
            }
            del_cursor.delete_current_row();

            // Step 3: Insert new table row (will throw on duplicate PK)
            BTreeCursor ins_cursor(pager_,
                static_cast<PageId>(schema->root_page_id),
                schema->columns.size());
            ins_cursor.insert(new_row);
            // Update root in case of split
            PageId new_tbl_root = ins_cursor.root_page_id();
            if (static_cast<int>(new_tbl_root) != schema->root_page_id) {
                schema->root_page_id = static_cast<int>(new_tbl_root);
            }

            // Step 4: Insert new secondary index entries
            for (auto& idx : schema->indexes) {
                int col_ord = catalog_.column_ordinal(stmt.table_name, idx.column);
                if (col_ord < 0) continue;
                const Value& new_val = new_row[static_cast<size_t>(col_ord)];
                PageId new_idx_root = IndexManager::insert_entry(pager_,
                    static_cast<PageId>(idx.root_page_id), new_val, new_pk);
                idx.root_page_id = static_cast<int>(new_idx_root);
            }
        }

        ++rows_updated;
    }

    auto_commit();
    save_catalog();

    std::cout << "[DML] UPDATE " << stmt.table_name << " -> " << rows_updated << " rows updated\n";
}

// ── SELECT ────────────────────────────────────────────────────────────────────


void DatabaseEngine::executeSelect(const SelectStmt& stmt) {
    Planner planner(catalog_);
    std::unique_ptr<OperatorNode> plan = planner.plan(stmt);

    ExecutorAdapter adapter(pager_, catalog_);
    OperatorPtr root = adapter.build(*plan);

    // Build column header names
    std::vector<std::string> headers;
    bool has_star = (stmt.columns.size() == 1 && stmt.columns[0] == "*");

    if (has_star && stmt.aggregate_columns.empty()) {
        if (!stmt.from.empty()) {
            const TableSchema* sch = catalog_.get_table(stmt.from[0].table_name);
            if (sch) {
                for (const auto& col : sch->columns) headers.push_back(col.name);
            }
        }
        if (headers.empty()) headers.push_back("*");
    } else {
        for (const auto& c : stmt.columns) {
            if (c != "*") headers.push_back(c);
        }
        for (const auto& agg : stmt.aggregate_columns) {
            std::string label = aggFuncToString(agg.func) + "(";
            label += agg.is_star ? "*" : "col";
            label += ")";
            headers.push_back(label);
        }
    }

    printHeader(headers);

    root->open();
    int row_count = 0;
    while (auto row = root->next()) {
        printRow(*row);
        row_count++;
    }
    root->close();

    std::cout << "(" << row_count << " row" << (row_count == 1 ? "" : "s") << ")\n";
}

// ── Transaction statements ────────────────────────────────────────────────────

void DatabaseEngine::executeTxn(const TxnStmt& stmt) {
    switch (stmt.kind) {
        case TxnStmt::Kind::BEGIN:
            if (active_txn_.has_value()) {
                std::cerr << "[TXN] WARNING: already in a transaction (ignored)\n";
                return;
            }
            active_txn_  = txn_mgr_.begin();
            explicit_txn_ = true;   // user explicitly opened this transaction
            std::cout << "[TXN] BEGIN\n";
            break;

        case TxnStmt::Kind::COMMIT:
            if (!active_txn_.has_value()) {
                std::cerr << "[TXN] WARNING: no active transaction\n";
                return;
            }
            txn_mgr_.commit(*active_txn_);
            active_txn_   = std::nullopt;
            explicit_txn_ = false;
            save_catalog();
            std::cout << "[TXN] COMMIT\n";
            break;

        case TxnStmt::Kind::ROLLBACK:
            if (!active_txn_.has_value()) {
                std::cerr << "[TXN] WARNING: no active transaction\n";
                return;
            }
            txn_mgr_.rollback(*active_txn_);
            active_txn_   = std::nullopt;
            explicit_txn_ = false;
            load_catalog();   // reload catalog to undo schema changes
            std::cout << "[TXN] ROLLBACK\n";
            break;
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

Value DatabaseEngine::literalToValue(const Literal& lit) {
    return std::visit([](const auto& v) -> Value {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, int64_t>)       return Value(v);
        if constexpr (std::is_same_v<T, double>)         return Value(v);
        if constexpr (std::is_same_v<T, std::string>)    return Value(v);
        if constexpr (std::is_same_v<T, std::monostate>) return Value(std::monostate{});
        return Value(std::monostate{});
    }, lit.value);
}

void DatabaseEngine::printRow(const Row& row) {
    for (size_t i = 0; i < row.size(); i++) {
        if (i > 0) std::cout << " | ";
        std::visit([](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) std::cout << "NULL";
            else                                              std::cout << v;
        }, row[i]);
    }
    std::cout << "\n";
}

void DatabaseEngine::printHeader(const std::vector<std::string>& names) {
    for (size_t i = 0; i < names.size(); i++) {
        if (i > 0) std::cout << " | ";
        std::cout << names[i];
    }
    std::cout << "\n";
    for (size_t i = 0; i < names.size(); i++) {
        if (i > 0) std::cout << "-+-";
        std::cout << std::string(names[i].size(), '-');
    }
    std::cout << "\n";
}
