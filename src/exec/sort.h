#pragma once
#include "../common/cursor_interface.h"
#include <vector>
#include <algorithm>

struct SortKey {
    size_t col;
    bool   descending;
};

class Sort : public Operator {
public:
    Sort(OperatorPtr child, std::vector<SortKey> sort_keys)
        : child_(std::move(child))
        , sort_keys_(std::move(sort_keys))
        , result_idx_(0) {}

    void open() override {
        child_->open();
        results_.clear();
        result_idx_ = 0;
        while (auto row = child_->next()) results_.push_back(*row);
        child_->close();
        std::stable_sort(results_.begin(), results_.end(),
            [this](const Row& a, const Row& b) {
                for (const auto& key : sort_keys_) {
                    int cmp = compare(a[key.col], b[key.col]);
                    if (cmp != 0) return key.descending ? cmp > 0 : cmp < 0;
                }
                return false;
            });
    }

    std::optional<Row> next() override {
        if (result_idx_ >= results_.size()) return std::nullopt;
        return results_[result_idx_++];
    }

    void close() override { results_.clear(); result_idx_ = 0; }

private:
    OperatorPtr          child_;
    std::vector<SortKey> sort_keys_;
    std::vector<Row>     results_;
    size_t               result_idx_;

    int compare(const Value& a, const Value& b) {
        return std::visit([](const auto& x, const auto& y) -> int {
            using TX = std::decay_t<decltype(x)>;
            using TY = std::decay_t<decltype(y)>;
            if constexpr (std::is_same_v<TX, TY>) {
                if (x < y) return -1;
                if (x > y) return  1;
                return 0;
            }
            return 0;
        }, a, b);
    }
};
