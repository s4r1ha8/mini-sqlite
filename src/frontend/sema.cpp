// ============================================================================
// sema.cpp — Semantic analysis implementation.
// ============================================================================

#include "frontend/sema.h"
#include <stdexcept>
#include <unordered_map>

SemanticAnalyzer::SemanticAnalyzer(Catalog& catalog) : catalog_(catalog) {}

// ── Public entry point ────────────────────────────────────────────────────────

void SemanticAnalyzer::analyze(const Statement& stmt) {
    std::visit([this](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if      constexpr (std::is_same_v<T, CreateTableStmt>) analyzeCreateTable(s);
        else if constexpr (std::is_same_v<T, CreateIndexStmt>) analyzeCreateIndex(s);
        else if constexpr (std::is_same_v<T, InsertStmt>)      analyzeInsert(s);
        else if constexpr (std::is_same_v<T, SelectStmt>)      analyzeSelect(s);
        else if constexpr (std::is_same_v<T, TxnStmt>)         { /* nothing to validate */ }
    }, stmt);
}

// ── CREATE TABLE ─────────────────────────────────────────────────────────────

void SemanticAnalyzer::analyzeCreateTable(const CreateTableStmt& stmt) {
    if (catalog_.table_exists(stmt.table_name)) {
        throw SemanticError(
            "Table '" + stmt.table_name + "' already exists.");
    }

    if (stmt.columns.empty()) {
        throw SemanticError(
            "Table '" + stmt.table_name + "' must have at least one column.");
    }

    // Check for duplicate column names within the definition
    std::unordered_map<std::string, int> seen;
    int pkCount = 0;

    for (const auto& col : stmt.columns) {
        if (seen.count(col.name)) {
            throw SemanticError(
                "Duplicate column name '" + col.name +
                "' in CREATE TABLE " + stmt.table_name + ".");
        }

        seen[col.name]++;

        if (col.is_primary_key)
            pkCount++;
    }

    if (pkCount > 1) {
        throw SemanticError(
            "Table '" + stmt.table_name + "' has more than one PRIMARY KEY.");
    }

    // Build and register the TableSchema
    TableSchema schema;
    schema.name = stmt.table_name;

    for (const auto& col : stmt.columns) {
        ColumnInfo info;
        info.name        = col.name;
        info.type        = col.type;
        info.is_not_null = col.is_not_null;

        if (col.is_primary_key)
            schema.primary_key = col.name;

        schema.columns.push_back(info);
    }

    catalog_.add_table(std::move(schema)); // also assigns ordinals
}

// ── CREATE INDEX ──────────────────────────────────────────────────────────────

void SemanticAnalyzer::analyzeCreateIndex(const CreateIndexStmt& stmt) {
    if (!catalog_.table_exists(stmt.table_name)) {
        throw SemanticError(
            "Cannot create index: table '" + stmt.table_name + "' does not exist.");
    }

    if (!catalog_.column_exists(stmt.table_name, stmt.column_name)) {
        throw SemanticError(
            "Cannot create index on '" + stmt.column_name +
            "': column does not exist in table '" + stmt.table_name + "'.");
    }

    // add_index itself checks for duplicate index names
    catalog_.add_index(
        stmt.table_name,
        IndexInfo{stmt.index_name, stmt.column_name, -1}
    );
}

// ── INSERT ────────────────────────────────────────────────────────────────────

void SemanticAnalyzer::analyzeInsert(const InsertStmt& stmt) {
    const TableSchema* schema = catalog_.get_table(stmt.table_name);

    if (!schema) {
        throw SemanticError(
            "INSERT: table '" + stmt.table_name + "' does not exist.");
    }

    // Positional insert: value count must match column count
    if (stmt.values.size() != schema->columns.size()) {
        throw SemanticError(
            "INSERT into '" + stmt.table_name + "' provides " +
            std::to_string(stmt.values.size()) + " value(s) but the table has " +
            std::to_string(schema->columns.size()) + " column(s).");
    }

    // Type-check each value against its corresponding column
    for (size_t i = 0; i < stmt.values.size(); ++i) {
        const ColumnInfo& col = schema->columns[i];
        const LiteralValue& val = stmt.values[i].value;

        // NULL is always allowed unless NOT NULL is set
        if (std::holds_alternative<std::monostate>(val)) {
            if (col.is_not_null) {
                throw SemanticError(
                    "INSERT into '" + stmt.table_name + "': column '" +
                    col.name + "' is NOT NULL but a NULL value was supplied.");
            }

            continue;
        }

        bool typeOk = false;

        switch (col.type) {
            case DataType::INTEGER:
                typeOk = std::holds_alternative<int64_t>(val);
                break;

            case DataType::REAL:
                typeOk =
                    std::holds_alternative<double>(val) ||
                    std::holds_alternative<int64_t>(val);
                break;

            case DataType::TEXT:
                typeOk = std::holds_alternative<std::string>(val);
                break;
        }

        if (!typeOk) {
            throw SemanticError(
                "INSERT into '" + stmt.table_name + "': value at position " +
                std::to_string(i + 1) + " has wrong type for column '" +
                col.name + "' (expected " +
                dataTypeToString(col.type) + ").");
        }
    }
}

// ── SELECT ────────────────────────────────────────────────────────────────────

void SemanticAnalyzer::analyzeSelect(const SelectStmt& stmt) {
    // Build alias → real_table_name map for all referenced tables
    std::unordered_map<std::string, std::string> tables;

    for (const auto& ref : stmt.from) {
        if (!catalog_.table_exists(ref.table_name)) {
            throw SemanticError(
                "SELECT: table '" + ref.table_name + "' does not exist.");
        }

        std::string key = ref.alias.value_or(ref.table_name);

        if (tables.count(key)) {
            throw SemanticError(
                "SELECT: duplicate table alias or name '" + key + "'.");
        }

        tables[key] = ref.table_name;
    }

    for (const auto& jc : stmt.joins) {
        if (!catalog_.table_exists(jc.table.table_name)) {
            throw SemanticError(
                "JOIN: table '" + jc.table.table_name + "' does not exist.");
        }

        std::string key = jc.table.alias.value_or(jc.table.table_name);

        if (tables.count(key)) {
            throw SemanticError(
                "JOIN: duplicate table alias or name '" + key + "'.");
        }

        tables[key] = jc.table.table_name;
        analyzeExpr(jc.on, tables);
    }

    // Validate SELECT column list
    //
    // Plain column references are validated against the catalog.
    // Aggregate expressions are validated separately using their AST form
    // in stmt.aggregate_columns.
    if (stmt.columns.size() != 1 || stmt.columns[0] != "*") {
        for (const auto& colSpec : stmt.columns) {

            // Aggregate output strings are generated by the parser.
            // They are not actual catalog column names.
            const bool isAggregate =
                colSpec.rfind("COUNT(", 0) == 0 ||
                colSpec.rfind("SUM(",   0) == 0 ||
                colSpec.rfind("AVG(",   0) == 0 ||
                colSpec.rfind("MIN(",   0) == 0 ||
                colSpec.rfind("MAX(",   0) == 0;

            if (isAggregate) {
                continue;
            }

            // colSpec is either "col" or "table.col"
            auto dot = colSpec.find('.');

            if (dot == std::string::npos) {
                // Unqualified — must exist in at least one referenced table
                bool found = false;

                for (const auto& [alias, realName] : tables) {
                    if (catalog_.column_exists(realName, colSpec)) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    throw SemanticError(
                        "SELECT: column '" + colSpec +
                        "' does not exist in any referenced table.");
                }

            } else {
                std::string tbl = colSpec.substr(0, dot);
                std::string col = colSpec.substr(dot + 1);

                if (!tables.count(tbl)) {
                    throw SemanticError(
                        "SELECT: table alias or name '" + tbl +
                        "' is not in the FROM / JOIN clause.");
                }

                if (!catalog_.column_exists(tables.at(tbl), col)) {
                    throw SemanticError(
                        "SELECT: column '" + col +
                        "' does not exist in table '" +
                        tables.at(tbl) + "'.");
                }
            }
        }
    }

    // Validate aggregate expressions using their AST representation.
    //
    // AggregateCall contains a unique_ptr, so it must not be copied into
    // another Expr. Validate its argument directly instead.
    for (const auto& aggregate : stmt.aggregate_columns) {
        if (!aggregate.is_star) {
            if (!aggregate.arg) {
                throw SemanticError(
                    "SELECT: aggregate function is missing its argument.");
            }

            analyzeExpr(*aggregate.arg, tables);
        }
    }

    // Validate WHERE expression
    if (stmt.where.has_value()) {
        analyzeExpr(*stmt.where, tables);
    }

    // Validate GROUP BY columns
    if (stmt.group_by.has_value()) {
        for (const auto& col : *stmt.group_by) {
            bool found = false;

            for (const auto& [alias, realName] : tables) {
                if (catalog_.column_exists(realName, col)) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                throw SemanticError(
                    "GROUP BY: column '" + col +
                    "' does not exist in any referenced table.");
            }
        }
    }

    // Validate HAVING
    if (stmt.having.has_value()) {
        if (!stmt.group_by.has_value()) {
            throw SemanticError(
                "HAVING clause requires a GROUP BY clause.");
        }

        analyzeExpr(*stmt.having, tables);
    }

    // Validate ORDER BY columns
    if (stmt.order_by.has_value()) {
        for (const auto& item : *stmt.order_by) {
            auto dot = item.column.find('.');

            std::string tbl;
            std::string col;

            if (dot == std::string::npos) {
                col = item.column;

                bool found = false;

                for (const auto& [alias, realName] : tables) {
                    if (catalog_.column_exists(realName, col)) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    throw SemanticError(
                        "ORDER BY: column '" + col +
                        "' does not exist in any referenced table.");
                }

            } else {
                tbl = item.column.substr(0, dot);
                col = item.column.substr(dot + 1);

                if (!tables.count(tbl)) {
                    throw SemanticError(
                        "ORDER BY: unknown table alias '" + tbl + "'.");
                }

                if (!catalog_.column_exists(tables.at(tbl), col)) {
                    throw SemanticError(
                        "ORDER BY: column '" + col +
                        "' does not exist in table '" +
                        tables.at(tbl) + "'.");
                }
            }
        }
    }

    // Validate LIMIT
    if (stmt.limit.has_value() && *stmt.limit <= 0) {
        throw SemanticError(
            "LIMIT must be a positive integer (got " +
            std::to_string(*stmt.limit) + ").");
    }
}

// ── Expr walker ───────────────────────────────────────────────────────────────

void SemanticAnalyzer::analyzeExpr(
        const Expr& expr,
        const std::unordered_map<std::string, std::string>& tables) const {

    std::visit([&](const auto& node) {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ColumnRef>) {
            resolveColumn(node, tables);
        }

        else if constexpr (std::is_same_v<T, Literal>) {
            // Nothing to validate — literals are always valid
        }

        else if constexpr (std::is_same_v<T, BinaryOp>) {
            analyzeExpr(*node.left,  tables);
            analyzeExpr(*node.right, tables);
        }

        else if constexpr (std::is_same_v<T, UnaryOp>) {
            analyzeExpr(*node.operand, tables);
        }

        else if constexpr (std::is_same_v<T, AggregateCall>) {
            if (!node.is_star) {
                if (!node.arg) {
                    throw SemanticError(
                        "Aggregate function is missing its argument.");
                }

                analyzeExpr(*node.arg, tables);
            }
        }

    }, expr.node);
}

std::string SemanticAnalyzer::resolveColumn(
        const ColumnRef& ref,
        const std::unordered_map<std::string, std::string>& tables) const {

    if (ref.table.has_value()) {
        // Qualified: table.column — the table must be in the FROM/JOIN set
        const std::string& tblAlias = *ref.table;

        if (!tables.count(tblAlias)) {
            throw SemanticError(
                "Column reference '" + *ref.table + "." + ref.column +
                "': table alias or name '" + *ref.table +
                "' is not in the FROM / JOIN clause.");
        }

        const std::string& realTable = tables.at(tblAlias);

        if (!catalog_.column_exists(realTable, ref.column)) {
            throw SemanticError(
                "Column '" + ref.column +
                "' does not exist in table '" + realTable + "'.");
        }

        return realTable;
    }

    // Unqualified: scan all referenced tables
    std::vector<std::string> matches;

    for (const auto& [alias, realName] : tables) {
        if (catalog_.column_exists(realName, ref.column)) {
            matches.push_back(realName);
        }
    }

    if (matches.empty()) {
        throw SemanticError(
            "Column '" + ref.column +
            "' does not exist in any referenced table.");
    }

    if (matches.size() > 1) {
        throw SemanticError(
            "Column '" + ref.column +
            "' is ambiguous — " +
            "it appears in multiple tables. Qualify it as table.column.");
    }

    return matches[0];
}