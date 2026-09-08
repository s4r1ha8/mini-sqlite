// ============================================================================
// test_parser.cpp — Tests for the recursive-descent Parser
// ============================================================================

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "common/ast.h"
#include "common/token.h"
#include "frontend/tokenizer.h"
#include "frontend/parser.h"

static int passed = 0, failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL [" << msg << "] line " << __LINE__ << "\n"; \
            failed++; \
        } else { \
            passed++; \
        } \
    } while(0)


static Statement parse(const std::string& sql) {
    Tokenizer tok(sql);
    auto tokens = tok.tokenize();
    Parser p(std::move(tokens));
    return p.parse();
}


static bool throwsParseError(const std::string& sql) {
    try {
        parse(sql);
        return false;
    } catch (const ParseError&) {
        return true;
    } catch (...) {
        return false;
    }
}


// ── CREATE TABLE ─────────────────────────────────────────────────────────────

static void testCreateTableBasic() {
    auto stmt =
        parse("CREATE TABLE users "
              "(id INTEGER PRIMARY KEY, name TEXT NOT NULL, score REAL)");

    CHECK(
        std::holds_alternative<CreateTableStmt>(stmt),
        "CreateTableStmt variant"
    );

    const auto& ct =
        std::get<CreateTableStmt>(stmt);

    CHECK(ct.table_name == "users", "table name");
    CHECK(ct.columns.size() == 3, "3 columns");

    CHECK(ct.columns[0].name == "id", "col0 name");
    CHECK(
        ct.columns[0].type == DataType::INTEGER,
        "col0 type INTEGER"
    );
    CHECK(
        ct.columns[0].is_primary_key == true,
        "col0 is PK"
    );
    CHECK(
        ct.columns[0].is_not_null == true,
        "col0 NOT NULL (implied by PK)"
    );

    CHECK(
        ct.columns[1].name == "name",
        "col1 name"
    );
    CHECK(
        ct.columns[1].type == DataType::TEXT,
        "col1 type TEXT"
    );
    CHECK(
        ct.columns[1].is_not_null == true,
        "col1 NOT NULL"
    );

    CHECK(
        ct.columns[2].name == "score",
        "col2 name"
    );
    CHECK(
        ct.columns[2].type == DataType::REAL,
        "col2 type REAL"
    );
    CHECK(
        ct.columns[2].is_primary_key == false,
        "col2 not PK"
    );
}


static void testCreateTableSingleCol() {
    auto stmt =
        parse("CREATE TABLE t (x INTEGER)");

    const auto& ct =
        std::get<CreateTableStmt>(stmt);

    CHECK(
        ct.table_name == "t",
        "single col table name"
    );

    CHECK(
        ct.columns.size() == 1,
        "single col count"
    );

    CHECK(
        ct.columns[0].name == "x",
        "single col name"
    );
}


static void testCreateTableErrors() {
    CHECK(
        throwsParseError("CREATE TABLE"),
        "missing table name"
    );

    CHECK(
        throwsParseError("CREATE TABLE t"),
        "missing parens"
    );

    CHECK(
        throwsParseError("CREATE TABLE t (x)"),
        "missing type"
    );

    CHECK(
        throwsParseError("CREATE TABLE t ()"),
        "empty column list"
    );
}


// ── CREATE INDEX ─────────────────────────────────────────────────────────────

static void testCreateIndex() {
    auto stmt =
        parse(
            "CREATE INDEX idx_users_name "
            "ON users (name)"
        );

    CHECK(
        std::holds_alternative<CreateIndexStmt>(stmt),
        "CreateIndexStmt"
    );

    const auto& ci =
        std::get<CreateIndexStmt>(stmt);

    CHECK(
        ci.index_name == "idx_users_name",
        "index name"
    );

    CHECK(
        ci.table_name == "users",
        "table name"
    );

    CHECK(
        ci.column_name == "name",
        "column name"
    );
}


// ── INSERT ───────────────────────────────────────────────────────────────────

static void testInsertBasic() {
    auto stmt =
        parse(
            "INSERT INTO users "
            "VALUES (1, 'Sarthak', 9.5)"
        );

    CHECK(
        std::holds_alternative<InsertStmt>(stmt),
        "InsertStmt"
    );

    const auto& ins =
        std::get<InsertStmt>(stmt);

    CHECK(
        ins.table_name == "users",
        "insert table name"
    );

    CHECK(
        ins.values.size() == 3,
        "3 values"
    );

    CHECK(
        std::holds_alternative<int64_t>(
            ins.values[0].value
        ),
        "v0 integer"
    );

    CHECK(
        std::get<int64_t>(
            ins.values[0].value
        ) == 1,
        "v0 = 1"
    );

    CHECK(
        std::holds_alternative<std::string>(
            ins.values[1].value
        ),
        "v1 string"
    );

    CHECK(
        std::get<std::string>(
            ins.values[1].value
        ) == "Sarthak",
        "v1 = Sarthak"
    );

    CHECK(
        std::holds_alternative<double>(
            ins.values[2].value
        ),
        "v2 real"
    );
}


static void testInsertNull() {
    auto stmt =
        parse("INSERT INTO t VALUES (NULL)");

    const auto& ins =
        std::get<InsertStmt>(stmt);

    CHECK(
        std::holds_alternative<std::monostate>(
            ins.values[0].value
        ),
        "NULL value"
    );
}


static void testInsertErrors() {
    CHECK(
        throwsParseError("INSERT users VALUES (1)"),
        "missing INTO"
    );

    CHECK(
        throwsParseError("INSERT INTO users (1)"),
        "missing VALUES"
    );

    CHECK(
        throwsParseError("INSERT INTO users VALUES 1"),
        "missing parens"
    );
}


// ── SELECT ───────────────────────────────────────────────────────────────────

static void testSelectStar() {
    auto stmt =
        parse("SELECT * FROM users");

    const auto& sel =
        std::get<SelectStmt>(stmt);

    CHECK(
        sel.columns.size() == 1 &&
        sel.columns[0] == "*",
        "SELECT *"
    );

    CHECK(
        sel.from.size() == 1 &&
        sel.from[0].table_name == "users",
        "FROM users"
    );

    CHECK(
        !sel.where.has_value(),
        "no WHERE"
    );
}


static void testSelectColumns() {
    auto stmt =
        parse("SELECT id, name FROM users");

    const auto& sel =
        std::get<SelectStmt>(stmt);

    CHECK(
        sel.columns.size() == 2,
        "2 columns"
    );

    CHECK(
        sel.columns[0] == "id",
        "col 0: id"
    );

    CHECK(
        sel.columns[1] == "name",
        "col 1: name"
    );
}


static void testSelectWhere() {
    auto stmt =
        parse(
            "SELECT * FROM users "
            "WHERE id = 5"
        );

    const auto& sel =
        std::get<SelectStmt>(stmt);

    CHECK(
        sel.where.has_value(),
        "has WHERE"
    );

    const auto& binop =
        std::get<BinaryOp>(
            sel.where->node
        );

    CHECK(
        binop.op == "=",
        "WHERE op ="
    );

    const auto& col =
        std::get<ColumnRef>(
            binop.left->node
        );

    CHECK(
        col.column == "id",
        "WHERE column id"
    );

    const auto& lit =
        std::get<Literal>(
            binop.right->node
        );

    CHECK(
        std::get<int64_t>(
            lit.value
        ) == 5,
        "WHERE literal 5"
    );
}


static void testSelectWhereAndOr() {
    auto stmt =
        parse(
            "SELECT * FROM t "
            "WHERE a = 1 AND b = 2 OR c = 3"
        );

    const auto& sel =
        std::get<SelectStmt>(stmt);

    CHECK(
        sel.where.has_value(),
        "has WHERE"
    );

    // OR has lower precedence, so root should be OR.
    const auto& orOp =
        std::get<BinaryOp>(
            sel.where->node
        );

    CHECK(
        orOp.op == "OR",
        "root op is OR"
    );

    const auto& andOp =
        std::get<BinaryOp>(
            orOp.left->node
        );

    CHECK(
        andOp.op == "AND",
        "left of OR is AND"
    );
}


static void testSelectWhereNot() {
    auto stmt =
        parse(
            "SELECT * FROM t "
            "WHERE NOT id = 5"
        );

    const auto& sel =
        std::get<SelectStmt>(stmt);

    CHECK(
        sel.where.has_value(),
        "has WHERE NOT"
    );

    const auto& notOp =
        std::get<UnaryOp>(
            sel.where->node
        );

    CHECK(
        notOp.op == "NOT",
        "NOT unary op"
    );
}


static void testSelectOrderBy() {
    auto stmt =
        parse(
            "SELECT * FROM users "
            "ORDER BY name DESC, id ASC"
        );

    const auto& sel =
        std::get<SelectStmt>(stmt);

    CHECK(
        sel.order_by.has_value(),
        "has ORDER BY"
    );

    const auto& ob =
        *sel.order_by;

    CHECK(
        ob.size() == 2,
        "2 order items"
    );

    CHECK(
        ob[0].column == "name",
        "order 0: name"
    );

    CHECK(
        ob[0].ascending == false,
        "order 0: DESC"
    );

    CHECK(
        ob[1].column == "id",
        "order 1: id"
    );

    CHECK(
        ob[1].ascending == true,
        "order 1: ASC"
    );
}


static void testSelectGroupBy() {
    auto stmt =
        parse(
            "SELECT * FROM orders "
            "GROUP BY status"
        );

    const auto& sel =
        std::get<SelectStmt>(stmt);

    CHECK(
        sel.group_by.has_value(),
        "has GROUP BY"
    );

    CHECK(
        (*sel.group_by)[0] == "status",
        "group by: status"
    );
}


static void testSelectJoin() {
    auto stmt =
        parse(
            "SELECT * FROM users u "
            "JOIN orders o "
            "ON u.id = o.user_id"
        );

    const auto& sel =
        std::get<SelectStmt>(stmt);

    CHECK(
        sel.from[0].table_name == "users",
        "FROM users"
    );

    CHECK(
        sel.from[0].alias.has_value() &&
        *sel.from[0].alias == "u",
        "alias u"
    );

    CHECK(
        sel.joins.size() == 1,
        "1 JOIN"
    );

    CHECK(
        sel.joins[0].table.table_name == "orders",
        "JOIN orders"
    );

    CHECK(
        sel.joins[0].table.alias.has_value() &&
        *sel.joins[0].table.alias == "o",
        "JOIN alias o"
    );
}


static void testSelectQualifiedColumn() {
    auto stmt =
        parse(
            "SELECT users.name FROM users"
        );

    const auto& sel =
        std::get<SelectStmt>(stmt);

    CHECK(
        sel.columns[0] == "users.name",
        "qualified column in select list"
    );
}


static void testSelectLimit() {
    auto stmt =
        parse(
            "SELECT * FROM users LIMIT 10"
        );

    const auto& sel =
        std::get<SelectStmt>(stmt);

    CHECK(
        sel.limit.has_value() &&
        *sel.limit == 10,
        "LIMIT 10"
    );
}


static void testSelectAggregate() {

    // COUNT(*) is now supported in the SELECT list.
    {
        auto stmt =
            parse("SELECT COUNT(*) FROM users");

        const auto& sel =
            std::get<SelectStmt>(stmt);

        CHECK(
            sel.columns.size() == 1 &&
            sel.columns[0] == "COUNT(*)",
            "aggregate in select list: COUNT(*) textual projection"
        );

        CHECK(
            sel.aggregate_columns.size() == 1 &&
            sel.aggregate_columns[0].func == AggFunc::COUNT &&
            sel.aggregate_columns[0].is_star,
            "aggregate in select list: COUNT(*) AST"
        );
    }


    // COUNT(column)
    {
        auto stmt =
            parse("SELECT COUNT(id) FROM users");

        const auto& sel =
            std::get<SelectStmt>(stmt);

        CHECK(
            sel.columns.size() == 1 &&
            sel.columns[0] == "COUNT(id)",
            "aggregate in select list: COUNT column projection"
        );

        CHECK(
            sel.aggregate_columns.size() == 1 &&
            sel.aggregate_columns[0].func == AggFunc::COUNT &&
            !sel.aggregate_columns[0].is_star,
            "aggregate in select list: COUNT column AST"
        );

        CHECK(
            sel.aggregate_columns[0].arg != nullptr,
            "aggregate in select list: COUNT argument exists"
        );

        if (sel.aggregate_columns[0].arg) {
            const auto& arg =
                std::get<ColumnRef>(
                    sel.aggregate_columns[0].arg->node
                );

            CHECK(
                arg.column == "id",
                "aggregate in select list: COUNT argument id"
            );
        }
    }


    // SUM(column)
    {
        auto stmt =
            parse("SELECT SUM(score) FROM users");

        const auto& sel =
            std::get<SelectStmt>(stmt);

        CHECK(
            sel.aggregate_columns.size() == 1 &&
            sel.aggregate_columns[0].func == AggFunc::SUM,
            "aggregate in select list: SUM AST"
        );
    }


    // AVG(column)
    {
        auto stmt =
            parse("SELECT AVG(score) FROM users");

        const auto& sel =
            std::get<SelectStmt>(stmt);

        CHECK(
            sel.aggregate_columns.size() == 1 &&
            sel.aggregate_columns[0].func == AggFunc::AVG,
            "aggregate in select list: AVG AST"
        );
    }


    // MIN(column)
    {
        auto stmt =
            parse("SELECT MIN(score) FROM users");

        const auto& sel =
            std::get<SelectStmt>(stmt);

        CHECK(
            sel.aggregate_columns.size() == 1 &&
            sel.aggregate_columns[0].func == AggFunc::MIN,
            "aggregate in select list: MIN AST"
        );
    }


    // MAX(column)
    {
        auto stmt =
            parse("SELECT MAX(score) FROM users");

        const auto& sel =
            std::get<SelectStmt>(stmt);

        CHECK(
            sel.aggregate_columns.size() == 1 &&
            sel.aggregate_columns[0].func == AggFunc::MAX,
            "aggregate in select list: MAX AST"
        );
    }
}


// ── Transaction statements ───────────────────────────────────────────────────

static void testTxn() {

    auto b =
        parse("BEGIN");

    CHECK(
        std::get<TxnStmt>(b).kind ==
        TxnStmt::Kind::BEGIN,
        "BEGIN"
    );


    auto c =
        parse("COMMIT");

    CHECK(
        std::get<TxnStmt>(c).kind ==
        TxnStmt::Kind::COMMIT,
        "COMMIT"
    );


    auto r =
        parse("ROLLBACK");

    CHECK(
        std::get<TxnStmt>(r).kind ==
        TxnStmt::Kind::ROLLBACK,
        "ROLLBACK"
    );
}


// ── Semicolon handling ────────────────────────────────────────────────────────

static void testSemicolon() {

    // Trailing semicolon should be silently consumed.
    auto stmt =
        parse("SELECT * FROM t;");

    CHECK(
        std::holds_alternative<SelectStmt>(stmt),
        "semicolon allowed"
    );
}


// ── main ──────────────────────────────────────────────────────────────────────

int main() {

    testCreateTableBasic();
    testCreateTableSingleCol();
    testCreateTableErrors();

    testCreateIndex();

    testInsertBasic();
    testInsertNull();
    testInsertErrors();

    testSelectStar();
    testSelectColumns();
    testSelectWhere();
    testSelectWhereAndOr();
    testSelectWhereNot();
    testSelectOrderBy();
    testSelectGroupBy();
    testSelectJoin();
    testSelectQualifiedColumn();
    testSelectLimit();
    testSelectAggregate();

    testTxn();
    testSemicolon();


    std::cout
        << "Parser: "
        << passed
        << " passed, "
        << failed
        << " failed.\n";

    return failed > 0 ? 1 : 0;
}