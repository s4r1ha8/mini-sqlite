#pragma once

#include "../common/cursor_interface.h"
#include "../common/eval.h"

#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

enum class RuntimeAggFunc {
    Count,
    Sum,
    Avg,
    Min,
    Max
};

struct AggCall {
    RuntimeAggFunc func;
    size_t col;
};

class Aggregate : public Operator {
public:
    Aggregate(
        OperatorPtr child,
        std::vector<size_t> group_by_cols,
        std::vector<AggCall> agg_calls,
        std::optional<Expr> having = std::nullopt,
        std::shared_ptr<TableSchema> output_schema = nullptr
    )
        : child_(std::move(child))
        , group_by_cols_(std::move(group_by_cols))
        , agg_calls_(std::move(agg_calls))
        , having_(std::move(having))
        , output_schema_(std::move(output_schema))
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
            std::string key =
                make_group_key(*row);

            auto& g =
                groups[key];

            if (!g.initialized) {
                g.counts.assign(
                    agg_calls_.size(),
                    0
                );

                g.sums.assign(
                    agg_calls_.size(),
                    0.0
                );

                g.mins.resize(
                    agg_calls_.size()
                );

                g.maxs.resize(
                    agg_calls_.size()
                );

                g.initialized = true;

                group_rows_[key] =
                    *row;
            }

            for (size_t i = 0;
                 i < agg_calls_.size();
                 ++i) {

                const auto& call =
                    agg_calls_[i];

                g.counts[i]++;

                if (call.func ==
                    RuntimeAggFunc::Count) {
                    continue;
                }

                if (call.col >= row->size()) {
                    throw std::runtime_error(
                        "Aggregate: column ordinal " +
                        std::to_string(call.col) +
                        " is outside the input row."
                    );
                }

                const Value& val =
                    (*row)[call.col];

                double num =
                    to_double(val);

                g.sums[i] += num;

                if (g.counts[i] == 1 ||
                    compare(
                        val,
                        g.mins[i]
                    ) < 0) {

                    g.mins[i] = val;
                }

                if (g.counts[i] == 1 ||
                    compare(
                        val,
                        g.maxs[i]
                    ) > 0) {

                    g.maxs[i] = val;
                }
            }
        }

        child_->close();

        for (auto& [key, g] : groups) {
            Row result;

            const Row& sample =
                group_rows_[key];

            // ------------------------------------------------------------
            // GROUP BY output
            // ------------------------------------------------------------

            for (size_t col :
                 group_by_cols_) {

                if (col >= sample.size()) {
                    throw std::runtime_error(
                        "Aggregate: GROUP BY column ordinal " +
                        std::to_string(col) +
                        " is outside the input row."
                    );
                }

                result.push_back(
                    sample[col]
                );
            }

            // ------------------------------------------------------------
            // Aggregate output
            // ------------------------------------------------------------

            for (size_t i = 0;
                 i < agg_calls_.size();
                 ++i) {

                switch (agg_calls_[i].func) {

                    case RuntimeAggFunc::Count:
                        result.push_back(
                            Value(g.counts[i])
                        );
                        break;

                    case RuntimeAggFunc::Sum:
                        result.push_back(
                            Value(g.sums[i])
                        );
                        break;

                    case RuntimeAggFunc::Avg:
                        result.push_back(
                            Value(
                                g.counts[i] > 0
                                    ? g.sums[i] /
                                      static_cast<double>(
                                          g.counts[i]
                                      )
                                    : 0.0
                            )
                        );
                        break;

                    case RuntimeAggFunc::Min:
                        result.push_back(
                            g.mins[i]
                        );
                        break;

                    case RuntimeAggFunc::Max:
                        result.push_back(
                            g.maxs[i]
                        );
                        break;
                }
            }

            // ------------------------------------------------------------
            // HAVING
            //
            // At this point `result` is a fully materialized aggregate
            // row. The evaluator uses output_schema_ so expressions such
            // as COUNT(*) > 1 can be evaluated against it.
            // ------------------------------------------------------------

            if (having_.has_value()) {
                if (!output_schema_) {
                    throw std::runtime_error(
                        "Aggregate: HAVING requires an output schema."
                    );
                }

                if (!evalBool(
                        *having_,
                        result,
                        *output_schema_
                    )) {
                    continue;
                }
            }

            results_.push_back(
                std::move(result)
            );
        }

        // SQL aggregate queries without GROUP BY still return one row on
        // empty input. COUNT(*) becomes 0. Other aggregates use 0.0 in
        // this v1 implementation.
        if (groups.empty() &&
            group_by_cols_.empty() &&
            !agg_calls_.empty()) {

            Row result;

            for (const auto& call :
                 agg_calls_) {

                switch (call.func) {

                    case RuntimeAggFunc::Count:
                        result.push_back(
                            Value(int64_t(0))
                        );
                        break;

                    case RuntimeAggFunc::Sum:
                    case RuntimeAggFunc::Avg:
                        result.push_back(
                            Value(0.0)
                        );
                        break;

                    case RuntimeAggFunc::Min:
                    case RuntimeAggFunc::Max:
                        result.push_back(
                            Value(std::monostate{})
                        );
                        break;
                }
            }

            if (having_.has_value()) {
                if (!output_schema_) {
                    throw std::runtime_error(
                        "Aggregate: HAVING requires an output schema."
                    );
                }

                if (evalBool(
                        *having_,
                        result,
                        *output_schema_
                    )) {
                    results_.push_back(
                        std::move(result)
                    );
                }
            } else {
                results_.push_back(
                    std::move(result)
                );
            }
        }
    }

    std::optional<Row> next() override {
        if (result_idx_ >= results_.size()) {
            return std::nullopt;
        }

        return results_[result_idx_++];
    }

    void close() override {
        results_.clear();
        group_rows_.clear();
        result_idx_ = 0;
    }

private:
    OperatorPtr child_;

    std::vector<size_t>
        group_by_cols_;

    std::vector<AggCall>
        agg_calls_;

    std::optional<Expr>
        having_;

    std::shared_ptr<TableSchema>
        output_schema_;

    std::vector<Row>
        results_;

    std::unordered_map<
        std::string,
        Row
    > group_rows_;

    size_t result_idx_;

    // ------------------------------------------------------------------------
    // GROUP BY key generation
    // ------------------------------------------------------------------------

    std::string make_group_key(
        const Row& row
    ) {
        std::string key;

        for (size_t col :
             group_by_cols_) {

            if (col >= row.size()) {
                throw std::runtime_error(
                    "Aggregate: GROUP BY column ordinal " +
                    std::to_string(col) +
                    " is outside the input row."
                );
            }

            std::visit(
                [&key](const auto& v) {
                    using T =
                        std::decay_t<
                            decltype(v)
                        >;

                    if constexpr (
                        std::is_same_v<
                            T,
                            int64_t
                        >
                    ) {
                        key +=
                            "I:" +
                            std::to_string(v) +
                            "|";
                    }

                    else if constexpr (
                        std::is_same_v<
                            T,
                            std::string
                        >
                    ) {
                        key +=
                            "S:" +
                            std::to_string(v.size()) +
                            ":" +
                            v +
                            "|";
                    }

                    else if constexpr (
                        std::is_same_v<
                            T,
                            double
                        >
                    ) {
                        key +=
                            "D:" +
                            std::to_string(v) +
                            "|";
                    }

                    else {
                        key += "N|";
                    }
                },
                row[col]
            );
        }

        return key;
    }

    // ------------------------------------------------------------------------
    // Numeric conversion
    // ------------------------------------------------------------------------

    double to_double(
        const Value& v
    ) {
        return std::visit(
            [](const auto& val) -> double {
                using T =
                    std::decay_t<
                        decltype(val)
                    >;

                if constexpr (
                    std::is_same_v<
                        T,
                        int64_t
                    >
                ) {
                    return static_cast<double>(
                        val
                    );
                }

                if constexpr (
                    std::is_same_v<
                        T,
                        double
                    >
                ) {
                    return val;
                }

                return 0.0;
            },
            v
        );
    }

    // ------------------------------------------------------------------------
    // Value comparison
    // ------------------------------------------------------------------------

    int compare(
        const Value& a,
        const Value& b
    ) {
        return std::visit(
            [](const auto& x,
               const auto& y) -> int {

                using TX =
                    std::decay_t<
                        decltype(x)
                    >;

                using TY =
                    std::decay_t<
                        decltype(y)
                    >;

                if constexpr (
                    std::is_same_v<
                        TX,
                        TY
                    >
                ) {
                    if (x < y) return -1;
                    if (x > y) return 1;
                    return 0;
                }

                // Numeric promotion for INTEGER / REAL.
                if constexpr (
                    (std::is_same_v<TX, int64_t> &&
                     std::is_same_v<TY, double>) ||
                    (std::is_same_v<TX, double> &&
                     std::is_same_v<TY, int64_t>)
                ) {
                    const double dx =
                        static_cast<double>(x);

                    const double dy =
                        static_cast<double>(y);

                    if (dx < dy) return -1;
                    if (dx > dy) return 1;
                    return 0;
                }

                return 0;
            },
            a,
            b
        );
    }
};