#pragma once
#include "../common/cursor_interface.h"
#include "../storage/pager.h"
#include "../storage/index_manager.h"
#include "../storage/btree.h"

// ─────────────────────────────────────────────────────────────────
//  IndexScan
//
//  Uses an index to find rows matching a specific column value,
//  instead of scanning the whole table.
//
//  Steps:
//    1. Look up `lookup_value` in the index B-tree → get PK
//    2. Use that PK to search the main table B-tree → get full row
//    3. Return the full row
//
//  This is exactly what happens when you write:
//    SELECT * FROM users WHERE name = 'Alice'
//  and there's an index on the name column.
//
//  For M3 we support point lookups only (exact match).
//  Range scans (WHERE age > 25) come in M3 stretch / M4.
// ─────────────────────────────────────────────────────────────────
class IndexScan : public Operator {
public:
    IndexScan(Pager& pager,
              PageId table_root,   // main table B-tree root
              PageId index_root,   // index B-tree root
              size_t num_cols,     // columns in the main table
              Value  lookup_value) // the value we're searching for
        : pager_(pager)
        , table_root_(table_root)
        , index_root_(index_root)
        , num_cols_(num_cols)
        , lookup_value_(std::move(lookup_value))
        , done_(false) {}

    void open() override {
        done_ = false;
        result_.reset();

        // Step 1: look up the index
        auto pk = IndexManager::lookup(pager_, index_root_, lookup_value_);
        if (!pk.has_value()) {
            done_ = true; // not found in index
            return;
        }

        // Step 2: fetch the full row from the main table using the PK
        BTree table(pager_, table_root_, num_cols_);
        auto row = table.search(*pk);
        if (!row.has_value()) {
            done_ = true; // shouldn't happen if index is consistent
            return;
        }

        result_ = std::move(row);
    }

    std::optional<Row> next() override {
        if (done_ || !result_.has_value()) return std::nullopt;
        // Point lookup returns at most one row — mark done after first call
        done_ = true;
        return result_;
    }

    void close() override {
        result_.reset();
        done_ = true;
    }

private:
    Pager&   pager_;
    PageId   table_root_;
    PageId   index_root_;
    size_t   num_cols_;
    Value    lookup_value_;
    bool     done_;
    std::optional<Row> result_;
};
