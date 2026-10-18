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
        , lookup_value_(std::move(lookup_value)) {}

    void open() override {
        target_key_ = IndexManager::value_to_key(lookup_value_);
        cursor_ = IndexManager::open_cursor(pager_, index_root_, lookup_value_);
    }

    std::optional<Row> next() override {
        while (cursor_ && !cursor_->is_exhausted()) {
            Row r = cursor_->current();
            int64_t cur_key = std::get<int64_t>(r[0]);
            
            if (cur_key != target_key_) {
                cursor_.reset(); // no more matches
                return std::nullopt;
            }

            int64_t pk = std::get<int64_t>(r[1]);
            cursor_->next(); // advance for the next call

            // fetch the full row from the main table using the PK
            BTree table(pager_, table_root_, num_cols_); // Main table uses default unique_keys=true
            auto row = table.search(pk);
            if (row.has_value()) {
                return row;
            }
            // If row not found in main table, it's a consistency error, but we skip and try the next index entry
        }
        return std::nullopt;
    }

    void close() override {
        cursor_.reset();
    }

private:
    Pager&   pager_;
    PageId   table_root_;
    PageId   index_root_;
    size_t   num_cols_;
    Value    lookup_value_;
    int64_t  target_key_;
    std::unique_ptr<BTreeCursor> cursor_;
};
