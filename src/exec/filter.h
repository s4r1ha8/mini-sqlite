#pragma once
#include "../common/cursor_interface.h"
#include <functional>

// ─────────────────────────────────────────────────────────────────
//  Filter
//
//  Wraps any child operator and only passes through rows where
//  the predicate returns true. This is how WHERE clauses work.
//
//  Example:
//    Filter(SeqScan("users"), [](Row r){ return age(r) > 25; })
//
//  In a real system, the predicate comes from Sarthak's expression
//  evaluator (eval(expr, row, schema) from cursor_interface.h).
//  For now we use std::function<bool(const Row&)> so you can
//  pass any lambda — easy to wire up to Sarthak's eval() later.
//
//  How it works:
//    next() keeps pulling rows from child until one passes the
//    predicate, then returns it. If child is exhausted, returns nullopt.
// ─────────────────────────────────────────────────────────────────
class Filter : public Operator {
public:
    using Predicate = std::function<bool(const Row&)>;

    Filter(OperatorPtr child, Predicate predicate)
        : child_(std::move(child))
        , predicate_(std::move(predicate)) {}

    void open() override {
        child_->open();  // just pass through to child
    }

    std::optional<Row> next() override {
        // Keep pulling from child until we find a row that passes,
        // or the child runs out of rows.
        while (true) {
            auto row = child_->next();
            if (!row.has_value()) return std::nullopt; // child exhausted
            if (predicate_(*row))  return row;          // passes filter
            // else: skip this row and try the next one
        }
    }

    void close() override {
        child_->close();
    }

private:
    OperatorPtr child_;
    Predicate   predicate_;
};
