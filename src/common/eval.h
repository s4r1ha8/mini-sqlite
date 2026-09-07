#pragma once

// ============================================================================
// eval.h  —  Expression evaluator.
//
// Evaluates an Expr AST node against a concrete Row (a vector of Values).
// Lives in common/ because:
//   • Sarthak builds it (it's based on Expr types from the AST).
//   • Priyanshu calls it from inside Filter and NestedLoopJoin operators
//     in his executor.
//
// Contract (from cursor_interface.md):
//   eval(expr, row, schema) -> Value
//
// `row` must be ordered according to `schema.columns[i].ordinal`.
// The schema is needed to resolve unqualified column names to row positions.
// ============================================================================

#include <stdexcept>
#include <string>
#include "common/row.h"
#include "common/ast.h"
#include "frontend/catalog.h"

// ── Value — the runtime value type (mirrors cursor_interface.md) ──────────────
//
// Using LiteralValue (already defined in ast.h) as our Value type:
//   std::variant<std::monostate, int64_t, double, std::string>
//   monostate = NULL
// Value and Row are defined by the shared common/row.h contract.

// ── EvalError — thrown when evaluation hits a type error or missing column ────
class EvalError : public std::runtime_error {
public:
    explicit EvalError(const std::string& msg)
        : std::runtime_error("Eval error: " + msg) {}
};

// ── eval — evaluate `expr` against `row` in the context of `schema` ──────────
//
// `schema` is used to resolve ColumnRef nodes to row positions.
// For JOIN rows the caller should pass the combined schema.
Value eval(const Expr& expr, const Row& row, const TableSchema& schema);

// ── evalBool — convenience wrapper that interprets the Value as a boolean ─────
//
// Rules:
//   - NULL  → false (NULL in a WHERE condition never passes)
//   - int64 → true if non-zero
//   - anything else → true if non-null/non-zero
bool evalBool(const Expr& expr, const Row& row, const TableSchema& schema);
