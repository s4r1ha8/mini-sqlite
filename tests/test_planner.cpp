// ============================================================================
// test_planner.cpp — Tests for the Query Planner
// ============================================================================

#include <cassert>
#include <iostream>
#include <string>
#include <variant>
#include "common/ast.h"
#include "common/token.h"
#include "frontend/tokenizer.h"
#include "frontend/parser.h"
#include "frontend/catalog.h"
#include "frontend/sema.h"
#include "frontend/planner.h"

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

// Helper: parse → sema → plan → return the root OperatorNode
static std::unique_ptr<OperatorNode> plan(Catalog& cat, const std::string& sql) {
    Tokenizer tok(sql);
    Parser p(tok.tokenize());
    Statement stmt = p.parse();
    SemanticAnalyzer sema(cat);
    sema.analyze(stmt);
    Planner planner(cat);
    return planner.plan(std::get<SelectStmt>(stmt));
}

// ── Build a catalog with users (indexed on name) + orders ────────────────────

static Catalog buildCatalog() {
    Catalog cat;

    TableSchema users;
    users.name        = "users";
    users.primary_key = "id";
    users.columns     = {
        {"id",   DataType::INTEGER, 0, true},
        {"name", DataType::TEXT,    1, false},
    };
    cat.add_table(std::move(users));

    // Add an index on users.name
    cat.add_index("users", {"idx_users_name", "name", 42});

    TableSchema orders;
    orders.name        = "orders";
    orders.primary_key = "order_id";
    orders.columns     = {
        {"order_id", DataType::INTEGER, 0, true},
        {"user_id",  DataType::INTEGER, 1, false},
        {"total",    DataType::REAL,    2, false},
    };
    cat.add_table(std::move(orders));

    return cat;
}

// ── Helpers to inspect the tree ──────────────────────────────────────────────

static const ProjectOp* asProject(const OperatorNode& n) {
    return std::get_if<ProjectOp>(&n.v);
}
static const FilterOp* asFilter(const OperatorNode& n) {
    return std::get_if<FilterOp>(&n.v);
}
static const SeqScanOp* asSeqScan(const OperatorNode& n) {
    return std::get_if<SeqScanOp>(&n.v);
}
static const IndexScanOp* asIndexScan(const OperatorNode& n) {
    return std::get_if<IndexScanOp>(&n.v);
}
static const SortOp* asSort(const OperatorNode& n) {
    return std::get_if<SortOp>(&n.v);
}
static const NestedLoopJoinOp* asJoin(const OperatorNode& n) {
    return std::get_if<NestedLoopJoinOp>(&n.v);
}
static const AggregateOp* asAggregate(const OperatorNode& n) {
    return std::get_if<AggregateOp>(&n.v);
}

// ── Tests ─────────────────────────────────────────────────────────────────────

static void testSimpleSeqScan() {
    // SELECT * FROM users
    // Expected: Project → SeqScan
    auto cat = buildCatalog();
    auto root = plan(cat, "SELECT * FROM users");

    const ProjectOp* proj = asProject(*root);
    CHECK(proj != nullptr, "simple: root is Project");
    const SeqScanOp* scan = asSeqScan(*proj->child);
    CHECK(scan != nullptr, "simple: child is SeqScan");
    CHECK(scan->table_name == "users", "simple: SeqScan table=users");
}

static void testSeqScanWithFilter() {
    // SELECT * FROM users WHERE id = 5
    // Expected: Project → Filter → SeqScan
    auto cat = buildCatalog();
    auto root = plan(cat, "SELECT * FROM users WHERE id = 5");

    const ProjectOp* proj = asProject(*root);
    CHECK(proj != nullptr, "filter: root is Project");
    const FilterOp* filt = asFilter(*proj->child);
    CHECK(filt != nullptr, "filter: next is Filter");
    const SeqScanOp* scan = asSeqScan(*filt->child);
    CHECK(scan != nullptr, "filter: leaf is SeqScan");
}

static void testIndexScan() {
    // SELECT * FROM users WHERE name = 'Sarthak'
    // users.name has an index, so planner should pick IndexScan
    // Expected: Project → Filter → IndexScan
    auto cat = buildCatalog();
    auto root = plan(cat, "SELECT * FROM users WHERE name = 'Sarthak'");

    const ProjectOp* proj = asProject(*root);
    CHECK(proj != nullptr, "index: root is Project");
    const FilterOp* filt = asFilter(*proj->child);
    CHECK(filt != nullptr, "index: next is Filter");
    const IndexScanOp* iscan = asIndexScan(*filt->child);
    CHECK(iscan != nullptr, "index: leaf is IndexScan");
    CHECK(iscan->table_name  == "users",          "index: table=users");
    CHECK(iscan->index_name  == "idx_users_name", "index: correct index");
}

static void testOrderBy() {
    // SELECT * FROM users ORDER BY name
    // Expected: Project → Sort → SeqScan
    auto cat = buildCatalog();
    auto root = plan(cat, "SELECT * FROM users ORDER BY name DESC");

    const ProjectOp* proj = asProject(*root);
    CHECK(proj != nullptr, "sort: root is Project");
    const SortOp* sort = asSort(*proj->child);
    CHECK(sort != nullptr, "sort: next is Sort");
    CHECK(sort->order_by.size() == 1,          "sort: 1 order item");
    CHECK(std::get<ColumnRef>(sort->order_by[0].expr.node).column == "name",  "sort: column=name");
    CHECK(sort->order_by[0].ascending == false,"sort: DESC");
    const SeqScanOp* scan = asSeqScan(*sort->child);
    CHECK(scan != nullptr, "sort: leaf is SeqScan");
}

static void testOrderByWithWhere() {
    // SELECT * FROM users WHERE id > 3 ORDER BY name
    // Expected: Project → Sort → Filter → SeqScan
    auto cat = buildCatalog();
    auto root = plan(cat, "SELECT * FROM users WHERE id > 3 ORDER BY name");

    const ProjectOp* proj = asProject(*root);
    const SortOp*    sort = asSort(*proj->child);
    const FilterOp*  filt = asFilter(*sort->child);
    const SeqScanOp* scan = asSeqScan(*filt->child);
    CHECK(sort != nullptr, "sort+filter: Sort present");
    CHECK(filt != nullptr, "sort+filter: Filter present");
    CHECK(scan != nullptr, "sort+filter: SeqScan present");
}

static void testJoin() {
    // SELECT * FROM users u JOIN orders o ON u.id = o.user_id
    // Expected: Project → NestedLoopJoin(SeqScan(users), SeqScan(orders))
    auto cat = buildCatalog();
    auto root = plan(cat,
        "SELECT * FROM users u JOIN orders o ON u.id = o.user_id");

    const ProjectOp*         proj = asProject(*root);
    CHECK(proj != nullptr, "join: root is Project");
    const NestedLoopJoinOp*  join = asJoin(*proj->child);
    CHECK(join != nullptr, "join: child is NestedLoopJoin");
    const SeqScanOp* left  = asSeqScan(*join->left);
    const SeqScanOp* right = asSeqScan(*join->right);
    CHECK(left  != nullptr && left->table_name  == "users",  "join: left=users");
    CHECK(right != nullptr && right->table_name == "orders", "join: right=orders");
}

static void testJoinWithFilter() {
    // SELECT * FROM users u JOIN orders o ON u.id = o.user_id WHERE o.total > 100
    // Expected: Project → Filter → NestedLoopJoin
    auto cat = buildCatalog();
    auto root = plan(cat,
        "SELECT * FROM users u JOIN orders o ON u.id = o.user_id WHERE o.total > 100");

    const ProjectOp*        proj = asProject(*root);
    const FilterOp*         filt = asFilter(*proj->child);
    const NestedLoopJoinOp* join = asJoin(*filt->child);
    CHECK(filt != nullptr, "join+filter: Filter present");
    CHECK(join != nullptr, "join+filter: Join under Filter");
}

static void testGroupBy() {
    // SELECT * FROM orders GROUP BY user_id
    // Expected: Project → Aggregate → SeqScan
    auto cat = buildCatalog();
    auto root = plan(cat, "SELECT * FROM orders GROUP BY user_id");

    const ProjectOp*   proj = asProject(*root);
    const AggregateOp* agg  = asAggregate(*proj->child);
    CHECK(agg  != nullptr, "group: Aggregate present");
    CHECK(agg->group_by.size() == 1,              "group: 1 group key");
    CHECK(agg->group_by[0] == "user_id",          "group: key=user_id");
    const SeqScanOp* scan = asSeqScan(*agg->child);
    CHECK(scan != nullptr, "group: leaf is SeqScan");
}

static void testProjectColumns() {
    auto cat = buildCatalog();
    auto root = plan(cat, "SELECT id, name FROM users");

    const ProjectOp* proj = asProject(*root);
    CHECK(proj != nullptr,            "project cols: root is Project");
    CHECK(proj->columns.size() == 2,  "project cols: 2 columns");
    CHECK(proj->columns[0] == "id",   "project cols: col 0 = id");
    CHECK(proj->columns[1] == "name", "project cols: col 1 = name");
}

static void testPrintTree() {
    auto cat = buildCatalog();
    auto root = plan(cat,
        "SELECT * FROM users WHERE id = 5 ORDER BY name");
    std::string tree = Planner::printTree(*root);
    CHECK(!tree.empty(), "printTree: non-empty output");
    // Tree should contain these operator names
    CHECK(tree.find("Project") != std::string::npos, "printTree: contains Project");
    CHECK(tree.find("Sort")    != std::string::npos, "printTree: contains Sort");
    CHECK(tree.find("Filter")  != std::string::npos, "printTree: contains Filter");
    CHECK(tree.find("SeqScan") != std::string::npos, "printTree: contains SeqScan");
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    testSimpleSeqScan();
    testSeqScanWithFilter();
    testIndexScan();
    testOrderBy();
    testOrderByWithWhere();
    testJoin();
    testJoinWithFilter();
    testGroupBy();
    testProjectColumns();
    testPrintTree();

    std::cout << "Planner: " << passed << " passed, " << failed << " failed.\n";
    return failed > 0 ? 1 : 0;
}
