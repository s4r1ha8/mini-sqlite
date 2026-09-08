#pragma once

// ============================================================================
// planner.h  —  Query planner: validated SelectStmt → operator tree.
//
// Owner: Sarthak (frontend).
// Priyanshu's executor walks and runs this tree — see cursor_interface.md.
//
// The planner converts a semantically-checked AST into an operator tree using
// the Volcano/iterator model.  Each node is *plain data* — no open()/next()
// methods here.  Execution logic lives entirely in Priyanshu's executor.
//
// Planning decisions (v1):
//   • If the WHERE clause has an equality predicate on an indexed column,
//     use IndexScan; otherwise use SeqScan.
//   • Multi-table queries use NestedLoopJoin (the simplest correct join).
//   • WHERE is applied as a Filter on top of the scan/join.
//   • ORDER BY wraps with Sort.
//   • GROUP BY / aggregates wrap with Aggregate.
// ============================================================================

#include <memory>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>
#include "common/ast.h"
#include "frontend/catalog.h"

// ── OperatorNode — forward declaration ────────────────────────────────────────
//
// IMPORTANT: OperatorNode must be a *struct*, not a type alias.  The operator
// structs below (FilterOp, SortOp, …) hold std::unique_ptr<OperatorNode> as
// children.  unique_ptr<T> is valid with an *incomplete* T (it only needs T to
// be complete when the destructor is instantiated, which happens after the full
// definition below).  By contrast, std::variant<T, …> requires all T to be
// *complete* at the point of the alias definition — so a `using` alias here
// would be a compile error because the Op structs aren't defined yet.
struct OperatorNode;

// ── Leaf operators ────────────────────────────────────────────────────────────

// SeqScan — scan every row in a table sequentially.
struct SeqScanOp {
    std::string table_name;
    std::string alias;        // how this table is named in the query scope
};

// IndexScan — jump directly to rows matching key_value using an index B-tree,
// then do a primary-key lookup to get the full row.
// Used when WHERE has an equality predicate on an indexed column.
struct IndexScanOp {
    std::string table_name;
    std::string alias;
    std::string index_name;
    Expr        key_condition; // the equality expression that triggered index use
};

// ── Unary operators (one child) ───────────────────────────────────────────────

// Filter — pull rows from child, evaluate predicate, pass through matches.
struct FilterOp {
    std::unique_ptr<OperatorNode> child;
    Expr                          predicate;
};

// Sort — buffer all rows, sort by order_by, yield in order.
struct SortOp {
    std::unique_ptr<OperatorNode>  child;
    std::vector<OrderByItem>       order_by;
};

// Aggregate — group rows by group_by columns and compute aggregates.
// `having` is evaluated per completed group (post-aggregation filter).
struct AggregateOp {
    std::unique_ptr<OperatorNode>   child;
    std::vector<std::string>        group_by;
    std::vector<AggregateCall>      aggregates;
    std::optional<Expr>             having;    // optional post-group filter
};

// Project — select only the requested columns from the row produced by child.
struct ProjectOp {
    std::unique_ptr<OperatorNode>   child;
    std::vector<std::string>        columns; // ["*"] or qualified column names
};

// Limit — return at most limit_count rows.
struct LimitOp {
    std::unique_ptr<OperatorNode> child;
    int64_t                       limit_count;
};

// ── Binary operator (two children) ───────────────────────────────────────────

// NestedLoopJoin — for each row from left, re-scan right, emit condition matches.
struct NestedLoopJoinOp {
    std::unique_ptr<OperatorNode> left;
    std::unique_ptr<OperatorNode> right;
    Expr                          condition;
};

// ── OperatorNode — the full definition ───────────────────────────────────────
//
// All Op structs are now complete so the variant can be instantiated.
// The template constructor lets callers write:
//   std::make_unique<OperatorNode>(SeqScanOp{ "users", "users" })
// instead of the more verbose aggregate-initialisation syntax.

struct OperatorNode {
    std::variant<
        SeqScanOp,
        IndexScanOp,
        FilterOp,
        NestedLoopJoinOp,
        SortOp,
        AggregateOp,
        ProjectOp,
        LimitOp
    > v;

    // Converting constructor: OperatorNode(SeqScanOp{…}) works directly.
    // The enable_if guard prevents this from shadowing the copy/move constructors.
    template<typename T,
             typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, OperatorNode>>>
    OperatorNode(T&& val) : v(std::forward<T>(val)) {}
};

// ── Planner ───────────────────────────────────────────────────────────────────

class Planner {
public:
    explicit Planner(Catalog& catalog);

    // Build an operator tree for a validated SELECT statement.
    std::unique_ptr<OperatorNode> plan(const SelectStmt& stmt);

    // Pretty-print the operator tree (for REPL output / debugging).
    static std::string printTree(const OperatorNode& node, int indent = 0);

    // Convert an Expr to a compact display string (used by printTree).
    static std::string exprToString(const Expr& expr);

private:
    Catalog& catalog_;

    bool        hasIndexOn(const std::string& table,
                            const std::string& column) const;
    std::string indexName(const std::string& table,
                           const std::string& column) const;
    std::string tryFindIndexEquality(const Expr* expr,
                                     const std::string& table) const;

    std::unique_ptr<OperatorNode> buildScan(const TableRef& ref,
                                             const Expr* whereExpr);
    std::unique_ptr<OperatorNode> buildJoinSpine(const SelectStmt& stmt);
};
