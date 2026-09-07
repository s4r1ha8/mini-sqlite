// ============================================================================
// eval.cpp  —  Expression evaluator implementation.
// ============================================================================

#include "common/eval.h"
#include <stdexcept>
#include <string>

// ── Internal helpers ──────────────────────────────────────────────────────────

// Compare two Values with a given operator string.
// Returns a boolean Value (stored as int64_t: 1 or 0).
static Value compareValues(const Value& left, const std::string& op, const Value& right) {
    // NULL comparisons always yield false (SQL three-valued logic simplified)
    if (std::holds_alternative<std::monostate>(left) ||
        std::holds_alternative<std::monostate>(right)) {
        return int64_t(0);
    }

    // Promote int/real for mixed arithmetic
    Value l = left, r = right;
    if (std::holds_alternative<double>(left) && std::holds_alternative<int64_t>(right))
        r = static_cast<double>(std::get<int64_t>(right));
    else if (std::holds_alternative<double>(right) && std::holds_alternative<int64_t>(left))
        l = static_cast<double>(std::get<int64_t>(left));

    // Type mismatch after promotion = false
    if (l.index() != r.index()) return int64_t(0);

    auto cmp = [&](auto lv, auto rv) -> int64_t {
        if (op == "=")  return lv == rv ? 1 : 0;
        if (op == "!=") return lv != rv ? 1 : 0;
        if (op == "<")  return lv <  rv ? 1 : 0;
        if (op == ">")  return lv >  rv ? 1 : 0;
        if (op == "<=") return lv <= rv ? 1 : 0;
        if (op == ">=") return lv >= rv ? 1 : 0;
        throw EvalError("Unknown comparison operator '" + op + "'.");
        return int64_t(0);
    };

    if (std::holds_alternative<int64_t>(l))
        return cmp(std::get<int64_t>(l), std::get<int64_t>(r));
    if (std::holds_alternative<double>(l))
        return cmp(std::get<double>(l), std::get<double>(r));
    if (std::holds_alternative<std::string>(l))
        return cmp(std::get<std::string>(l), std::get<std::string>(r));

    throw EvalError("Cannot compare values of this type with '" + op + "'.");
}

// ── Main evaluator ────────────────────────────────────────────────────────────

Value eval(const Expr& expr, const Row& row, const TableSchema& schema) {
    return std::visit([&](const auto& node) -> Value {
        using T = std::decay_t<decltype(node)>;

        // ── Literal ──────────────────────────────────────────────────────────
        if constexpr (std::is_same_v<T, Literal>) {
            return std::visit([](const auto& v) -> Value { return Value(v); }, node.value);
        }

        // ── ColumnRef ────────────────────────────────────────────────────────
        if constexpr (std::is_same_v<T, ColumnRef>) {
            // Qualified reference: table.column
            //
            // For combined JOIN schemas, the execution layer may store
            // column names as "table.column" while preserving the original
            // ordinal. This lets runtime evaluation honor the qualifier.
            if (node.table.has_value()) {
                const std::string qualified =
                    *node.table + "." + node.column;

                for (const auto& col : schema.columns) {
                    if (col.name == qualified) {
                        if (col.ordinal < 0 ||
                            col.ordinal >= static_cast<int>(row.size())) {
                            throw EvalError(
                                "Row does not have value at ordinal " +
                                std::to_string(col.ordinal) +
                                " for column '" + qualified + "'.");
                        }
                        return row[col.ordinal];
                    }
                }

                // Also allow a schema whose column name is the original
                // unqualified name when there is only one such match.
                // This preserves existing single-table behavior.
                const TableSchema* single_table_schema = &schema;
                if (single_table_schema->columns.size() > 0 &&
                    schema.name == *node.table) {
                    for (const auto& col : schema.columns) {
                        if (col.name == node.column) {
                            if (col.ordinal < 0 ||
                                col.ordinal >= static_cast<int>(row.size())) {
                                throw EvalError(
                                    "Row does not have value at ordinal " +
                                    std::to_string(col.ordinal) +
                                    " for column '" + node.column + "'.");
                            }
                            return row[col.ordinal];
                        }
                    }
                }

                throw EvalError(
                    "Column '" + qualified +
                    "' not found in schema '" + schema.name + "'.");
            }

            // Unqualified reference.
            // Find exactly one matching bare column name.
            const ColumnInfo* match = nullptr;

            for (const auto& col : schema.columns) {
                std::string bare_name = col.name;

                const size_t dot = bare_name.find('.');
                if (dot != std::string::npos)
                    bare_name = bare_name.substr(dot + 1);

                if (bare_name == node.column) {
                    if (match != nullptr) {
                        throw EvalError(
                            "Column '" + node.column +
                            "' is ambiguous in schema '" +
                            schema.name + "'.");
                    }
                    match = &col;
                }
            }

            if (!match) {
                throw EvalError(
                    "Column '" + node.column +
                    "' not found in schema '" + schema.name + "'.");
            }

            if (match->ordinal < 0 ||
                match->ordinal >= static_cast<int>(row.size())) {
                throw EvalError(
                    "Row does not have value at ordinal " +
                    std::to_string(match->ordinal) +
                    " for column '" + node.column + "'.");
            }

            return row[match->ordinal];
        }

        // ── BinaryOp ─────────────────────────────────────────────────────────
        if constexpr (std::is_same_v<T, BinaryOp>) {
            // Short-circuit AND / OR
            if (node.op == "AND") {
                Value lv = eval(*node.left, row, schema);
                if (std::holds_alternative<int64_t>(lv) && std::get<int64_t>(lv) == 0)
                    return int64_t(0); // false AND anything = false
                if (std::holds_alternative<std::monostate>(lv))
                    return int64_t(0); // NULL AND anything = false (simplified)
                Value rv = eval(*node.right, row, schema);
                if (std::holds_alternative<int64_t>(rv) && std::get<int64_t>(rv) == 0)
                    return int64_t(0);
                if (std::holds_alternative<std::monostate>(rv))
                    return int64_t(0);
                return int64_t(1);
            }
            if (node.op == "OR") {
                Value lv = eval(*node.left, row, schema);
                if (std::holds_alternative<int64_t>(lv) && std::get<int64_t>(lv) != 0)
                    return int64_t(1); // true OR anything = true
                Value rv = eval(*node.right, row, schema);
                if (std::holds_alternative<int64_t>(rv) && std::get<int64_t>(rv) != 0)
                    return int64_t(1);
                return int64_t(0);
            }
            // Comparison operators
            Value lv = eval(*node.left,  row, schema);
            Value rv = eval(*node.right, row, schema);
            return compareValues(lv, node.op, rv);
        }

        // ── UnaryOp (NOT) ─────────────────────────────────────────────────────
        if constexpr (std::is_same_v<T, UnaryOp>) {
            if (node.op != "NOT") {
                throw EvalError("Unknown unary operator '" + node.op + "'.");
            }
            Value v = eval(*node.operand, row, schema);
            if (std::holds_alternative<std::monostate>(v)) return int64_t(0);
            if (std::holds_alternative<int64_t>(v))
                return int64_t(std::get<int64_t>(v) ? 0 : 1);
            return int64_t(0);
        }

        // ── AggregateCall ─────────────────────────────────────────────────────
        // Aggregates can't be evaluated row-by-row — they're handled by the
        // executor's AggregateOp.  If we reach here it's a bug.
        if constexpr (std::is_same_v<T, AggregateCall>) {
            throw EvalError(
                "Aggregate function " + aggFuncToString(node.func) +
                " cannot be evaluated in a row-level expression context. "
                "It should be handled by the AggregateOp executor node.");
        }

        throw EvalError("Unknown Expr node type in eval().");
    }, expr.node);
}

// ── evalBool ─────────────────────────────────────────────────────────────────

bool evalBool(const Expr& expr, const Row& row, const TableSchema& schema) {
    Value v = eval(expr, row, schema);
    if (std::holds_alternative<std::monostate>(v)) return false; // NULL → false
    if (std::holds_alternative<int64_t>(v))        return std::get<int64_t>(v) != 0;
    if (std::holds_alternative<double>(v))         return std::get<double>(v) != 0.0;
    if (std::holds_alternative<std::string>(v))    return !std::get<std::string>(v).empty();
    return false;
}
