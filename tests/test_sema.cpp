// ============================================================================
// test_sema.cpp — Tests for the SemanticAnalyzer
// ============================================================================

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

#include "common/ast.h"
#include "common/token.h"

#include "frontend/tokenizer.h"
#include "frontend/parser.h"
#include "frontend/catalog.h"
#include "frontend/sema.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL [" << msg << "] line " << __LINE__ << "\n"; \
            failed++; \
        } else { \
            passed++; \
        } \
    } while (0)

// ============================================================================
// Helpers
// ============================================================================

// Helper: parse + analyze in one shot
static bool analyzeOk(
    Catalog& cat,
    const std::string& sql
) {
    try {
        Tokenizer tok(sql);
        Parser p(tok.tokenize());
        Statement stmt = p.parse();

        SemanticAnalyzer sema(cat);
        sema.analyze(stmt);

        return true;
    }
    catch (const SemanticError&) {
        return false;
    }
    catch (const ParseError&) {
        return false;
    }
}

static bool analyzeThrows(
    Catalog& cat,
    const std::string& sql
) {
    try {
        Tokenizer tok(sql);
        Parser p(tok.tokenize());
        Statement stmt = p.parse();

        SemanticAnalyzer sema(cat);
        sema.analyze(stmt);

        return false;
    }
    catch (const SemanticError&) {
        return true;
    }
}

// ============================================================================
// Setup: build a catalog with users + orders tables
// ============================================================================

static Catalog buildCatalog() {
    Catalog cat;

    TableSchema users;

    users.name =
        "users";

    users.primary_key =
        "id";

    users.columns = {
        {"id",   DataType::INTEGER, 0, true},
        {"name", DataType::TEXT,    1, true},
    };

    cat.add_table(
        std::move(users)
    );


    TableSchema orders;

    orders.name =
        "orders";

    orders.primary_key =
        "order_id";

    orders.columns = {
        {"order_id", DataType::INTEGER, 0, true},
        {"user_id",  DataType::INTEGER, 1, true},
        {"total",    DataType::REAL,    2, false},
    };

    cat.add_table(
        std::move(orders)
    );

    return cat;
}

// ============================================================================
// CREATE TABLE
// ============================================================================

static void testCreateTableOk() {
    Catalog cat;

    CHECK(
        analyzeOk(
            cat,
            "CREATE TABLE t (x INTEGER PRIMARY KEY)"
        ),
        "valid CREATE TABLE ok"
    );

    CHECK(
        cat.table_exists("t"),
        "catalog updated after CREATE TABLE"
    );
}

static void testCreateTableDuplicate() {
    Catalog cat;

    analyzeOk(
        cat,
        "CREATE TABLE t (x INTEGER)"
    );

    CHECK(
        analyzeThrows(
            cat,
            "CREATE TABLE t (y TEXT)"
        ),
        "duplicate table throws SemanticError"
    );
}

static void testCreateTableDupCols() {
    Catalog cat;

    CHECK(
        analyzeThrows(
            cat,
            "CREATE TABLE t (x INTEGER, x TEXT)"
        ),
        "duplicate column name throws"
    );
}

static void testCreateTableMultiplePK() {
    Catalog cat;

    CHECK(
        analyzeThrows(
            cat,
            "CREATE TABLE t "
            "(a INTEGER PRIMARY KEY, b TEXT PRIMARY KEY)"
        ),
        "multiple PK throws"
    );
}

// ============================================================================
// INSERT
// ============================================================================

static void testInsertOk() {
    auto cat = buildCatalog();

    CHECK(
        analyzeOk(
            cat,
            "INSERT INTO users VALUES (1, 'Sarthak')"
        ),
        "valid INSERT ok"
    );
}

static void testInsertMissingTable() {
    auto cat = buildCatalog();

    CHECK(
        analyzeThrows(
            cat,
            "INSERT INTO nonexistent VALUES (1)"
        ),
        "INSERT into missing table throws"
    );
}

static void testInsertWrongCount() {
    auto cat = buildCatalog();

    CHECK(
        analyzeThrows(
            cat,
            "INSERT INTO users VALUES (1)"
        ),
        "INSERT wrong value count throws"
    );

    CHECK(
        analyzeThrows(
            cat,
            "INSERT INTO users VALUES (1, 'x', 3)"
        ),
        "INSERT too many values throws"
    );
}

static void testInsertWrongType() {
    auto cat = buildCatalog();

    // id is INTEGER, passing a string
    CHECK(
        analyzeThrows(
            cat,
            "INSERT INTO users VALUES ('abc', 'Sarthak')"
        ),
        "INSERT wrong type throws"
    );
}

static void testInsertNull() {
    Catalog cat;

    TableSchema t;

    t.name =
        "t";

    t.primary_key =
        "a";

    t.columns = {
        {"a", DataType::INTEGER, 0, false}
    };

    cat.add_table(
        std::move(t)
    );

    CHECK(
        analyzeOk(
            cat,
            "INSERT INTO t VALUES (NULL)"
        ),
        "INSERT NULL into nullable column ok"
    );
}

static void testInsertNullNotNull() {
    Catalog cat;

    TableSchema t;

    t.name =
        "t";

    t.primary_key =
        "a";

    t.columns = {
        {"a", DataType::INTEGER, 0, true}
    };

    cat.add_table(
        std::move(t)
    );

    CHECK(
        analyzeThrows(
            cat,
            "INSERT INTO t VALUES (NULL)"
        ),
        "INSERT NULL into NOT NULL column throws"
    );
}

// ============================================================================
// SELECT
// ============================================================================

static void testSelectOk() {
    auto cat = buildCatalog();

    CHECK(
        analyzeOk(
            cat,
            "SELECT * FROM users"
        ),
        "SELECT * ok"
    );

    CHECK(
        analyzeOk(
            cat,
            "SELECT id, name FROM users"
        ),
        "SELECT cols ok"
    );

    CHECK(
        analyzeOk(
            cat,
            "SELECT * FROM users WHERE id = 5"
        ),
        "SELECT WHERE ok"
    );

    CHECK(
        analyzeOk(
            cat,
            "SELECT * FROM users ORDER BY name"
        ),
        "SELECT ORDER BY ok"
    );

    CHECK(
        analyzeOk(
            cat,
            "SELECT * FROM users "
            "WHERE id = 1 AND name = 'x'"
        ),
        "SELECT AND ok"
    );
}

static void testSelectMissingTable() {
    auto cat = buildCatalog();

    CHECK(
        analyzeThrows(
            cat,
            "SELECT * FROM ghosts"
        ),
        "SELECT from missing table throws"
    );
}

static void testSelectMissingColumn() {
    auto cat = buildCatalog();

    CHECK(
        analyzeThrows(
            cat,
            "SELECT email FROM users"
        ),
        "SELECT missing column throws"
    );

    CHECK(
        analyzeThrows(
            cat,
            "SELECT * FROM users WHERE email = 'x'"
        ),
        "WHERE on missing column throws"
    );
}

static void testSelectJoinOk() {
    auto cat = buildCatalog();

    CHECK(
        analyzeOk(
            cat,
            "SELECT * FROM users u "
            "JOIN orders o ON u.id = o.user_id"
        ),
        "JOIN with valid columns ok"
    );
}

static void testSelectJoinMissingTable() {
    auto cat = buildCatalog();

    CHECK(
        analyzeThrows(
            cat,
            "SELECT * FROM users "
            "JOIN nonexistent "
            "ON users.id = nonexistent.user_id"
        ),
        "JOIN missing table throws"
    );
}

static void testSelectAmbiguousColumn() {
    // Two tables both have a column called "id" —
    // unqualified "id" is ambiguous.
    Catalog cat;

    TableSchema a;

    a.name =
        "a";

    a.primary_key =
        "id";

    a.columns = {
        {"id", DataType::INTEGER, 0, false}
    };

    cat.add_table(
        std::move(a)
    );


    TableSchema b;

    b.name =
        "b";

    b.primary_key =
        "id";

    b.columns = {
        {"id", DataType::INTEGER, 0, false}
    };

    cat.add_table(
        std::move(b)
    );

    CHECK(
        analyzeThrows(
            cat,
            "SELECT * FROM a "
            "JOIN b ON a.id = b.id "
            "WHERE id = 1"
        ),
        "ambiguous column in WHERE throws"
    );
}

static void testSelectOrderByMissingColumn() {
    auto cat = buildCatalog();

    CHECK(
        analyzeThrows(
            cat,
            "SELECT * FROM users ORDER BY nonexistent"
        ),
        "ORDER BY missing column throws"
    );
}

// ============================================================================
// SELECT aggregates + HAVING
// ============================================================================

static void testSelectAggregateAndHaving() {
    auto cat = buildCatalog();

    CHECK(
        analyzeOk(
            cat,
            "SELECT COUNT(*) FROM users"
        ),
        "aggregate COUNT(*) ok"
    );

    CHECK(
        analyzeOk(
            cat,
            "SELECT COUNT(id) FROM users"
        ),
        "aggregate COUNT(column) ok"
    );

    CHECK(
        analyzeOk(
            cat,
            "SELECT SUM(total), AVG(total), "
            "MIN(total), MAX(total) "
            "FROM orders"
        ),
        "aggregate numeric functions ok"
    );

    CHECK(
        analyzeOk(
            cat,
            "SELECT user_id, COUNT(*) "
            "FROM orders "
            "GROUP BY user_id "
            "HAVING COUNT(*) > 1"
        ),
        "GROUP BY with HAVING aggregate ok"
    );

    CHECK(
        analyzeThrows(
            cat,
            "SELECT user_id, COUNT(*) "
            "FROM orders "
            "HAVING COUNT(*) > 1"
        ),
        "HAVING without GROUP BY throws"
    );

    CHECK(
        analyzeThrows(
            cat,
            "SELECT COUNT(does_not_exist) FROM users"
        ),
        "aggregate on missing column throws"
    );
}

// ============================================================================
// CREATE INDEX
// ============================================================================

static void testCreateIndexOk() {
    auto cat = buildCatalog();

    CHECK(
        analyzeOk(
            cat,
            "CREATE INDEX idx_name ON users (name)"
        ),
        "valid CREATE INDEX ok"
    );

    CHECK(
        cat.get_table("users")->indexes.size() == 1,
        "index added to catalog"
    );
}

static void testCreateIndexMissingTable() {
    auto cat = buildCatalog();

    CHECK(
        analyzeThrows(
            cat,
            "CREATE INDEX idx ON ghost (name)"
        ),
        "CREATE INDEX on missing table throws"
    );
}

static void testCreateIndexMissingColumn() {
    auto cat = buildCatalog();

    CHECK(
        analyzeThrows(
            cat,
            "CREATE INDEX idx ON users (email)"
        ),
        "CREATE INDEX on missing column throws"
    );
}

// ============================================================================
// main
// ============================================================================

int main() {

    testCreateTableOk();
    testCreateTableDuplicate();
    testCreateTableDupCols();
    testCreateTableMultiplePK();

    testInsertOk();
    testInsertMissingTable();
    testInsertWrongCount();
    testInsertWrongType();
    testInsertNull();
    testInsertNullNotNull();

    testSelectOk();
    testSelectMissingTable();
    testSelectMissingColumn();
    testSelectJoinOk();
    testSelectJoinMissingTable();
    testSelectAmbiguousColumn();
    testSelectOrderByMissingColumn();
    testSelectAggregateAndHaving();

    testCreateIndexOk();
    testCreateIndexMissingTable();
    testCreateIndexMissingColumn();

    std::cout
        << "Sema: "
        << passed
        << " passed, "
        << failed
        << " failed.\n";

    return failed > 0 ? 1 : 0;
}