#pragma once

// ============================================================================
// ast.h  —  All AST node types produced by the parser.
//
// Owner: Sarthak (frontend).
// Priyanshu doesn't read the AST directly — he works off the operator tree
// defined in cursor_interface.md.  But changes to column types or table
// shapes ripple into the catalog format, so ping before modifying.
//
// Design notes:
//  • We use std::variant<> for sum types (Expr, Statement) — no virtual
//    dispatch, no heap allocation per node, safe with C++17 move semantics.
//  • Every node is plain data (no methods except helpers).  All logic lives
//    in the parser, semantic analyser, and planner.
//  • std::unique_ptr is used where a node contains a recursive child Expr
//    (because a variant can't contain itself directly).
// ============================================================================

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// ── Data types supported by the engine (v1) ──────────────────────────────────
enum class DataType { INTEGER, TEXT, REAL };

inline std::string dataTypeToString(DataType dt) {
    switch (dt) {
        case DataType::INTEGER: return "INTEGER";
        case DataType::TEXT:    return "TEXT";
        case DataType::REAL:    return "REAL";
    }
    return "UNKNOWN";
}

// ── Forward declarations needed for recursive Expr ───────────────────────────
struct BinaryOp;
struct UnaryOp;
struct AggregateCall;

// ── Aggregate function names ─────────────────────────────────────────────────
enum class AggFunc { COUNT, SUM, AVG, MIN, MAX };

inline std::string aggFuncToString(AggFunc f) {
    switch (f) {
        case AggFunc::COUNT: return "COUNT";
        case AggFunc::SUM:   return "SUM";
        case AggFunc::AVG:   return "AVG";
        case AggFunc::MIN:   return "MIN";
        case AggFunc::MAX:   return "MAX";
    }
    return "UNKNOWN";
}

// ── Literal value (NULL is represented as std::monostate) ────────────────────
using LiteralValue = std::variant<
    std::monostate,   // NULL
    int64_t,          // INTEGER literal
    double,           // REAL literal
    std::string       // TEXT literal (content without surrounding quotes)
>;

// ── Expression nodes ─────────────────────────────────────────────────────────
//
// Expr is a recursive sum type.  Because a std::variant cannot directly
// contain itself we wrap recursive children in std::unique_ptr<Expr>.
//
// Expr =
//   | ColumnRef      { table?: string, column: string }
//   | Literal        { value: LiteralValue }
//   | BinaryOp       { left: Expr, op: string, right: Expr }
//   | UnaryOp        { op: NOT,  operand: Expr }
//   | AggregateCall  { func: AggFunc, arg: Expr | "*" }

struct ColumnRef {
    std::optional<std::string> table;  // null means unqualified (e.g. "id")
    std::string                column; // e.g. "id" or "name"
};

struct Literal {
    LiteralValue value;
};

// Forward-declare Expr so BinaryOp / UnaryOp / AggregateCall can hold it.
struct Expr;

// op is one of: "=", "!=", "<", ">", "<=", ">=", "AND", "OR"
struct BinaryOp {
    std::unique_ptr<Expr> left;
    std::string           op;
    std::unique_ptr<Expr> right;
};

struct UnaryOp {
    std::string           op;      // "NOT"
    std::unique_ptr<Expr> operand;
};

// arg is either a real Expr or the bare star "*" (for COUNT(*))
struct AggregateCall {
    AggFunc                        func;
    bool                           is_star; // true  → COUNT(*)
    std::unique_ptr<Expr>          arg;     // non-null when is_star == false
};

// The Expr variant itself
struct Expr {
    std::variant<ColumnRef, Literal, BinaryOp, UnaryOp, AggregateCall> node;
};

// ── Helper constructors (so callers don't have to write variant noise) ────────
inline Expr makeColumnRef(std::optional<std::string> table, std::string col) {
    return Expr{ColumnRef{std::move(table), std::move(col)}};
}
inline Expr makeLiteral(LiteralValue v) {
    return Expr{Literal{std::move(v)}};
}
inline Expr makeNullLiteral() {
    return Expr{Literal{std::monostate{}}};
}
inline Expr makeBinaryOp(Expr left, std::string op, Expr right) {
    return Expr{BinaryOp{
        std::make_unique<Expr>(std::move(left)),
        std::move(op),
        std::make_unique<Expr>(std::move(right))
    }};
}
inline Expr makeUnaryOp(std::string op, Expr operand) {
    return Expr{UnaryOp{
        std::move(op),
        std::make_unique<Expr>(std::move(operand))
    }};
}
inline Expr makeAggregate(AggFunc func, bool is_star, Expr arg = Expr{Literal{std::monostate{}}}) {
    return Expr{AggregateCall{
        func,
        is_star,
        is_star ? nullptr : std::make_unique<Expr>(std::move(arg))
    }};
}

// ── Deep-copy helper ──────────────────────────────────────────────────────────
//
// Expr is non-copyable: std::variant disables copy when any alternative
// contains a std::unique_ptr (BinaryOp, UnaryOp, AggregateCall do).
// Use cloneExpr() whenever you need a true value copy of an Expr.
//
inline Expr cloneExpr(const Expr& src);  // forward declaration (recursive)

inline Expr cloneExpr(const Expr& src) {
    return std::visit([](const auto& node) -> Expr {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ColumnRef>) {
            return Expr{node};  // ColumnRef has no unique_ptr — trivially copyable
        }
        if constexpr (std::is_same_v<T, Literal>) {
            return Expr{node};  // LiteralValue has no unique_ptr
        }
        if constexpr (std::is_same_v<T, BinaryOp>) {
            return makeBinaryOp(cloneExpr(*node.left), node.op, cloneExpr(*node.right));
        }
        if constexpr (std::is_same_v<T, UnaryOp>) {
            return makeUnaryOp(node.op, cloneExpr(*node.operand));
        }
        if constexpr (std::is_same_v<T, AggregateCall>) {
            if (node.is_star) return makeAggregate(node.func, true);
            return makeAggregate(node.func, false, cloneExpr(*node.arg));
        }
        return Expr{Literal{std::monostate{}}};  // unreachable
    }, src.node);
}

// ── Column definition (inside CREATE TABLE) ───────────────────────────────────
struct ColumnDef {
    std::string name;
    DataType    type;
    bool        is_primary_key = false;
    bool        is_not_null    = false;
};

// ── Table reference (FROM clause or JOIN) ─────────────────────────────────────
struct TableRef {
    std::string                table_name;
    std::optional<std::string> alias; // e.g. "u" in "FROM users u"
};

// ── JOIN clause ───────────────────────────────────────────────────────────────
struct JoinClause {
    TableRef table;
    Expr     on; // join condition, e.g. orders.user_id = users.id
};

// ── ORDER BY item ─────────────────────────────────────────────────────────────
struct OrderByItem {
    std::string column;
    bool        ascending = true; // false → DESC
};

// ── Statement node types ─────────────────────────────────────────────────────

struct CreateTableStmt {
    std::string            table_name;
    std::vector<ColumnDef> columns;
};

struct CreateIndexStmt {
    std::string index_name;
    std::string table_name;
    std::string column_name;
};

struct InsertStmt {
    std::string          table_name;
    std::vector<Literal> values; // positional — matches column order in catalog
};

struct SelectStmt {
    // columns: ["*"] or explicit column names, may be qualified as "table.column".
    // Aggregate expressions are stored separately in aggregate_columns.
    std::vector<std::string>  columns;

    // Aggregate expressions appearing in the SELECT list, in SELECT-list order.
    // Examples:
    //   COUNT(*)
    //   SUM(salary)
    //   AVG(age)
    std::vector<AggregateCall> aggregate_columns;

    std::vector<TableRef>     from;
    std::vector<JoinClause>   joins;
    std::optional<Expr>       where;
    std::optional<std::vector<std::string>> group_by;
    std::optional<Expr>                      having;
    std::optional<std::vector<OrderByItem>>  order_by;
    std::optional<int64_t>                   limit;
};
struct Assignment {
    std::string column;
    Expr        value;
};

struct UpdateStmt {
    std::string             table_name;
    std::vector<Assignment> updates;
    std::optional<Expr>     where;
};

struct DeleteStmt {
    std::string         table_name;
    std::optional<Expr> where;
};

struct TxnStmt {
    enum class Kind { BEGIN, COMMIT, ROLLBACK } kind;
};

// ── Top-level Statement variant ───────────────────────────────────────────────
using Statement = std::variant<
    CreateTableStmt,
    CreateIndexStmt,
    InsertStmt,
    SelectStmt,
    UpdateStmt,
    DeleteStmt,
    TxnStmt
>;
