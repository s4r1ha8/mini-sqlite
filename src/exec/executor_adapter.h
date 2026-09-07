#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <utility>
#include "common/cursor_interface.h"
#include "common/eval.h"
#include "frontend/planner.h"
#include "frontend/catalog.h"
#include "storage/pager.h"
#include "exec/seq_scan.h"
#include "exec/filter.h"

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