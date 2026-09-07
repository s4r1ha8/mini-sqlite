#pragma once

#include <memory>
#include <stdexcept>
#include <string>

#include "common/cursor_interface.h"
#include "frontend/planner.h"
#include "storage/pager.h"
#include "frontend/catalog.h"
#include "exec/seq_scan.h"

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

    template <typename T>
    OperatorPtr buildNode(const T&) {
        throw std::runtime_error(
            "ExecutorAdapter: operator not implemented yet");
    }
};