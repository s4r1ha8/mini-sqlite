#pragma once
#include "../common/cursor_interface.h"
#include <functional>
#include <vector>

// ─────────────────────────────────────────────────────────────────
//  NestedLoopJoin
//
//  Joins two tables: for every row in `left`, scan ALL rows in
//  `right` and emit any pair where the join condition is true.
//
//  Example: users JOIN orders ON users.id = orders.user_id
//
//    NestedLoopJoin(
//        SeqScan("users"),
//        SeqScan("orders"),
//        [](Row combined) { return combined[0] == combined[4]; }
//    )
//
//  "Combined row" = left row columns + right row columns appended.
//  So if users has 3 cols and orders has 3 cols, the combined row
//  has 6 cols: [u.id, u.name, u.age, o.id, o.user_id, o.total]
//
//  Why "nested loop"?
//    Outer loop: for each left row
//      Inner loop: scan ALL right rows
//    This is O(left * right) — slow but always correct.
//    M3's IndexScan will make the inner loop fast via index.
//
//  Note: we buffer right into memory on first open() call so we
//  can re-scan it for each left row without re-opening the pager.
//  For large tables this is not ideal — fine for our scale.
// ─────────────────────────────────────────────────────────────────
class NestedLoopJoin : public Operator {
public:
    using JoinCondition = std::function<bool(const Row&)>;

    NestedLoopJoin(OperatorPtr left, OperatorPtr right, JoinCondition condition)
        : left_(std::move(left))
        , right_(std::move(right))
        , condition_(std::move(condition))
        , right_idx_(0) {}

    void open() override {
        left_->open();
        right_->open();

        // Buffer the entire right side into memory once.
        // This lets us re-scan it cheaply for every left row.
        right_buffer_.clear();
        while (auto row = right_->next()) {
            right_buffer_.push_back(*row);
        }
        right_->close();

        // Pull the first left row
        current_left_ = left_->next();
        right_idx_ = 0;
    }

    std::optional<Row> next() override {
        // Walk through (left row × right buffer) pairs
        // and return the first pair that satisfies the condition.
        while (current_left_.has_value()) {
            while (right_idx_ < right_buffer_.size()) {
                const Row& right_row = right_buffer_[right_idx_];
                right_idx_++;

                // Build combined row: left columns + right columns
                Row combined = *current_left_;
                combined.insert(combined.end(),
                                right_row.begin(), right_row.end());

                if (condition_(combined)) return combined;
            }
            // Exhausted right side for this left row — advance left
            current_left_ = left_->next();
            right_idx_ = 0; // reset right scan
        }
        return std::nullopt; // no more matching pairs
    }

    void close() override {
        left_->close();
        right_buffer_.clear();
    }

private:
    OperatorPtr  left_;
    OperatorPtr  right_;
    JoinCondition condition_;

    std::vector<Row>   right_buffer_;   // entire right table buffered
    std::optional<Row> current_left_;   // current left row being joined
    size_t             right_idx_;      // position in right_buffer_
};
