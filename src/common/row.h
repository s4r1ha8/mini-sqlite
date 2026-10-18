#pragma once
#include <variant>
#include <string>
#include <vector>
#include <cstdint>
#include <iostream>

// ─────────────────────────────────────────────
//  Value  —  a single cell in a row
// ─────────────────────────────────────────────
//
// A database cell can hold one of four things:
//   - An integer  (e.g. age = 25)
//   - Some text   (e.g. name = "Alice")
//   - A decimal   (e.g. price = 9.99)
//   - Nothing     (NULL — column has no value)
//
// std::variant is C++'s way of saying "this variable holds
// exactly ONE of these types at a time."
// std::monostate is the C++ way of representing "nothing" / NULL.

using Value = std::variant<
    int64_t,         // INTEGER  — 64-bit whole number
    std::string,     // TEXT     — any length string
    double,          // REAL     — decimal number
    std::monostate   // NULL     — no value
>;

// ─────────────────────────────────────────────
//  Key  —  used to look up / seek in a B-tree
// ─────────────────────────────────────────────
//
// For a table cursor: the primary key value (usually an integer)
// For an index cursor: the indexed column's value
// It's the same type as Value — just given a name to make
// intent clear when reading code.

using Key = Value;

// ─────────────────────────────────────────────
//  Row  —  one complete database row
// ─────────────────────────────────────────────
//
// A row is just an ordered list of Values.
// The order matches the column order defined in the catalog.
// e.g. for table users(id INTEGER, name TEXT, age INTEGER):
//   Row = { Value(1), Value("Alice"), Value(30) }

using Row = std::vector<Value>;

// ─────────────────────────────────────────────
//  Helper: print a Value (useful for debugging)
// ─────────────────────────────────────────────

inline std::ostream& operator<<(std::ostream& os, const Value& v) {
    std::visit([&os](const auto& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::monostate>)
            os << "NULL";
        else
            os << val;
    }, v);
    return os;
}

// ─────────────────────────────────────────────
//  Helper: print a whole Row (useful for debugging)
// ─────────────────────────────────────────────

inline std::ostream& operator<<(std::ostream& os, const Row& row) {
    os << "(";
    for (size_t i = 0; i < row.size(); ++i) {
        os << row[i];
        if (i + 1 < row.size()) os << ", ";
    }
    os << ")";
    return os;
}

// ─────────────────────────────────────────────
//  Helper: extract the primary key from a row
//  (always column 0 per our convention)
// ─────────────────────────────────────────────

inline Key primary_key(const Row& row) {
    return row.at(0);
}
