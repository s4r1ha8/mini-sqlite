#pragma once
#include "../common/cursor_interface.h"
#include <unordered_map>
#include <string>
#include <vector>
#include <cmath>

enum class AggFunc { Count, Sum, Avg, Min, Max };

struct AggCall {
    AggFunc func;
    size_t  col;
};

class Aggregate : public Operator {
public:
    Aggregate(OperatorPtr child,
              std::vector<size_t> group_by_cols,
              std::vector<AggCall> agg_calls)
        : child_(std::move(child))
        , group_by_cols_(std::move(group_by_cols))
        , agg_calls_(std::move(agg_calls))
        , result_idx_(0) {}

    void open() override {
        child_->open();
        results_.clear();
        group_rows_.clear();
        result_idx_ = 0;

        struct GroupState {
            std::vector<int64_t> counts;
            std::vector<double>  sums;
            std::vector<Value>   mins;
            std::vector<Value>   maxs;
            bool initialized = false;
        };

        std::unordered_map<std::string, GroupState> groups;

        while (auto row = child_->next()) {
            std::string key = make_group_key(*row);
            auto& g = groups[key];

            if (!g.initialized) {
                g.counts.assign(agg_calls_.size(), 0);
                g.sums.assign(agg_calls_.size(), 0.0);
                g.mins.resize(agg_calls_.size());
                g.maxs.resize(agg_calls_.size());
                g.initialized = true;
                group_rows_[key] = *row;
            }

            for (size_t i = 0; i < agg_calls_.size(); i++) {
                const auto& call = agg_calls_[i];
                g.counts[i]++;
                if (call.func == AggFunc::Count) continue;
                Value val = (*row)[call.col];
                double num = to_double(val);
                g.sums[i] += num;
                if (g.counts[i] == 1 || compare(val, g.mins[i]) < 0) g.mins[i] = val;
                if (g.counts[i] == 1 || compare(val, g.maxs[i]) > 0) g.maxs[i] = val;
            }
        }
        child_->close();

        for (auto& [key, g] : groups) {
            Row result;
            const Row& sample = group_rows_[key];
            for (size_t col : group_by_cols_) result.push_back(sample[col]);
            for (size_t i = 0; i < agg_calls_.size(); i++) {
                switch (agg_calls_[i].func) {
                    case AggFunc::Count: result.push_back(Value(g.counts[i])); break;
                    case AggFunc::Sum:   result.push_back(Value(g.sums[i]));   break;
                    case AggFunc::Avg:   result.push_back(Value(g.counts[i] > 0 ? g.sums[i]/g.counts[i] : 0.0)); break;
                    case AggFunc::Min:   result.push_back(g.mins[i]); break;
                    case AggFunc::Max:   result.push_back(g.maxs[i]); break;
                }
            }
            results_.push_back(std::move(result));
        }
    }

    std::optional<Row> next() override {
        if (result_idx_ >= results_.size()) return std::nullopt;
        return results_[result_idx_++];
    }

    void close() override { results_.clear(); group_rows_.clear(); result_idx_ = 0; }

private:
    OperatorPtr          child_;
    std::vector<size_t>  group_by_cols_;
    std::vector<AggCall> agg_calls_;
    std::vector<Row>     results_;
    std::unordered_map<std::string, Row> group_rows_;
    size_t               result_idx_;

    std::string make_group_key(const Row& row) {
        std::string key;
        for (size_t col : group_by_cols_) {
            std::visit([&key](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, int64_t>)    key += std::to_string(v) + "|";
                else if constexpr (std::is_same_v<T, std::string>) key += v + "|";
                else if constexpr (std::is_same_v<T, double>) key += std::to_string(v) + "|";
                else key += "NULL|";
            }, row[col]);
        }
        return key;
    }

    double to_double(const Value& v) {
        return std::visit([](const auto& val) -> double {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, int64_t>) return static_cast<double>(val);
            if constexpr (std::is_same_v<T, double>)  return val;
            return 0.0;
        }, v);
    }

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
