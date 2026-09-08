#pragma once
#include "../common/cursor_interface.h"
#include "../common/ast.h"
#include "../common/eval.h"
#include "../frontend/catalog.h"
#include <vector>
#include <algorithm>
#include <memory>

class Sort : public Operator {
public:
    Sort(OperatorPtr child,
         std::vector<OrderByItem> order_by_items,
         std::shared_ptr<TableSchema> schema)
        : child_(std::move(child))
        , order_by_items_(std::move(order_by_items))
        , schema_(std::move(schema))
        , result_idx_(0) {}

    void open() override {
        child_->open();
        results_.clear();
        result_idx_ = 0;
        
        // Evaluate sort keys for each row exactly once
        while (auto row = child_->next()) {
            std::vector<Value> keys;
            keys.reserve(order_by_items_.size());
            for (const auto& item : order_by_items_) {
                keys.push_back(eval(item.expr, *row, *schema_));
            }
            results_.push_back({*row, std::move(keys)});
        }
        child_->close();

        std::stable_sort(results_.begin(), results_.end(),
            [this](const RowWithKeys& a, const RowWithKeys& b) {
                for (size_t i = 0; i < order_by_items_.size(); ++i) {
                    int cmp = compare(a.keys[i], b.keys[i]);
                    if (cmp != 0) {
                        return order_by_items_[i].ascending ? cmp < 0 : cmp > 0;
                    }
                }
                return false;
            });
    }

    std::optional<Row> next() override {
        if (result_idx_ >= results_.size()) return std::nullopt;
        return results_[result_idx_++].row;
    }

    void close() override { results_.clear(); result_idx_ = 0; }

private:
    struct RowWithKeys {
        Row row;
        std::vector<Value> keys;
    };

    OperatorPtr                  child_;
    std::vector<OrderByItem>     order_by_items_;
    std::shared_ptr<TableSchema> schema_;
    std::vector<RowWithKeys>     results_;
    size_t                       result_idx_;

    int compare(const Value& a, const Value& b) {
        return std::visit([](const auto& x, const auto& y) -> int {
            using TX = std::decay_t<decltype(x)>;
            using TY = std::decay_t<decltype(y)>;
            if constexpr (std::is_same_v<TX, TY>) {
                if (x < y) return -1;
                if (x > y) return  1;
                return 0;
            }
            return 0; // mismatched types fallback
        }, a, b);
    }
};
