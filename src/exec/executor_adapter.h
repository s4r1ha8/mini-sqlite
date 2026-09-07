#pragma once

#include "exec/index_scan.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <variant>
#include <utility>
#include "common/cursor_interface.h"
#include "common/eval.h"
#include "frontend/planner.h"
#include "frontend/catalog.h"
#include "storage/pager.h"
#include "exec/seq_scan.h"
#include "exec/filter.h"
#include "exec/sort.h"

class Project : public Operator {
public:
    Project(OperatorPtr child, std::vector<size_t> columns)
        : child_(std::move(child)),
          columns_(std::move(columns)) {}

    void open() override {
        child_->open();
    }

    std::optional<Row> next() override {
        auto row = child_->next();

        if (!row.has_value())
            return std::nullopt;

        Row result;
        result.reserve(columns_.size());

        for (size_t index : columns_) {
            if (index >= row->size()) {
                throw std::runtime_error(
                    "Project: column index out of range");
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

class ExecutorAdapter {
public:
    ExecutorAdapter(Pager& pager, Catalog& catalog)
        : pager_(pager), catalog_(catalog) {}

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

OperatorPtr buildNode(const ProjectOp& op) {
    OperatorPtr child = build(*op.child);

    const TableSchema* schema = schemaForNode(*op.child);

    if (!schema) {
        throw std::runtime_error(
            "ExecutorAdapter: cannot determine schema for Project");
    }

    std::vector<size_t> columns;

    if (op.columns.size() == 1 && op.columns[0] == "*") {
        columns.reserve(schema->columns.size());

        for (const auto& column : schema->columns) {
            columns.push_back(
                static_cast<size_t>(column.ordinal));
        }
    } else {
        columns.reserve(op.columns.size());

        for (const auto& requested : op.columns) {
            std::string column_name = requested;

            const size_t dot = column_name.find('.');
            if (dot != std::string::npos) {
                column_name = column_name.substr(dot + 1);
            }

            bool found = false;

            for (const auto& column : schema->columns) {
                if (column.name == column_name) {
                    columns.push_back(
                        static_cast<size_t>(column.ordinal));
                    found = true;
                    break;
                }
            }

            if (!found) {
                throw std::runtime_error(
                    "ExecutorAdapter: SELECT column '" +
                    requested + "' not found");
            }
        }
    }

    return std::make_unique<Project>(
        std::move(child),
        std::move(columns)
    );
}

OperatorPtr buildNode(const IndexScanOp& op) {
    const TableSchema* schema = catalog_.get_table(op.table_name);

    if (!schema) {
        throw std::runtime_error(
            "ExecutorAdapter: table '" + op.table_name +
            "' does not exist");
    }

    if (schema->root_page_id < 0) {
        throw std::runtime_error(
            "ExecutorAdapter: table '" + op.table_name +
            "' has no root page");
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
            "ExecutorAdapter: index '" + op.index_name +
            "' not found on table '" + op.table_name + "'");
    }

    if (index->root_page_id < 0) {
        throw std::runtime_error(
            "ExecutorAdapter: index '" + op.index_name +
            "' has no root page");
    }

    const auto* binary = std::get_if<BinaryOp>(&op.key_condition.node);

    if (!binary || binary->op != "=") {
        throw std::runtime_error(
            "ExecutorAdapter: IndexScan requires equality predicate");
    }

    const Literal* literal = std::get_if<Literal>(&binary->left->node);
    if (!literal) {
        literal = std::get_if<Literal>(&binary->right->node);
    }

    if (!literal) {
        throw std::runtime_error(
            "ExecutorAdapter: IndexScan key condition must contain a literal");
    }

    Value lookup_value = std::visit(
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

    OperatorPtr buildNode(const SeqScanOp& op) {
        const TableSchema* schema = catalog_.get_table(op.table_name);

        if (!schema) {
            throw std::runtime_error(
                "ExecutorAdapter: table '" + op.table_name +
                "' does not exist");
        }

        if (schema->root_page_id < 0) {
            throw std::runtime_error(
                "ExecutorAdapter: table '" + op.table_name +
                "' has no root page");
        }

        return std::make_unique<SeqScan>(
            pager_,
            static_cast<PageId>(schema->root_page_id),
            schema->columns.size()
        );
    }

   OperatorPtr buildNode(const FilterOp& op) {
    OperatorPtr child = build(*op.child);

    const TableSchema* schema = schemaForNode(*op.child);

    if (!schema) {
        throw std::runtime_error(
            "ExecutorAdapter: cannot determine schema for Filter");
    }

    auto predicate =
        std::make_shared<Expr>(cloneExpr(op.predicate));

    auto schema_copy =
        std::make_shared<TableSchema>(*schema);

    return std::make_unique<Filter>(
        std::move(child),
        [predicate, schema_copy](const Row& row) {
            return evalBool(*predicate, row, *schema_copy);
        }
    );
}

OperatorPtr buildNode(const SortOp& op) {
    OperatorPtr child = build(*op.child);

    const TableSchema* schema = schemaForNode(*op.child);

    if (!schema) {
        throw std::runtime_error(
            "ExecutorAdapter: cannot determine schema for Sort");
    }

    std::vector<SortKey> keys;
    keys.reserve(op.order_by.size());

    for (const auto& item : op.order_by) {
        std::string column_name = item.column;

        // ORDER BY currently stores a name or qualified name.
        const size_t dot = column_name.find('.');
        if (dot != std::string::npos) {
            column_name = column_name.substr(dot + 1);
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
                item.column + "' not found");
        }
    }

    return std::make_unique<Sort>(
        std::move(child),
        std::move(keys)
    );
}

    template <typename T>
    OperatorPtr buildNode(const T&) {
        throw std::runtime_error(
            "ExecutorAdapter: operator not implemented yet");
    }

    const TableSchema* schemaForNode(const OperatorNode& node) const {
        return std::visit(
            [this](const auto& op) -> const TableSchema* {
                using T = std::decay_t<decltype(op)>;

                if constexpr (std::is_same_v<T, SeqScanOp>) {
                    return catalog_.get_table(op.table_name);
                } else if constexpr (std::is_same_v<T, FilterOp>) {
                    return schemaForNode(*op.child);
                } else {
                    return nullptr;
                }
            },
            node.v
        );
    }
};
