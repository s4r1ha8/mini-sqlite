#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <variant>
#include <utility>
#include <type_traits>

#include "common/cursor_interface.h"
#include "common/eval.h"

#include "frontend/planner.h"
#include "frontend/catalog.h"

#include "storage/pager.h"

#include "exec/seq_scan.h"
#include "exec/filter.h"
#include "exec/sort.h"
#include "exec/index_scan.h"
#include "exec/nested_loop_join.h"
#include "exec/aggregate.h"


// ============================================================================
// Runtime Project operator
// ============================================================================

class Project : public Operator {
public:
    Project(
        OperatorPtr child,
        std::vector<size_t> columns
    )
        : child_(std::move(child)),
          columns_(std::move(columns)) {}

    void open() override {
        child_->open();
    }

    std::optional<Row> next() override {
        auto row = child_->next();

        if (!row.has_value()) {
            return std::nullopt;
        }

        Row result;
        result.reserve(columns_.size());

        for (size_t index : columns_) {
            if (index >= row->size()) {
                throw std::runtime_error(
                    "Project: column index out of range"
                );
            }

            result.push_back((*row)[index]);
        }

        return result;
    }

    void close() override {
        child_->close();
    }

private:
    OperatorPtr child_;
    std::vector<size_t> columns_;
};


// ============================================================================
// ExecutorAdapter
// ============================================================================

class ExecutorAdapter {
public:
    ExecutorAdapter(Pager& pager, Catalog& catalog)
        : pager_(pager),
          catalog_(catalog) {}

    OperatorPtr build(const OperatorNode& node) {
        return std::visit(
            [this](const auto& op) -> OperatorPtr {
                return buildNode(op);
            },
            node.v
        );
    }


private:
    Pager& pager_;
    Catalog& catalog_;


    // ========================================================================
    // SeqScan
    // ========================================================================

    OperatorPtr buildNode(const SeqScanOp& op) {
        const TableSchema* schema =
            catalog_.get_table(op.table_name);

        if (!schema) {
            throw std::runtime_error(
                "ExecutorAdapter: table '" +
                op.table_name +
                "' does not exist"
            );
        }

        if (schema->root_page_id < 0) {
            throw std::runtime_error(
                "ExecutorAdapter: table '" +
                op.table_name +
                "' has no root page"
            );
        }

        return std::make_unique<SeqScan>(
            pager_,
            static_cast<PageId>(schema->root_page_id),
            schema->columns.size()
        );
    }


    // ========================================================================
    // IndexScan
    // ========================================================================

    OperatorPtr buildNode(const IndexScanOp& op) {
        const TableSchema* schema =
            catalog_.get_table(op.table_name);

        if (!schema) {
            throw std::runtime_error(
                "ExecutorAdapter: table '" +
                op.table_name +
                "' does not exist"
            );
        }

        if (schema->root_page_id < 0) {
            throw std::runtime_error(
                "ExecutorAdapter: table '" +
                op.table_name +
                "' has no root page"
            );
        }

        const IndexInfo* index = nullptr;

        for (const auto& candidate : schema->indexes) {
            if (candidate.name == op.index_name) {
                index = &candidate;
                break;
            }
        }

        if (!index) {
            throw std::runtime_error(
                "ExecutorAdapter: index '" +
                op.index_name +
                "' not found on table '" +
                op.table_name +
                "'"
            );
        }

        if (index->root_page_id < 0) {
            throw std::runtime_error(
                "ExecutorAdapter: index '" +
                op.index_name +
                "' has no root page"
            );
        }

        const auto* binary =
            std::get_if<BinaryOp>(&op.key_condition.node);

        if (!binary || binary->op != "=") {
            throw std::runtime_error(
                "ExecutorAdapter: IndexScan requires equality predicate"
            );
        }

        const Literal* literal =
            std::get_if<Literal>(&binary->left->node);

        if (!literal) {
            literal =
                std::get_if<Literal>(&binary->right->node);
        }

        if (!literal) {
            throw std::runtime_error(
                "ExecutorAdapter: IndexScan key condition "
                "must contain a literal"
            );
        }

        Value lookup_value =
            std::visit(
                [](const auto& value) -> Value {
                    return Value(value);
                },
                literal->value
            );

        return std::make_unique<IndexScan>(
            pager_,
            static_cast<PageId>(schema->root_page_id),
            static_cast<PageId>(index->root_page_id),
            schema->columns.size(),
            std::move(lookup_value)
        );
    }


    // ========================================================================
    // Filter
    // ========================================================================

    OperatorPtr buildNode(const FilterOp& op) {
        OperatorPtr child = build(*op.child);

        const TableSchema* schema =
            schemaForNode(*op.child);

        if (!schema) {
            throw std::runtime_error(
                "ExecutorAdapter: cannot determine schema for Filter"
            );
        }

        auto predicate =
            std::make_shared<Expr>(
                cloneExpr(op.predicate)
            );

        auto schema_copy =
            std::make_shared<TableSchema>(*schema);

        return std::make_unique<Filter>(
            std::move(child),
            [predicate, schema_copy](const Row& row) {
                return evalBool(
                    *predicate,
                    row,
                    *schema_copy
                );
            }
        );
    }


    // ========================================================================
    // Sort
    // ========================================================================

    OperatorPtr buildNode(const SortOp& op) {
        OperatorPtr child = build(*op.child);

        const TableSchema* schema =
            schemaForNode(*op.child);

        if (!schema) {
            throw std::runtime_error(
                "ExecutorAdapter: cannot determine schema for Sort"
            );
        }

        std::vector<SortKey> keys;
        keys.reserve(op.order_by.size());

        for (const auto& item : op.order_by) {
            std::string column_name = item.column;

            const size_t dot =
                column_name.find('.');

            if (dot != std::string::npos) {
                column_name =
                    column_name.substr(dot + 1);
            }

            bool found = false;

            for (const auto& column : schema->columns) {
                if (column.name == column_name) {
                    keys.push_back(
                        SortKey{
                            static_cast<size_t>(column.ordinal),
                            !item.ascending
                        }
                    );

                    found = true;
                    break;
                }
            }

            if (!found) {
                throw std::runtime_error(
                    "ExecutorAdapter: ORDER BY column '" +
                    item.column +
                    "' not found"
                );
            }
        }

        return std::make_unique<Sort>(
            std::move(child),
            std::move(keys)
        );
    }


    // ========================================================================
    // Project
    // ========================================================================

    OperatorPtr buildNode(const ProjectOp& op) {
        OperatorPtr child = build(*op.child);

        const TableSchema* schema =
            schemaForNode(*op.child);

        if (!schema) {
            throw std::runtime_error(
                "ExecutorAdapter: cannot determine schema for Project"
            );
        }

        std::vector<size_t> columns;

        // SELECT *
        if (op.columns.size() == 1 &&
            op.columns[0] == "*") {

            columns.reserve(schema->columns.size());

            for (const auto& column : schema->columns) {
                columns.push_back(
                    static_cast<size_t>(column.ordinal)
                );
            }

        } else {

            columns.reserve(op.columns.size());

            for (const auto& requested : op.columns) {
                std::string column_name =
                    requested;

                const size_t dot =
                    column_name.find('.');

                if (dot != std::string::npos) {
                    column_name =
                        column_name.substr(dot + 1);
                }

                bool found = false;

                for (const auto& column : schema->columns) {
                    if (column.name == column_name) {
                        columns.push_back(
                            static_cast<size_t>(
                                column.ordinal
                            )
                        );

                        found = true;
                        break;
                    }
                }

                if (!found) {
                    throw std::runtime_error(
                        "ExecutorAdapter: SELECT column '" +
                        requested +
                        "' not found"
                    );
                }
            }
        }

        return std::make_unique<Project>(
            std::move(child),
            std::move(columns)
        );
    }


    // ========================================================================
    // Aggregate
    // ========================================================================

    OperatorPtr buildNode(const AggregateOp& op) {
        OperatorPtr child =
            build(*op.child);

        const TableSchema* schema =
            schemaForNode(*op.child);

        if (!schema) {
            throw std::runtime_error(
                "ExecutorAdapter: cannot determine schema for Aggregate"
            );
        }


        // --------------------------------------------------------------------
        // GROUP BY columns
        // --------------------------------------------------------------------

        std::vector<size_t> group_by_cols;
        group_by_cols.reserve(op.group_by.size());

        for (const auto& name : op.group_by) {

            std::string column_name = name;

            const size_t dot =
                column_name.find('.');

            if (dot != std::string::npos) {
                column_name =
                    column_name.substr(dot + 1);
            }

            bool found = false;

            for (const auto& column : schema->columns) {
                if (column.name == column_name) {

                    group_by_cols.push_back(
                        static_cast<size_t>(
                            column.ordinal
                        )
                    );

                    found = true;
                    break;
                }
            }

            if (!found) {
                throw std::runtime_error(
                    "ExecutorAdapter: GROUP BY column '" +
                    name +
                    "' not found"
                );
            }
        }


        // --------------------------------------------------------------------
        // Aggregate calls
        // --------------------------------------------------------------------

        std::vector<::AggCall> agg_calls;
        agg_calls.reserve(op.aggregates.size());

        for (const auto& aggregate : op.aggregates) {

            // COUNT(*)
            if (aggregate.is_star) {

                if (aggregate.func != AggFunc::COUNT) {
                    throw std::runtime_error(
                        "ExecutorAdapter: '*' is only valid for COUNT"
                    );
                }

                /*
                 * Runtime COUNT is row-count based, so the actual
                 * source column is irrelevant. Runtime aggregate currently
                 * expects a column index, therefore column 0 is used.
                 */
                agg_calls.push_back(
                    ::AggCall{
                        RuntimeAggFunc::Count,
                        0
                    }
                );

                continue;
            }


            // ---------------------------------------------------------------
            // Non-star aggregates must have a column argument
            // ---------------------------------------------------------------

            if (!aggregate.arg) {
                throw std::runtime_error(
                    "ExecutorAdapter: aggregate argument is missing"
                );
            }

            const auto* arg =
                std::get_if<ColumnRef>(
                    &aggregate.arg->node
                );

            if (!arg) {
                throw std::runtime_error(
                    "ExecutorAdapter: aggregate argument "
                    "must be a column"
                );
            }


            std::string column_name =
                arg->column;

            bool found = false;

            for (const auto& column : schema->columns) {

                if (column.name != column_name) {
                    continue;
                }

                RuntimeAggFunc runtime_func;

                switch (aggregate.func) {

                    case AggFunc::COUNT:
                        runtime_func =
                            RuntimeAggFunc::Count;
                        break;

                    case AggFunc::SUM:
                        runtime_func =
                            RuntimeAggFunc::Sum;
                        break;

                    case AggFunc::AVG:
                        runtime_func =
                            RuntimeAggFunc::Avg;
                        break;

                    case AggFunc::MIN:
                        runtime_func =
                            RuntimeAggFunc::Min;
                        break;

                    case AggFunc::MAX:
                        runtime_func =
                            RuntimeAggFunc::Max;
                        break;
                }

                agg_calls.push_back(
                    ::AggCall{
                        runtime_func,
                        static_cast<size_t>(
                            column.ordinal
                        )
                    }
                );

                found = true;
                break;
            }


            if (!found) {
                throw std::runtime_error(
                    "ExecutorAdapter: aggregate column '" +
                    arg->column +
                    "' not found"
                );
            }
        }


        // --------------------------------------------------------------------
        // Create runtime Aggregate operator
        // --------------------------------------------------------------------

        return std::make_unique<Aggregate>(
            std::move(child),
            std::move(group_by_cols),
            std::move(agg_calls)
        );
    }


    // ========================================================================
    // NestedLoopJoin
    // ========================================================================

    OperatorPtr buildNode(
        const NestedLoopJoinOp& op
    ) {

        OperatorPtr left =
            build(*op.left);

        OperatorPtr right =
            build(*op.right);


        const TableSchema* left_schema =
            schemaForNode(*op.left);

        const TableSchema* right_schema =
            schemaForNode(*op.right);

        if (!left_schema || !right_schema) {
            throw std::runtime_error(
                "ExecutorAdapter: cannot determine JOIN child schemas"
            );
        }


        const std::string left_alias =
            nodeAlias(*op.left);

        const std::string right_alias =
            nodeAlias(*op.right);


        auto combined =
            std::make_shared<TableSchema>(
                makeCombinedSchema(
                    *left_schema,
                    left_alias,
                    *right_schema,
                    right_alias
                )
            );


        auto condition =
            std::make_shared<Expr>(
                cloneExpr(op.condition)
            );


        return std::make_unique<NestedLoopJoin>(
            std::move(left),
            std::move(right),
            [condition, combined](const Row& row) {

                return evalBool(
                    *condition,
                    row,
                    *combined
                );
            }
        );
    }


    // ========================================================================
    // Unsupported operators
    // ========================================================================

    template <typename T>
    OperatorPtr buildNode(const T&) {
        throw std::runtime_error(
            "ExecutorAdapter: operator not implemented yet"
        );
    }


    // ========================================================================
    // Find schema represented by a planner node
    // ========================================================================

    const TableSchema* schemaForNode(
        const OperatorNode& node
    ) const {

        return std::visit(
            [this](const auto& op) -> const TableSchema* {

                using T =
                    std::decay_t<decltype(op)>;


                if constexpr (
                    std::is_same_v<T, SeqScanOp>
                ) {

                    return catalog_.get_table(
                        op.table_name
                    );


                } else if constexpr (
                    std::is_same_v<T, IndexScanOp>
                ) {

                    return catalog_.get_table(
                        op.table_name
                    );


                } else if constexpr (
                    std::is_same_v<T, FilterOp>
                ) {

                    return schemaForNode(
                        *op.child
                    );


                } else if constexpr (
                    std::is_same_v<T, SortOp>
                ) {

                    return schemaForNode(
                        *op.child
                    );


                } else if constexpr (
                    std::is_same_v<T, ProjectOp>
                ) {

                    return schemaForNode(
                        *op.child
                    );


                } else if constexpr (
                    std::is_same_v<T, AggregateOp>
                ) {

                    /*
                     * For now the runtime Aggregate consumes the child's
                     * schema. A full output-schema layer will be added
                     * later so expressions after aggregation can resolve
                     * aggregate output columns correctly.
                     */
                    return schemaForNode(
                        *op.child
                    );


                } else {

                    /*
                     * JOIN currently builds its own combined schema in
                     * buildNode(NestedLoopJoinOp).
                     */
                    return nullptr;
                }
            },
            node.v
        );
    }


    // ========================================================================
    // Determine query-visible alias of a node
    // ========================================================================

    std::string nodeAlias(
        const OperatorNode& node
    ) const {

        return std::visit(
            [this](const auto& op) -> std::string {

                using T =
                    std::decay_t<decltype(op)>;


                if constexpr (
                    std::is_same_v<T, SeqScanOp>
                ) {

                    return op.alias.empty()
                        ? op.table_name
                        : op.alias;


                } else if constexpr (
                    std::is_same_v<T, IndexScanOp>
                ) {

                    return op.alias.empty()
                        ? op.table_name
                        : op.alias;


                } else if constexpr (
                    std::is_same_v<T, FilterOp>
                ) {

                    return nodeAlias(
                        *op.child
                    );


                } else if constexpr (
                    std::is_same_v<T, SortOp>
                ) {

                    return nodeAlias(
                        *op.child
                    );


                } else if constexpr (
                    std::is_same_v<T, ProjectOp>
                ) {

                    return nodeAlias(
                        *op.child
                    );


                } else {

                    throw std::runtime_error(
                        "ExecutorAdapter: cannot determine node alias"
                    );
                }
            },
            node.v
        );
    }


    // ========================================================================
    // Build temporary schema for:
    //
    //     [left row columns] + [right row columns]
    //
    // Qualified names are used so expression evaluation can distinguish
    // duplicate column names:
    //
    //     users.id
    //     users.name
    //     orders.id
    //     orders.user_id
    // ========================================================================

    static TableSchema makeCombinedSchema(
        const TableSchema& left,
        const std::string& left_alias,
        const TableSchema& right,
        const std::string& right_alias
    ) {

        TableSchema result;

        result.name =
            left_alias +
            "_JOIN_" +
            right_alias;

        result.primary_key.clear();
        result.root_page_id = -1;


        int ordinal = 0;


        // --------------------------------------------------------------------
        // Left table
        // --------------------------------------------------------------------

        for (const auto& column : left.columns) {

            ColumnInfo copy =
                column;

            copy.name =
                left_alias +
                "." +
                column.name;

            copy.ordinal =
                ordinal++;

            result.columns.push_back(
                std::move(copy)
            );
        }


        // --------------------------------------------------------------------
        // Right table
        // --------------------------------------------------------------------

        for (const auto& column : right.columns) {

            ColumnInfo copy =
                column;

            copy.name =
                right_alias +
                "." +
                column.name;

            copy.ordinal =
                ordinal++;

            result.columns.push_back(
                std::move(copy)
            );
        }


        return result;
    }
};