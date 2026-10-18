// ============================================================================
// sema.cpp — Semantic analysis implementation.
// ============================================================================

#include "frontend/sema.h"
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

SemanticAnalyzer::SemanticAnalyzer(Catalog& catalog) : catalog_(catalog) {}

// ── Public entry point ────────────────────────────────────────────────────────

void SemanticAnalyzer::analyze(const Statement& stmt) {
    std::visit([this](const auto& s) {
        using T = std::decay_t<decltype(s)>;
        if      constexpr (std::is_same_v<T, CreateTableStmt>) analyzeCreateTable(s);
        else if constexpr (std::is_same_v<T, CreateIndexStmt>) analyzeCreateIndex(s);
        else if constexpr (std::is_same_v<T, InsertStmt>)      analyzeInsert(s);
        else if constexpr (std::is_same_v<T, SelectStmt>)      analyzeSelect(s);
        else if constexpr (std::is_same_v<T, UpdateStmt>)      analyzeUpdate(s);
        else if constexpr (std::is_same_v<T, DeleteStmt>)      analyzeDelete(s);
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

// ── UPDATE ────────────────────────────────────────────────────────────────────
//
// Type-checking helpers for SET RHS expressions.
//
// inferExprType() performs a best-effort static analysis to determine the
// DataType that an Expr will produce at runtime.  It returns nullopt when the
// type genuinely cannot be determined statically (e.g., arithmetic expressions
// that mix column references of different types).
//
// isAssignmentCompatible() mirrors INSERT's type rules:
//   INTEGER  ← INTEGER   ok
//   INTEGER  ← REAL      ok (numeric widening — matches INSERT behaviour)
//   INTEGER  ← TEXT      REJECT
//   REAL     ← INTEGER   ok
//   REAL     ← REAL      ok
//   REAL     ← TEXT      REJECT
//   TEXT     ← TEXT      ok
//   TEXT     ← INTEGER   REJECT
//   TEXT     ← REAL      REJECT
// NULL (monostate) is validated by the caller via the NOT NULL flag.

static std::optional<DataType>
inferExprType(const Expr& expr,
              const std::string& table_name,
              const Catalog& catalog,
              const std::unordered_map<std::string, std::string>& tables) {
    return std::visit([&](const auto& node) -> std::optional<DataType> {
        using T = std::decay_t<decltype(node)>;

        // ── Literal: type is determined directly from the variant ──────────
        if constexpr (std::is_same_v<T, Literal>) {
            return std::visit([](const auto& v) -> std::optional<DataType> {
                using V = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<V, std::monostate>) return std::nullopt; // NULL
                if constexpr (std::is_same_v<V, int64_t>)       return DataType::INTEGER;
                if constexpr (std::is_same_v<V, double>)        return DataType::REAL;
                if constexpr (std::is_same_v<V, std::string>)   return DataType::TEXT;
                return std::nullopt;
            }, node.value);
        }

        // ── ColumnRef: look up the column type in the catalog ─────────────
        if constexpr (std::is_same_v<T, ColumnRef>) {
            // Resolve the table this column belongs to
            std::string real_table;
            if (node.table.has_value()) {
                auto it = tables.find(*node.table);
                if (it != tables.end()) real_table = it->second;
            } else {
                // Unqualified: check each referenced table
                for (const auto& [alias, tname] : tables) {
                    if (catalog.column_exists(tname, node.column)) {
                        real_table = tname;
                        break;
                    }
                }
            }
            if (!real_table.empty()) {
                return catalog.column_type(real_table, node.column);
            }
            return std::nullopt;
        }

        // ── BinaryOp / UnaryOp: comparison and logical operators always
        //    produce an INTEGER (0 or 1) at runtime.  Arithmetic expressions
        //    (+,-,*,/) may produce INTEGER or REAL depending on operands;
        //    for safety we only claim INTEGER for operators that definitely
        //    yield boolean-integer results (AND, OR, =, !=, <, >, <=, >=).
        //    For arithmetic we return nullopt so the caller does not
        //    over-constrain.
        if constexpr (std::is_same_v<T, BinaryOp>) {
            static const std::unordered_set<std::string> bool_ops = {
                "AND","OR","=","!=","<",">","<=",">="
            };
            if (bool_ops.count(node.op)) return DataType::INTEGER;
            return std::nullopt; // arithmetic — cannot determine statically
        }

        if constexpr (std::is_same_v<T, UnaryOp>) {
            // NOT always yields INTEGER (0 or 1)
            return DataType::INTEGER;
        }

        // ── AggregateCall: invalid inside a SET clause ─────────────────────
        if constexpr (std::is_same_v<T, AggregateCall>) {
            return std::nullopt; // handled separately — caller rejects this
        }

        return std::nullopt;
    }, expr.node);
}

static bool isAssignmentCompatible(DataType target, DataType source) {
    if (target == source) return true;
    // Numeric widening: INTEGER <-> REAL allowed (matches INSERT semantics)
    if (target == DataType::INTEGER && source == DataType::REAL) return true;
    if (target == DataType::REAL    && source == DataType::INTEGER) return true;
    // Everything else (TEXT <-> numeric, numeric <-> TEXT) is incompatible
    return false;
}

void SemanticAnalyzer::analyzeUpdate(const UpdateStmt& stmt) {
    if (!catalog_.table_exists(stmt.table_name)) {
        throw SemanticError(
            "UPDATE: table '" + stmt.table_name + "' does not exist.");
    }

    std::unordered_map<std::string, std::string> tables = {
        {stmt.table_name, stmt.table_name}
    };

    std::unordered_set<std::string> seen_columns;

    for (const auto& assign : stmt.updates) {
        if (!catalog_.column_exists(stmt.table_name, assign.column)) {
            throw SemanticError(
                "UPDATE: column '" + assign.column +
                "' does not exist in table '" + stmt.table_name + "'.");
        }

        if (!seen_columns.insert(assign.column).second) {
            throw SemanticError(
                "UPDATE: duplicate column '" + assign.column + "' in SET clause.");
        }

        // ── Validate RHS column references ────────────────────────────────
        analyzeExpr(assign.value, tables);

        // ── Reject aggregate functions in SET context ─────────────────────
        if (std::holds_alternative<AggregateCall>(assign.value.node)) {
            throw SemanticError(
                "UPDATE: aggregate functions are not allowed in SET clause.");
        }

        // ── Static type check: RHS type must be compatible with target ─────
        // Retrieve the declared type of the target column.
        auto target_type_opt = catalog_.column_type(stmt.table_name, assign.column);
        if (!target_type_opt.has_value()) continue; // column doesn't exist — caught above
        DataType target_type = *target_type_opt;

        // NULL literal: validate NOT NULL constraint only.
        if (const auto* lit = std::get_if<Literal>(&assign.value.node)) {
            if (std::holds_alternative<std::monostate>(lit->value)) {
                // NULL — NOT NULL check is a runtime concern (we allow NULL here
                // just as INSERT does at sema level when is_not_null is false).
                const TableSchema* s = catalog_.get_table(stmt.table_name);
                if (s) {
                    for (const auto& col : s->columns) {
                        if (col.name == assign.column && col.is_not_null) {
                            throw SemanticError(
                                "UPDATE: column '" + assign.column +
                                "' is NOT NULL but a NULL value was assigned.");
                        }
                    }
                }
                continue; // NULL type check done
            }
        }

        // Infer static type of the RHS expression.
        auto rhs_type_opt = inferExprType(assign.value, stmt.table_name,
                                          catalog_, tables);

        if (rhs_type_opt.has_value()) {
            DataType rhs_type = *rhs_type_opt;
            if (!isAssignmentCompatible(target_type, rhs_type)) {
                throw SemanticError(
                    "UPDATE: cannot assign " + dataTypeToString(rhs_type) +
                    " expression to column '" + assign.column +
                    "' of type " + dataTypeToString(target_type) + ".");
            }
        }
        // If rhs_type_opt is nullopt (complex arithmetic expression whose type
        // cannot be determined statically), allow it through — the evaluator
        // will produce an appropriate runtime error if there is a mismatch.
    }

    if (stmt.where.has_value()) {
        analyzeExpr(*stmt.where, tables);
    }
}

// ── DELETE ────────────────────────────────────────────────────────────────────

void SemanticAnalyzer::analyzeDelete(const DeleteStmt& stmt) {
    if (!catalog_.table_exists(stmt.table_name)) {
        throw SemanticError(
            "DELETE: table '" + stmt.table_name + "' does not exist.");
    }

    std::unordered_map<std::string, std::string> tables = {
        {stmt.table_name, stmt.table_name}
    };

    if (stmt.where.has_value()) {
        analyzeExpr(*stmt.where, tables);
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

    // Validate ORDER BY expressions
    if (stmt.order_by.has_value()) {
        for (const auto& item : *stmt.order_by) {
            analyzeExpr(item.expr, tables);
        }
    }

    // Validate LIMIT
    if (stmt.limit.has_value() && *stmt.limit < 0) {
        throw SemanticError(
            "LIMIT must be a non-negative integer (got " +
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