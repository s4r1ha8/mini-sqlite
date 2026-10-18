#pragma once
#include "../common/cursor_interface.h"
#include "../storage/cursor.h"
#include "../storage/pager.h"

// ─────────────────────────────────────────────────────────────────
//  SeqScan
//
//  The simplest operator: scan every row in a table from first
//  to last key order. Wraps a BTreeCursor and just calls
//  rewind() + next() repeatedly.
//
//  Sarthak's planner creates: SeqScan("users")
//  Your executor runs it by: open() → loop next() → close()
//
//  Usage:
//    SeqScan scan(pager, root_page_id, num_cols);
//    scan.open();
//    while (auto row = scan.next()) {
//        // use *row
//    }
//    scan.close();
// ─────────────────────────────────────────────────────────────────
class SeqScan : public Operator {
public:
    SeqScan(Pager& pager, PageId root_page, size_t num_cols)
        : pager_(pager)
        , root_page_(root_page)
        , num_cols_(num_cols)
        , cursor_(nullptr) {}

    void open() override {
        // Create cursor and position at the very first row
        cursor_ = std::make_unique<BTreeCursor>(pager_, root_page_, num_cols_);
        cursor_->rewind();
    }

    std::optional<Row> next() override {
        if (!cursor_ || cursor_->is_exhausted()) return std::nullopt;
        Row r = cursor_->current();
        cursor_->next();   // advance so next call gets the next row
        return r;
    }

    void close() override {
        cursor_.reset(); // release the cursor
    }

private:
    Pager&   pager_;
    PageId   root_page_;
    size_t   num_cols_;
    std::unique_ptr<BTreeCursor> cursor_;
};
