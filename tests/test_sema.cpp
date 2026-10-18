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
// UPDATE
// ============================================================================

static void testUpdateOk() {
    auto cat = buildCatalog();

    CHECK(
        analyzeOk(
            cat,
            "UPDATE users SET name = 'Y' WHERE id = 1"
        ),
        "valid UPDATE ok"
    );
}

static void testUpdateMissingTable() {
    auto cat = buildCatalog();

    CHECK(
        analyzeThrows(
            cat,
            "UPDATE ghosts SET name = 'Y'"
        ),
        "UPDATE on missing table throws"
    );
}

static void testUpdateMissingColumn() {
    auto cat = buildCatalog();

    CHECK(
        analyzeThrows(
            cat,
            "UPDATE users SET fake_column = 'Y'"
        ),
        "UPDATE on missing column throws"
    );
}

static void testUpdateMissingWhereColumn() {
    auto cat = buildCatalog();

    CHECK(
        analyzeThrows(
            cat,
            "UPDATE users SET name = 'Y' WHERE fake_column = 1"
        ),
        "UPDATE with missing column in WHERE throws"
    );
}

static void testUpdateDuplicateSetColumn() {
    auto cat = buildCatalog();

    CHECK(
        analyzeThrows(
            cat,
            "UPDATE users SET name = 'Y', name = 'Z'"
        ),
        "UPDATE with duplicate SET columns throws"
    );
}

// ============================================================================
// UPDATE — type validation regression tests (BUG 1 fix)
// ============================================================================

static void testUpdateTypeValidation() {
    // Build a catalog that has all three column types.
    Catalog cat;
    TableSchema t;
    t.name        = "t";
    t.primary_key = "id";
    t.columns = {
        {"id",    DataType::INTEGER, 0, true},   // NOT NULL PK
        {"score", DataType::REAL,    1, false},
        {"label", DataType::TEXT,    2, false},
        {"extra", DataType::INTEGER, 3, false},   // nullable int
    };
    cat.add_table(std::move(t));

    // ── Disallowed cross-type assignments ──────────────────────────────────────

    // INTEGER column <- TEXT literal
    CHECK(
        analyzeThrows(cat, "UPDATE t SET id = 'oops'"),
        "INTEGER col <- TEXT literal must throw"
    );

    // INTEGER column <- TEXT literal (non-PK)
    CHECK(
        analyzeThrows(cat, "UPDATE t SET extra = 'bad'"),
        "INTEGER col <- TEXT literal (non-PK) must throw"
    );

    // TEXT column <- INTEGER literal
    CHECK(
        analyzeThrows(cat, "UPDATE t SET label = 42"),
        "TEXT col <- INTEGER literal must throw"
    );

    // TEXT column <- REAL literal
    CHECK(
        analyzeThrows(cat, "UPDATE t SET label = 3.14"),
        "TEXT col <- REAL literal must throw"
    );

    // REAL column <- TEXT literal
    CHECK(
        analyzeThrows(cat, "UPDATE t SET score = 'hello'"),
        "REAL col <- TEXT literal must throw"
    );

    // PK (INTEGER) <- TEXT literal — must fail BEFORE mutation
    CHECK(
        analyzeThrows(cat, "UPDATE t SET id = 'oops' WHERE id = 1"),
        "PK INTEGER col <- TEXT literal with WHERE must throw"
    );

    // ── Allowed same-type and numeric-widening assignments ────────────────────

    // INTEGER <- INTEGER
    CHECK(
        analyzeOk(cat, "UPDATE t SET id = 99"),
        "INTEGER col <- INTEGER literal ok"
    );

    // REAL <- REAL
    CHECK(
        analyzeOk(cat, "UPDATE t SET score = 3.14"),
        "REAL col <- REAL literal ok"
    );

    // TEXT <- TEXT
    CHECK(
        analyzeOk(cat, "UPDATE t SET label = 'hello'"),
        "TEXT col <- TEXT literal ok"
    );

    // INTEGER <- REAL (numeric widening — matches INSERT behaviour)
    CHECK(
        analyzeOk(cat, "UPDATE t SET id = 3.14"),
        "INTEGER col <- REAL literal (widening) ok"
    );

    // REAL <- INTEGER (numeric widening)
    CHECK(
        analyzeOk(cat, "UPDATE t SET score = 42"),
        "REAL col <- INTEGER literal (widening) ok"
    );

    // ── NULL assignments ───────────────────────────────────────────────────────

    // NULL into nullable column — ok
    CHECK(
        analyzeOk(cat, "UPDATE t SET label = NULL"),
        "NULL into nullable TEXT col ok"
    );
    CHECK(
        analyzeOk(cat, "UPDATE t SET score = NULL"),
        "NULL into nullable REAL col ok"
    );
    CHECK(
        analyzeOk(cat, "UPDATE t SET extra = NULL"),
        "NULL into nullable INTEGER col ok"
    );

    // NULL into NOT NULL column — must throw
    CHECK(
        analyzeThrows(cat, "UPDATE t SET id = NULL"),
        "NULL into NOT NULL col must throw"
    );

    // ── ColumnRef RHS — type is resolved from catalog ─────────────────────────

    // INTEGER col <- INTEGER col (same type)
    CHECK(
        analyzeOk(cat, "UPDATE t SET id = extra"),
        "INTEGER col <- INTEGER col ref ok"
    );
    CHECK(
        analyzeOk(cat, "UPDATE t SET extra = id"),
        "INTEGER col <- INTEGER col ref (reverse) ok"
    );

    // REAL col <- INTEGER col (numeric widening)
    CHECK(
        analyzeOk(cat, "UPDATE t SET score = extra"),
        "REAL col <- INTEGER col ref (widening) ok"
    );

    // INTEGER col <- REAL col (numeric widening)
    CHECK(
        analyzeOk(cat, "UPDATE t SET extra = score"),
        "INTEGER col <- REAL col ref (widening) ok"
    );

    // TEXT col <- INTEGER col — must throw
    CHECK(
        analyzeThrows(cat, "UPDATE t SET label = id"),
        "TEXT col <- INTEGER col ref must throw"
    );

    // INTEGER col <- TEXT col — must throw
    CHECK(
        analyzeThrows(cat, "UPDATE t SET id = label"),
        "INTEGER col <- TEXT col ref must throw"
    );

    // ── Boolean/comparison expression RHS — result is INTEGER ─────────────────

    // INTEGER col <- comparison expr (yields INTEGER) — ok
    CHECK(
        analyzeOk(cat, "UPDATE t SET id = (id = 1)"),
        "INTEGER col <- comparison expr (INTEGER result) ok"
    );

    // TEXT col <- comparison expr (yields INTEGER) — must throw
    CHECK(
        analyzeThrows(cat, "UPDATE t SET label = (id = 1)"),
        "TEXT col <- comparison expr (INTEGER result) must throw"
    );
}

// ============================================================================
// DELETE
// ============================================================================

static void testDeleteOk() {
    auto cat = buildCatalog();

    CHECK(
        analyzeOk(
            cat,
            "DELETE FROM users WHERE id = 1"
        ),
        "valid DELETE ok"
    );

    CHECK(
        analyzeOk(
            cat,
            "DELETE FROM users"
        ),
        "valid DELETE without WHERE ok"
    );
}

static void testDeleteMissingTable() {
    auto cat = buildCatalog();

    CHECK(
        analyzeThrows(
            cat,
            "DELETE FROM ghosts"
        ),
        "DELETE from missing table throws"
    );
}

static void testDeleteMissingWhereColumn() {
    auto cat = buildCatalog();

    CHECK(
        analyzeThrows(
            cat,
            "DELETE FROM users WHERE fake_column = 1"
        ),
        "DELETE with missing column in WHERE throws"
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

    testUpdateOk();
    testUpdateMissingTable();
    testUpdateMissingColumn();
    testUpdateMissingWhereColumn();
    testUpdateDuplicateSetColumn();
    testUpdateTypeValidation();

    testDeleteOk();
    testDeleteMissingTable();
    testDeleteMissingWhereColumn();

    std::cout
        << "Sema: "
        << passed
        << " passed, "
        << failed
        << " failed.\n";

    return failed > 0 ? 1 : 0;
}