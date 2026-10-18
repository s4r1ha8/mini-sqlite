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
#include "exec/limit.h"

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
    ExecutorAdapter(
        Pager& pager,
        Catalog& catalog
    )
        : pager_(pager),
          catalog_(catalog) {}

    OperatorPtr build(
        const OperatorNode& node
    ) {
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

    OperatorPtr buildNode(
        const SeqScanOp& op
    ) {
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
            static_cast<PageId>(
                schema->root_page_id
            ),
            schema->columns.size()
        );
    }


    // ========================================================================
    // IndexScan
    // ========================================================================

    OperatorPtr buildNode(
        const IndexScanOp& op
    ) {
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

        for (const auto& candidate :
             schema->indexes) {

            if (candidate.name ==
                op.index_name) {

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
            std::get_if<BinaryOp>(
                &op.key_condition.node
            );

        if (!binary ||
            binary->op != "=") {

            throw std::runtime_error(
                "ExecutorAdapter: IndexScan "
                "requires equality predicate"
            );
        }

        const Literal* literal =
            std::get_if<Literal>(
                &binary->left->node
            );

        if (!literal) {
            literal =
                std::get_if<Literal>(
                    &binary->right->node
                );
        }

        if (!literal) {
            throw std::runtime_error(
                "ExecutorAdapter: IndexScan key "
                "condition must contain a literal"
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
            static_cast<PageId>(
                schema->root_page_id
            ),
            static_cast<PageId>(
                index->root_page_id
            ),
            schema->columns.size(),
            std::move(lookup_value)
        );
    }


    // ========================================================================
    // Filter
    // ========================================================================

    OperatorPtr buildNode(
        const FilterOp& op
    ) {
        OperatorPtr child =
            build(*op.child);

        auto schema =
            schemaForNode(*op.child);

        if (!schema) {
            throw std::runtime_error(
                "ExecutorAdapter: cannot determine "
                "schema for Filter"
            );
        }

        auto predicate =
            std::make_shared<Expr>(
                cloneExpr(op.predicate)
            );

        auto schema_copy =
            std::make_shared<TableSchema>(
                *schema
            );

        return std::make_unique<Filter>(
            std::move(child),
            [predicate, schema_copy](
                const Row& row
            ) {
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

    OperatorPtr buildNode(
        const SortOp& op
    ) {
        OperatorPtr child =
            build(*op.child);

        auto schema =
            schemaForNode(*op.child);

        if (!schema) {
            throw std::runtime_error(
                "ExecutorAdapter: cannot determine "
                "schema for Sort"
            );
        }

        std::vector<OrderByItem> cloned_order_by;
        cloned_order_by.reserve(op.order_by.size());
        for (const auto& item : op.order_by) {
            cloned_order_by.push_back({cloneExpr(item.expr), item.ascending});
        }

        return std::make_unique<Sort>(
            std::move(child),
            std::move(cloned_order_by),
            std::move(schema)
        );
    }


    // ========================================================================
    // Project
    // ========================================================================

    OperatorPtr buildNode(
        const ProjectOp& op
    ) {
        OperatorPtr child =
            build(*op.child);

        auto schema =
            schemaForNode(*op.child);

        if (!schema) {
            throw std::runtime_error(
                "ExecutorAdapter: cannot determine "
                "schema for Project"
            );
        }

        std::vector<size_t> columns;

        // --------------------------------------------------------------------
        // SELECT *
        // --------------------------------------------------------------------

        if (op.columns.size() == 1 &&
            op.columns[0] == "*") {

            columns.reserve(
                schema->columns.size()
            );

            for (const auto& column :
                 schema->columns) {

                columns.push_back(
                    static_cast<size_t>(
                        column.ordinal
                    )
                );
            }

        } else {

            // ----------------------------------------------------------------
            // Explicit projection
            // ----------------------------------------------------------------

            columns.reserve(
                op.columns.size()
            );

            for (const auto& requested :
                 op.columns) {

                int ordinal =
                    findColumnOrdinal(
                        *schema,
                        requested
                    );

                if (ordinal < 0) {
                    throw std::runtime_error(
                        "ExecutorAdapter: SELECT column '" +
                        requested +
                        "' not found"
                    );
                }

                columns.push_back(
                    static_cast<size_t>(
                        ordinal
                    )
                );
            }
        }

        return std::make_unique<Project>(
            std::move(child),
            std::move(columns)
        );
    }

    // ========================================================================
    // Limit
    // ========================================================================

    OperatorPtr buildNode(
        const LimitOp& op
    ) {
        OperatorPtr child =
            build(*op.child);

        return std::make_unique<Limit>(
            std::move(child),
            op.limit_count
        );
    }

    // ========================================================================
    // Aggregate
    // ========================================================================

    OperatorPtr buildNode(
        const AggregateOp& op
    ) {
        OperatorPtr child =
            build(*op.child);

        auto child_schema =
            schemaForNode(*op.child);

        if (!child_schema) {
            throw std::runtime_error(
                "ExecutorAdapter: cannot determine "
                "schema for Aggregate"
            );
        }

        // This is the schema of the rows produced by the Aggregate operator.
        // HAVING must be evaluated against this schema, not the input schema.
        auto aggregate_schema =
            std::make_shared<TableSchema>();

        aggregate_schema->name =
            child_schema->name +
            "_AGGREGATE";

        aggregate_schema->primary_key.clear();
        aggregate_schema->root_page_id = -1;

        int output_ordinal = 0;

        // --------------------------------------------------------------------
        // GROUP BY input column ordinals
        // --------------------------------------------------------------------

        std::vector<size_t> group_by_cols;
        group_by_cols.reserve(op.group_by.size());

        for (const auto& name :
             op.group_by) {

            int ordinal =
                findColumnOrdinal(
                    *child_schema,
                    name
                );

            if (ordinal < 0) {
                throw std::runtime_error(
                    "ExecutorAdapter: GROUP BY column '" +
                    name +
                    "' not found"
                );
            }

            group_by_cols.push_back(
                static_cast<size_t>(ordinal)
            );
        }

        // --------------------------------------------------------------------
        // GROUP BY output columns
        // --------------------------------------------------------------------

        for (const auto& name :
             op.group_by) {

            int ordinal =
                findColumnOrdinal(
                    *child_schema,
                    name
                );

            if (ordinal < 0) {
                throw std::runtime_error(
                    "ExecutorAdapter: GROUP BY column '" +
                    name +
                    "' not found"
                );
            }

            ColumnInfo column =
                child_schema->columns[
                    static_cast<size_t>(
                        ordinal
                    )
                ];

            column.ordinal =
                output_ordinal++;

            aggregate_schema->columns.push_back(
                std::move(column)
            );
        }

        // --------------------------------------------------------------------
        // Aggregate output columns
        // --------------------------------------------------------------------

        for (const auto& aggregate :
             op.aggregates) {

            ColumnInfo column;

            column.ordinal =
                output_ordinal++;

            column.is_not_null = false;

            if (aggregate.is_star) {

                if (aggregate.func !=
                    AggFunc::COUNT) {

                    throw std::runtime_error(
                        "ExecutorAdapter: '*' is only "
                        "valid for COUNT"
                    );
                }

                column.name =
                    "COUNT(*)";

                column.type =
                    DataType::INTEGER;

            } else {

                if (!aggregate.arg) {
                    throw std::runtime_error(
                        "ExecutorAdapter: aggregate "
                        "argument is missing"
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

                std::string arg_name =
                    arg->table.has_value()
                        ? *arg->table +
                          "." +
                          arg->column
                        : arg->column;

                switch (aggregate.func) {

                    case AggFunc::COUNT:
                        column.name =
                            "COUNT(" +
                            arg_name +
                            ")";

                        column.type =
                            DataType::INTEGER;
                        break;

                    case AggFunc::SUM:
                        column.name =
                            "SUM(" +
                            arg_name +
                            ")";

                        column.type =
                            DataType::REAL;
                        break;

                    case AggFunc::AVG:
                        column.name =
                            "AVG(" +
                            arg_name +
                            ")";

                        column.type =
                            DataType::REAL;
                        break;

                    case AggFunc::MIN:
                        column.name =
                            "MIN(" +
                            arg_name +
                            ")";

                        column.type =
                            DataType::REAL;
                        break;

                    case AggFunc::MAX:
                        column.name =
                            "MAX(" +
                            arg_name +
                            ")";

                        column.type =
                            DataType::REAL;
                        break;
                }
            }

            aggregate_schema->columns.push_back(
                std::move(column)
            );
        }

        // --------------------------------------------------------------------
        // Aggregate calls
        // --------------------------------------------------------------------

        std::vector<::AggCall> agg_calls;

        agg_calls.reserve(
            op.aggregates.size()
        );

        for (const auto& aggregate :
             op.aggregates) {

            // COUNT(*)
            if (aggregate.is_star) {

                if (aggregate.func !=
                    AggFunc::COUNT) {

                    throw std::runtime_error(
                        "ExecutorAdapter: '*' is only "
                        "valid for COUNT"
                    );
                }

                agg_calls.push_back(
                    ::AggCall{
                        RuntimeAggFunc::Count,
                        0
                    }
                );

                continue;
            }

            // Aggregate argument
            if (!aggregate.arg) {
                throw std::runtime_error(
                    "ExecutorAdapter: aggregate "
                    "argument is missing"
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

            int ordinal =
                findColumnOrdinal(
                    *child_schema,
                    arg->table.has_value()
                        ? *arg->table +
                          "." +
                          arg->column
                        : arg->column
                );

            if (ordinal < 0) {
                throw std::runtime_error(
                    "ExecutorAdapter: aggregate column '" +
                    arg->column +
                    "' not found"
                );
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
                        ordinal
                    )
                }
            );
        }

        // --------------------------------------------------------------------
        // HAVING
        // --------------------------------------------------------------------

        std::optional<Expr> having;

if (op.having.has_value()) {
    having =
        rewriteHavingForAggregate(
            *op.having,
            *aggregate_schema
        );
}

        return std::make_unique<Aggregate>(
            std::move(child),
            std::move(group_by_cols),
            std::move(agg_calls),
            std::move(having),
            std::move(aggregate_schema)
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

        auto left_schema =
            schemaForNode(
                *op.left
            );

        auto right_schema =
            schemaForNode(
                *op.right
            );

        if (!left_schema ||
            !right_schema) {

            throw std::runtime_error(
                "ExecutorAdapter: cannot determine "
                "JOIN child schemas"
            );
        }

        auto join_schema =
            std::make_shared<TableSchema>(
                combineSchemas(
                    *left_schema,
                    *right_schema
                )
            );

        auto condition =
            std::make_shared<Expr>(
                cloneExpr(op.condition)
            );

        return std::make_unique<NestedLoopJoin>(
            std::move(left),
            std::move(right),
            [condition, join_schema](
                const Row& row
            ) {
                return evalBool(
                    *condition,
                    row,
                    *join_schema
                );
            }
        );
    }


    // ========================================================================
    // Unsupported operators
    // ========================================================================

    template <typename T>
    OperatorPtr buildNode(
        const T&
    ) {
        throw std::runtime_error(
            "ExecutorAdapter: operator not implemented yet"
        );
    }


    // ========================================================================
    // Schema resolver
    //
    // Every returned schema describes the ACTUAL row layout emitted by that
    // operator.
    // ========================================================================

    std::shared_ptr<TableSchema> schemaForNode(
        const OperatorNode& node
    ) const {

        return std::visit(
            [this](const auto& op)
                -> std::shared_ptr<TableSchema> {

                using T =
                    std::decay_t<
                        decltype(op)
                    >;

                // ------------------------------------------------------------
                // SeqScan
                // ------------------------------------------------------------

                if constexpr (
                    std::is_same_v<
                        T,
                        SeqScanOp
                    >
                ) {

                    const TableSchema* base =
                        catalog_.get_table(
                            op.table_name
                        );

                    if (!base) {
                        return nullptr;
                    }

                    auto result =
                        std::make_shared<TableSchema>(
                            *base
                        );

                    result->root_page_id = -1;

                    const std::string alias =
                        op.alias.empty()
                            ? op.table_name
                            : op.alias;

                    for (
                        auto& column :
                        result->columns
                    ) {

                        column.name =
                            alias +
                            "." +
                            column.name;
                    }

                    return result;
                }

                // ------------------------------------------------------------
                // IndexScan
                // ------------------------------------------------------------

                else if constexpr (
                    std::is_same_v<
                        T,
                        IndexScanOp
                    >
                ) {

                    const TableSchema* base =
                        catalog_.get_table(
                            op.table_name
                        );

                    if (!base) {
                        return nullptr;
                    }

                    auto result =
                        std::make_shared<TableSchema>(
                            *base
                        );

                    result->root_page_id = -1;

                    const std::string alias =
                        op.alias.empty()
                            ? op.table_name
                            : op.alias;

                    for (
                        auto& column :
                        result->columns
                    ) {

                        column.name =
                            alias +
                            "." +
                            column.name;
                    }

                    return result;
                }

                // ------------------------------------------------------------
                // Filter
                // ------------------------------------------------------------

                else if constexpr (
                    std::is_same_v<
                        T,
                        FilterOp
                    >
                ) {

                    return schemaForNode(
                        *op.child
                    );
                }

                // ------------------------------------------------------------
                // Sort
                // ------------------------------------------------------------

                else if constexpr (
                    std::is_same_v<
                        T,
                        SortOp
                    >
                ) {

                    return schemaForNode(
                        *op.child
                    );
                }

                // ------------------------------------------------------------
                // Project
                // ------------------------------------------------------------

                else if constexpr (
                    std::is_same_v<
                        T,
                        ProjectOp
                    >
                ) {

                    auto child =
                        schemaForNode(
                            *op.child
                        );

                    if (!child) {
                        return nullptr;
                    }

                    auto result =
                        std::make_shared<TableSchema>();

                    result->name =
                        child->name +
                        "_PROJECT";

                    result->primary_key.clear();
                    result->root_page_id = -1;

                    if (op.columns.size() == 1 &&
                        op.columns[0] == "*") {

                        for (
                            size_t i = 0;
                            i < child->columns.size();
                            ++i
                        ) {

                            ColumnInfo column =
                                child->columns[i];

                            column.ordinal =
                                static_cast<int>(i);

                            result->columns.push_back(
                                std::move(column)
                            );
                        }

                        return result;
                    }

                    for (
                        const auto& requested :
                        op.columns
                    ) {

                        int ordinal =
                            findColumnOrdinal(
                                *child,
                                requested
                            );

                        if (ordinal < 0) {
                            return nullptr;
                        }

                        ColumnInfo column =
                            child->columns[
                                static_cast<size_t>(
                                    ordinal
                                )
                            ];

                        column.ordinal =
                            static_cast<int>(
                                result->columns.size()
                            );

                        result->columns.push_back(
                            std::move(column)
                        );
                    }

                    return result;
                }

                // ------------------------------------------------------------
                // NestedLoopJoin
                // ------------------------------------------------------------

                else if constexpr (
                    std::is_same_v<
                        T,
                        NestedLoopJoinOp
                    >
                ) {

                    auto left =
                        schemaForNode(
                            *op.left
                        );

                    auto right =
                        schemaForNode(
                            *op.right
                        );

                    if (!left ||
                        !right) {

                        return nullptr;
                    }

                    return std::make_shared<TableSchema>(
                        combineSchemas(
                            *left,
                            *right
                        )
                    );
                }

                // ------------------------------------------------------------
                // Aggregate
                // ------------------------------------------------------------

                else if constexpr (
                    std::is_same_v<
                        T,
                        AggregateOp
                    >
                ) {

                    auto child =
                        schemaForNode(
                            *op.child
                        );

                    if (!child) {
                        return nullptr;
                    }

                    auto result =
                        std::make_shared<TableSchema>();

                    result->name =
                        child->name +
                        "_AGGREGATE";

                    result->primary_key.clear();
                    result->root_page_id = -1;

                    int ordinal = 0;

                    // GROUP BY output columns
                    for (
                        const auto& group_name :
                        op.group_by
                    ) {

                        int source_ordinal =
                            findColumnOrdinal(
                                *child,
                                group_name
                            );

                        if (source_ordinal < 0) {
                            return nullptr;
                        }

                        ColumnInfo column =
                            child->columns[
                                static_cast<size_t>(
                                    source_ordinal
                                )
                            ];

                        column.ordinal =
                            ordinal++;

                        result->columns.push_back(
                            std::move(column)
                        );
                    }

                    // Aggregate output columns
                    for (
                        const auto& aggregate :
                        op.aggregates
                    ) {

                        ColumnInfo column;

                        column.ordinal =
                            ordinal++;

                        column.is_not_null = false;

                        if (aggregate.is_star) {

                            if (aggregate.func !=
                                AggFunc::COUNT) {
                                return nullptr;
                            }

                            column.name =
                                "COUNT(*)";

                            column.type =
                                DataType::INTEGER;

                        } else {

                            if (!aggregate.arg) {
                                return nullptr;
                            }

                            const auto* arg =
                                std::get_if<ColumnRef>(
                                    &aggregate.arg->node
                                );

                            if (!arg) {
                                return nullptr;
                            }

                            std::string arg_name =
                                arg->table.has_value()
                                    ? *arg->table +
                                      "." +
                                      arg->column
                                    : arg->column;

                            switch (aggregate.func) {

                                case AggFunc::COUNT:
                                    column.name =
                                        "COUNT(" +
                                        arg_name +
                                        ")";

                                    column.type =
                                        DataType::INTEGER;
                                    break;

                                case AggFunc::SUM:
                                    column.name =
                                        "SUM(" +
                                        arg_name +
                                        ")";

                                    column.type =
                                        DataType::REAL;
                                    break;

                                case AggFunc::AVG:
                                    column.name =
                                        "AVG(" +
                                        arg_name +
                                        ")";

                                    column.type =
                                        DataType::REAL;
                                    break;

                                case AggFunc::MIN:
                                    column.name =
                                        "MIN(" +
                                        arg_name +
                                        ")";

                                    column.type =
                                        DataType::REAL;
                                    break;

                                case AggFunc::MAX:
                                    column.name =
                                        "MAX(" +
                                        arg_name +
                                        ")";

                                    column.type =
                                        DataType::REAL;
                                    break;
                            }
                        }

                        result->columns.push_back(
                            std::move(column)
                        );
                    }

                    return result;
                }

                else {
                    return nullptr;
                }
            },
            node.v
        );
    }


    // ========================================================================
    // Find a column ordinal.
    //
    // Supports:
    //   id
    //   users.id
    //   u.id
    //
    // Qualified names are preferred when available.
    // ========================================================================

    static int findColumnOrdinal(
        const TableSchema& schema,
        const std::string& requested
    ) {

        // --------------------------------------------------------------------
        // Exact match first
        // --------------------------------------------------------------------

        for (
            const auto& column :
            schema.columns
        ) {

            if (column.name ==
                requested) {

                return column.ordinal;
            }
        }

        // --------------------------------------------------------------------
        // Bare-name lookup
        // --------------------------------------------------------------------

        std::string requested_bare =
            requested;

        const size_t requested_dot =
            requested_bare.find('.');

        if (requested_dot !=
            std::string::npos) {

            requested_bare =
                requested_bare.substr(
                    requested_dot + 1
                );
        }

        int found =
            -1;

        for (
            const auto& column :
            schema.columns
        ) {

            std::string bare =
                column.name;

            const size_t dot =
                bare.find('.');

            if (dot !=
                std::string::npos) {

                bare =
                    bare.substr(
                        dot + 1
                    );
            }

            if (bare !=
                requested_bare) {

                continue;
            }

            // Ambiguous bare column.
            if (found >= 0) {
                return -1;
            }

            found =
                column.ordinal;
        }

        return found;
    }


    // ========================================================================
    // Rewrite aggregate calls in HAVING into references to the
    // materialized aggregate output columns.
    // ========================================================================

    static std::string aggregateOutputName(
        const AggregateCall& aggregate
    ) {
        std::string name;

        switch (aggregate.func) {
            case AggFunc::COUNT:
                name = "COUNT(";
                break;

            case AggFunc::SUM:
                name = "SUM(";
                break;

            case AggFunc::AVG:
                name = "AVG(";
                break;

            case AggFunc::MIN:
                name = "MIN(";
                break;

            case AggFunc::MAX:
                name = "MAX(";
                break;
        }

        if (aggregate.is_star) {
            name += "*";
        } else {
            const auto* ref =
                std::get_if<ColumnRef>(
                    &aggregate.arg->node
                );

            if (!ref) {
                throw std::runtime_error(
                    "ExecutorAdapter: aggregate "
                    "argument in HAVING must be "
                    "a column reference"
                );
            }

            if (ref->table.has_value()) {
                name +=
                    *ref->table +
                    ".";
            }

            name +=
                ref->column;
        }

        name += ")";

        return name;
    }


    static Expr rewriteHavingForAggregate(
        const Expr& expr,
        const TableSchema& aggregate_schema
    ) {
        return std::visit(
            [&](const auto& node) -> Expr {
                using T =
                    std::decay_t<decltype(node)>;

                if constexpr (
                    std::is_same_v<T, ColumnRef>
                ) {
                    return Expr{
                        ColumnRef{
                            node.table,
                            node.column
                        }
                    };
                }

                else if constexpr (
                    std::is_same_v<T, Literal>
                ) {
                    return Expr{
                        Literal{
                            node.value
                        }
                    };
                }

                else if constexpr (
                    std::is_same_v<T, BinaryOp>
                ) {
                    return Expr{
                        BinaryOp{
                            std::make_unique<Expr>(
                                rewriteHavingForAggregate(
                                    *node.left,
                                    aggregate_schema
                                )
                            ),
                            node.op,
                            std::make_unique<Expr>(
                                rewriteHavingForAggregate(
                                    *node.right,
                                    aggregate_schema
                                )
                            )
                        }
                    };
                }

                else if constexpr (
                    std::is_same_v<T, UnaryOp>
                ) {
                    return Expr{
                        UnaryOp{
                            node.op,
                            std::make_unique<Expr>(
                                rewriteHavingForAggregate(
                                    *node.operand,
                                    aggregate_schema
                                )
                            )
                        }
                    };
                }

                else if constexpr (
                    std::is_same_v<T, AggregateCall>
                ) {
                    const std::string name =
                        aggregateOutputName(node);

                    if (
                        findColumnOrdinal(
                            aggregate_schema,
                            name
                        ) < 0
                    ) {
                        throw std::runtime_error(
                            "ExecutorAdapter: aggregate "
                            "output column '" +
                            name +
                            "' not found in HAVING schema"
                        );
                    }

                    return Expr{
                        ColumnRef{
                            std::nullopt,
                            name
                        }
                    };
                }

                else {
                    throw std::runtime_error(
                        "ExecutorAdapter: unsupported "
                        "expression in HAVING"
                    );
                }
            },
            expr.node
        );
    }



    // ========================================================================
    // Combine two already-resolved schemas.
    //
    // Child schemas already contain query-visible names such as:
    //
    //   users.id
    //   users.name
    //   orders.id
    //
    // Therefore we simply concatenate them.
    // ========================================================================

    static TableSchema combineSchemas(
        const TableSchema& left,
        const TableSchema& right
    ) {

        TableSchema result;

        result.name =
            left.name +
            "_JOIN_" +
            right.name;

        result.primary_key.clear();
        result.root_page_id = -1;

        int ordinal = 0;

        for (
            const auto& column :
            left.columns
        ) {

            ColumnInfo copy =
                column;

            copy.ordinal =
                ordinal++;

            result.columns.push_back(
                std::move(copy)
            );
        }

        for (
            const auto& column :
            right.columns
        ) {

            ColumnInfo copy =
                column;

            copy.ordinal =
                ordinal++;

            result.columns.push_back(
                std::move(copy)
            );
        }

        return result;
    }
};