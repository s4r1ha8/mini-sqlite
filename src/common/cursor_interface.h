#pragma once
#include "row.h"

// ══════════════════════════════════════════════════════════════
//  Cursor Interface
//  ── CO-OWNED by Priyanshu (storage) and Sarthak (executor) ──
//  DO NOT change this file without telling the other person first.
//  This is the exact boundary where our two halves of the project meet.
// ══════════════════════════════════════════════════════════════
//
// What is a Cursor?
// -----------------
// Think of a Cursor like a finger pointing at a row in a table.
// You can move the finger forward (next), read what it's pointing
// at (current), jump to a specific row (seek), insert a new row,
// or delete the row the finger is on (remove).
//
// Priyanshu implements this (backed by the B-tree + pager).
// Sarthak's executor operators (SeqScan, IndexScan) call into it.
// Neither side needs to know how the other side works — just this contract.

class Cursor {
public:

    // Move to the next row in key order.
    // Returns TRUE  if there is a next row (you can call current() now).
    // Returns FALSE if we've gone past the last row — stop iterating.
    //
    // Usage pattern:
    //   cursor->seek(first_key);
    //   while (cursor->next()) {
    //       Row r = cursor->current();
    //       // do something with r
    //   }
    virtual bool next() = 0;

    // Return the row at the current cursor position.
    // Only call this AFTER a successful seek() or next().
    // Calling current() before seek/next, or after next() returned false,
    // is undefined behaviour — don't do it.
    virtual Row current() = 0;

    // Jump directly to the row with the given key.
    // Returns TRUE  if that exact key was found.
    // Returns FALSE if the key doesn't exist.
    //
    // After a successful seek(), current() returns that row,
    // and next() will advance from there.
    //
    // This is what makes index lookups fast: instead of scanning
    // every row, we jump straight to the one we want.
    virtual bool seek(Key key) = 0;

    // Insert a new row into the table/index this cursor is over.
    // The row's primary key (column 0) must not already exist.
    // After insert, cursor position is unspecified — seek() if you
    // need to be at a known position afterward.
    virtual void insert(Row row) = 0;

    // Delete the row at the current cursor position.
    // Only call this after a successful seek() or next().
    // After remove(), cursor position is unspecified.
    virtual void remove() = 0;

    // Virtual destructor — needed whenever you have virtual methods.
    // This lets callers safely delete a Cursor* without knowing the
    // concrete type (e.g. BTreeCursor) underneath.
    virtual ~Cursor() = default;
};


// ══════════════════════════════════════════════════════════════
//  Operator Interface
//  Owner: Priyanshu implements the operators, Sarthak's planner
//  builds the tree of them.
// ══════════════════════════════════════════════════════════════
//
// What is an Operator?
// --------------------
// An Operator is one step in a query plan.
// e.g. "scan the users table", "filter rows where age > 25",
//      "join users with orders".
//
// They stack into a tree (the "Volcano/iterator model"):
//
//   Sort
//     └─ Filter (age > 25)
//          └─ SeqScan(users)
//
// Each operator only knows how to ask the one below it for
// the next row. Sort asks Filter, Filter asks SeqScan, SeqScan
// asks the Cursor (which reads from the B-tree on disk).
//
// How you use an operator:
//   op->open();                    // set up, allocate resources
//   while (true) {
//       auto row = op->next();     // pull one row
//       if (!row.has_value()) break; // no more rows
//       // process row...
//   }
//   op->close();                   // clean up

#include <optional>
#include <memory>

class Operator {
public:

    // Called once before any next() calls.
    // Sets up internal state, opens child operators, initializes cursors.
    virtual void open() = 0;

    // Pull the next output row from this operator.
    // Returns the row if one is available.
    // Returns std::nullopt when this operator is exhausted (no more rows).
    //
    // The caller (the operator above this one) keeps calling next()
    // until it gets nullopt. This is the entire Volcano model in one method.
    virtual std::optional<Row> next() = 0;

    // Called once after you're done pulling rows.
    // Frees resources, closes child operators, releases cursors.
    virtual void close() = 0;

    virtual ~Operator() = default;
};

// Convenient alias — operators are always heap-allocated and
// owned by their parent operator (or the executor at the top).
using OperatorPtr = std::unique_ptr<Operator>;
