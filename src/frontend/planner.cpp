// ============================================================================
// planner.cpp  —  Query planner implementation.
// ============================================================================

#include "frontend/planner.h"
#include <sstream>
#include <string>

// ── Constructor ───────────────────────────────────────────────────────────────

Planner::Planner(Catalog& catalog) : catalog_(catalog) {}

// ── Public entry point ────────────────────────────────────────────────────────

std::unique_ptr<OperatorNode> Planner::plan(const SelectStmt& stmt) {
    // 1. Build the base scan / join spine
    std::unique_ptr<OperatorNode> root = buildJoinSpine(stmt);

    // 2. Wrap with Filter if there is a WHERE clause
    if (stmt.where.has_value()) {
        // Even when IndexScan is used we still emit a Filter for correctness
        // in v1 — the executor can optimise away redundant filter checks later.
        root = std::make_unique<OperatorNode>(
            FilterOp{ std::move(root), cloneExpr(*stmt.where) });
    }

     // 3. Wrap with Aggregate if GROUP BY is present or the SELECT list
    // contains aggregate expressions.
    if (stmt.group_by.has_value() ||
        !stmt.aggregate_columns.empty()) {

        std::vector<AggregateCall> aggs;
        aggs.reserve(stmt.aggregate_columns.size());

        for (const auto& aggregate : stmt.aggregate_columns) {
            if (aggregate.is_star) {
                aggs.push_back(
                    AggregateCall{
                        aggregate.func,
                        true,
                        nullptr
                    }
                );
            } else {
                aggs.push_back(
                    AggregateCall{
                        aggregate.func,
                        false,
                        aggregate.arg
                            ? std::make_unique<Expr>(
                                  cloneExpr(*aggregate.arg))
                            : nullptr
                    }
                );
            }
        }

        std::optional<Expr> having;

        if (stmt.having.has_value()) {
            having = cloneExpr(*stmt.having);
        }

        std::vector<std::string> group_by;

        if (stmt.group_by.has_value()) {
            group_by = *stmt.group_by;
        }

        root = std::make_unique<OperatorNode>(
            AggregateOp{
                std::move(root),
                std::move(group_by),
                std::move(aggs),
                std::move(having)
            }
        );
    }
    // 4. Wrap with Sort if ORDER BY is present
    if (stmt.order_by.has_value()) {
        root = std::make_unique<OperatorNode>(
            SortOp{ std::move(root), *stmt.order_by });
    }

    // 5. Always wrap with Project (selects requested columns / handles *)
    root = std::make_unique<OperatorNode>(
        ProjectOp{ std::move(root), stmt.columns });

    return root;
}

// ── Join spine builder ────────────────────────────────────────────────────────

std::unique_ptr<OperatorNode>
Planner::buildJoinSpine(const SelectStmt& stmt) {
    const Expr* wherePtr = stmt.where.has_value() ? &(*stmt.where) : nullptr;
    std::unique_ptr<OperatorNode> current = buildScan(stmt.from[0], wherePtr);

    for (const auto& jc : stmt.joins) {
        // Pass nullptr for WHERE on the right side of a join;
        // the WHERE Filter wraps the whole join output.
        std::unique_ptr<OperatorNode> rightScan = buildScan(jc.table, nullptr);
        current = std::make_unique<OperatorNode>(
            NestedLoopJoinOp{
                std::move(current),
                std::move(rightScan),
                cloneExpr(jc.on)
            });
    }
    return current;
}

// ── Scan builder ─────────────────────────────────────────────────────────────

std::unique_ptr<OperatorNode>
Planner::buildScan(const TableRef& ref, const Expr* whereExpr) {
    std::string alias  = ref.alias.value_or(ref.table_name);
    std::string indexN = whereExpr
        ? tryFindIndexEquality(whereExpr, ref.table_name)
        : "";

    if (!indexN.empty()) {
        return std::make_unique<OperatorNode>(
            IndexScanOp{ ref.table_name, alias, indexN, cloneExpr(*whereExpr) });
    }
    return std::make_unique<OperatorNode>(
        SeqScanOp{ ref.table_name, alias });
}

// ── Index detection helpers ───────────────────────────────────────────────────

bool Planner::hasIndexOn(const std::string& table,
                          const std::string& column) const {
    const TableSchema* schema = catalog_.get_table(table);
    if (!schema) return false;
    for (const auto& idx : schema->indexes)
        if (idx.column == column) return true;
    return false;
}

std::string Planner::indexName(const std::string& table,
                                const std::string& column) const {
    const TableSchema* schema = catalog_.get_table(table);
    if (!schema) return "";
    for (const auto& idx : schema->indexes)
        if (idx.column == column) return idx.name;
    return "";
}

// Inspect `expr` for a simple "col = literal" or "literal = col" pattern
// where `col` belongs to `table` and has an index.
// Returns the index name on success, empty string otherwise.
std::string Planner::tryFindIndexEquality(const Expr* expr,
                                           const std::string& table) const {
    if (!expr) return "";
    const auto* binop = std::get_if<BinaryOp>(&expr->node);
    if (!binop || binop->op != "=") return "";

    // Case 1: column = literal
    const auto* leftCol  = std::get_if<ColumnRef>(&binop->left->node);
    const auto* rightLit = std::get_if<Literal>(&binop->right->node);
    if (leftCol && rightLit) {
        bool tableMatch = !leftCol->table.has_value() ||
                          *leftCol->table == table;
        if (tableMatch && hasIndexOn(table, leftCol->column))
            return indexName(table, leftCol->column);
    }

    // Case 2: literal = column  (commutative)
    const auto* leftLit  = std::get_if<Literal>(&binop->left->node);
    const auto* rightCol = std::get_if<ColumnRef>(&binop->right->node);
    if (leftLit && rightCol) {
        bool tableMatch = !rightCol->table.has_value() ||
                          *rightCol->table == table;
        if (tableMatch && hasIndexOn(table, rightCol->column))
            return indexName(table, rightCol->column);
    }

    return "";
}

// ── exprToString ─────────────────────────────────────────────────────────────

// Helper: overloaded visitor pattern (C++17, no extra header needed)
template<typename... Ts>
struct Overloaded : Ts... { using Ts::operator()...; };
template<typename... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

std::string Planner::exprToString(const Expr& expr) {
    return std::visit(Overloaded{
        [](const ColumnRef& node) -> std::string {
            return node.table.has_value()
                ? *node.table + "." + node.column
                : node.column;
        },
        [](const Literal& node) -> std::string {
            return std::visit(Overloaded{
                [](std::monostate)        -> std::string { return "NULL"; },
                [](int64_t v)             -> std::string { return std::to_string(v); },
                [](double v)              -> std::string { return std::to_string(v); },
                [](const std::string& v)  -> std::string { return "'" + v + "'"; }
            }, node.value);
        },
        [](const BinaryOp& node) -> std::string {
            return "(" + Planner::exprToString(*node.left)
                 + " " + node.op
                 + " " + Planner::exprToString(*node.right) + ")";
        },
        [](const UnaryOp& node) -> std::string {
            return node.op + " " + Planner::exprToString(*node.operand);
        },
        [](const AggregateCall& node) -> std::string {
            std::string fname = aggFuncToString(node.func);
            if (node.is_star) return fname + "(*)" ;
            return fname + "(" + Planner::exprToString(*node.arg) + ")";
        }
    }, expr.node);
}

// ── printTree — pretty-printer ────────────────────────────────────────────────
//
// Produces output like:
//   Project([*])
//   └─ Sort([name DESC])
//      └─ Filter((id = 5))
//         └─ SeqScan(users)
//
// `connector` is the prefix that appears before this node's header line.
// `childIndent` is the indentation string used for this node's children's
//               connectors (grows by 3 spaces at each level).
//
// The recursion works outside the class (file-static helper) so it doesn't
// need the Planner:: qualifier inside lambdas — the helper calls itself
// and Planner::exprToString directly.

static std::string printNode(const OperatorNode& node,
                              const std::string& connector,
                              const std::string& childIndent) {
    std::ostringstream out;

    // Helper: emit one child with the right connector and updated indent.
    auto printChild = [&](const OperatorNode& child,
                          const std::string& conn,
                          const std::string& nextIndent) {
        out << printNode(child, childIndent + conn, nextIndent);
    };

    std::visit([&](const auto& op) {
        using T = std::decay_t<decltype(op)>;

        if constexpr (std::is_same_v<T, SeqScanOp>) {
            out << connector << "SeqScan(" << op.table_name;
            if (op.alias != op.table_name) out << " as " << op.alias;
            out << ")\n";
        }
        else if constexpr (std::is_same_v<T, IndexScanOp>) {
            out << connector << "IndexScan(" << op.table_name
                << ", idx=" << op.index_name
                << ", key=" << Planner::exprToString(op.key_condition)
                << ")\n";
        }
        else if constexpr (std::is_same_v<T, FilterOp>) {
            out << connector << "Filter("
                << Planner::exprToString(op.predicate) << ")\n";
            printChild(*op.child, "└─ ", childIndent + "   ");
        }
        else if constexpr (std::is_same_v<T, SortOp>) {
            out << connector << "Sort([";
            for (size_t i = 0; i < op.order_by.size(); ++i) {
                if (i) out << ", ";
                out << op.order_by[i].column
                    << (op.order_by[i].ascending ? " ASC" : " DESC");
            }
            out << "])\n";
            printChild(*op.child, "└─ ", childIndent + "   ");
        }
        else if constexpr (std::is_same_v<T, AggregateOp>) {
            out << connector << "Aggregate(group_by=[";
            for (size_t i = 0; i < op.group_by.size(); ++i) {
                if (i) out << ", ";
                out << op.group_by[i];
            }
            out << "]";
            if (op.having.has_value())
                out << ", having=" << Planner::exprToString(*op.having);
            out << ")\n";
            printChild(*op.child, "└─ ", childIndent + "   ");
        }
        else if constexpr (std::is_same_v<T, ProjectOp>) {
            out << connector << "Project([";
            for (size_t i = 0; i < op.columns.size(); ++i) {
                if (i) out << ", ";
                out << op.columns[i];
            }
            out << "])\n";
            printChild(*op.child, "└─ ", childIndent + "   ");
        }
        else if constexpr (std::is_same_v<T, NestedLoopJoinOp>) {
            out << connector << "NestedLoopJoin(on: "
                << Planner::exprToString(op.condition) << ")\n";
            // Left child: more children follow → use "├─ " and "│  " indent.
            printChild(*op.left,  "├─ ", childIndent + "│  ");
            // Right child: last child → use "└─ " and "   " indent.
            printChild(*op.right, "└─ ", childIndent + "   ");
        }
    }, node.v);  // note: node.v, not node (OperatorNode is a wrapper struct)

    return out.str();
}

std::string Planner::printTree(const OperatorNode& node, int /*indent*/) {
    // Root node has no connector prefix and no child indent yet.
    return printNode(node, "", "");
}
