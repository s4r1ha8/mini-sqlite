#include <cassert>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "common/ast.h"
#include "common/cursor_interface.h"

#include "frontend/catalog.h"
#include "frontend/parser.h"
#include "frontend/planner.h"
#include "frontend/sema.h"
#include "frontend/tokenizer.h"

#include "exec/executor_adapter.h"

#include "storage/btree.h"
#include "storage/pager.h"


static Statement parseSql(const std::string& sql) {
    std::cout << "[parse] tokenizing...\n";

    Tokenizer tokenizer(sql);
    auto tokens = tokenizer.tokenize();

    std::cout
        << "[parse] token count: "
        << tokens.size()
        << "\n";

    std::cout << "[parse] parsing...\n";

    Parser parser(std::move(tokens));
    return parser.parse();
}


static void addColumn(
    TableSchema& schema,
    const std::string& name,
    DataType type,
    int ordinal,
    bool not_null = false
) {
    ColumnInfo column;

    column.name = name;
    column.type = type;
    column.ordinal = ordinal;
    column.is_not_null = not_null;

    schema.columns.push_back(column);
}


int main() {

    const char* db_file =
        "integration_runtime_test.db";

    std::remove(db_file);


    // ========================================================================
    // 1. Create database + B-trees
    // ========================================================================

    std::cout
        << "[1] Creating Pager...\n";

    Pager pager(db_file);

    std::cout
        << "[1] Creating users B-tree...\n";

    PageId users_root =
        BTree::create(pager);

    std::cout
        << "[1] users root = "
        << users_root
        << "\n";

    std::cout
        << "[1] Creating orders B-tree...\n";

    PageId orders_root =
        BTree::create(pager);

    std::cout
        << "[1] orders root = "
        << orders_root
        << "\n";


    BTree users_tree(
        pager,
        users_root,
        2
    );

    BTree orders_tree(
        pager,
        orders_root,
        3
    );


    // ========================================================================
    // 2. Populate users
    // ========================================================================

    std::cout
        << "[2] Inserting users...\n";

    users_tree.insert(
        Row{
            Value(int64_t(1)),
            Value(std::string("Alice"))
        }
    );

    users_tree.insert(
        Row{
            Value(int64_t(2)),
            Value(std::string("Bob"))
        }
    );

    users_tree.insert(
        Row{
            Value(int64_t(3)),
            Value(std::string("Carol"))
        }
    );

    std::cout
        << "[2] Users inserted.\n";


    // ========================================================================
    // 3. Populate orders
    // ========================================================================

    std::cout
        << "[3] Inserting orders...\n";

    orders_tree.insert(
        Row{
            Value(int64_t(101)),
            Value(int64_t(1)),
            Value(int64_t(100))
        }
    );

    orders_tree.insert(
        Row{
            Value(int64_t(102)),
            Value(int64_t(2)),
            Value(int64_t(200))
        }
    );

    orders_tree.insert(
        Row{
            Value(int64_t(103)),
            Value(int64_t(2)),
            Value(int64_t(300))
        }
    );

    orders_tree.insert(
        Row{
            Value(int64_t(104)),
            Value(int64_t(3)),
            Value(int64_t(150))
        }
    );

    std::cout
        << "[3] Orders inserted.\n";


    std::cout
        << "[3] Flushing pager...\n";

    pager.flush_all();

    std::cout
        << "[3] Pager flushed.\n";


    // ========================================================================
    // 4. Build Catalog
    // ========================================================================

    std::cout
        << "[4] Building catalog...\n";

    Catalog catalog;


    TableSchema users_schema;

    users_schema.name =
        "users";

    users_schema.primary_key =
        "id";

    users_schema.root_page_id =
        static_cast<int>(
            users_tree.root_page_id()
        );


    addColumn(
        users_schema,
        "id",
        DataType::INTEGER,
        0,
        true
    );

    addColumn(
        users_schema,
        "name",
        DataType::TEXT,
        1
    );


    catalog.add_table(
        std::move(users_schema)
    );


    TableSchema orders_schema;

    orders_schema.name =
        "orders";

    orders_schema.primary_key =
        "id";

    orders_schema.root_page_id =
        static_cast<int>(
            orders_tree.root_page_id()
        );


    addColumn(
        orders_schema,
        "id",
        DataType::INTEGER,
        0,
        true
    );

    addColumn(
        orders_schema,
        "user_id",
        DataType::INTEGER,
        1
    );

    addColumn(
        orders_schema,
        "total",
        DataType::INTEGER,
        2
    );


    catalog.add_table(
        std::move(orders_schema)
    );


    std::cout
        << "[4] Catalog ready.\n";


    // ========================================================================
    // 5. Parse SQL
    // ========================================================================

    std::cout
        << "[5] Before parseSql...\n";

    Statement statement =
        parseSql(
            "SELECT u.name, o.total "
            "FROM users u "
            "JOIN orders o "
            "ON u.id = o.user_id"
        );

    std::cout
        << "[5] After parseSql.\n";


    // ========================================================================
    // 6. Semantic analysis
    // ========================================================================

    std::cout
        << "[6] Before semantic analysis...\n";

    SemanticAnalyzer semantic_analyzer(
        catalog
    );

    semantic_analyzer.analyze(
        statement
    );

    std::cout
        << "[6] After semantic analysis.\n";


    const SelectStmt& select =
        std::get<SelectStmt>(
            statement
        );


    // ========================================================================
    // 7. Planner
    // ========================================================================

    std::cout
        << "[7] Before planning...\n";

    Planner planner(
        catalog
    );

    std::unique_ptr<OperatorNode> plan =
        planner.plan(
            select
        );

    std::cout
        << "[7] After planning.\n";

    std::cout
        << "[7] Plan:\n"
        << Planner::printTree(*plan)
        << "\n";


    // ========================================================================
    // 8. Executor
    // ========================================================================

    std::cout
        << "[8] Before ExecutorAdapter construction...\n";

    ExecutorAdapter executor(
        pager,
        catalog
    );

    std::cout
        << "[8] Before executor.build...\n";

    OperatorPtr root =
        executor.build(
            *plan
        );

    std::cout
        << "[8] After executor.build.\n";


    // ========================================================================
    // 9. Execute
    // ========================================================================

    std::cout
        << "[9] Before root->open()...\n";

    root->open();

    std::cout
        << "[9] After root->open().\n";


    std::vector<Row> results;


    std::cout
        << "[9] Before next() loop...\n";

    size_t iteration = 0;

    while (auto row = root->next()) {

        ++iteration;

        std::cout
            << "[9] next() returned row "
            << iteration
            << " with "
            << row->size()
            << " columns.\n";

        results.push_back(
            std::move(*row)
        );

        // Safety limit so a broken iterator cannot loop forever.
        if (iteration > 100) {
            std::cerr
                << "[9] ERROR: more than 100 rows returned.\n";
            std::remove(db_file);
            return 2;
        }
    }

    std::cout
        << "[9] next() returned nullopt.\n";


    std::cout
        << "[9] Before root->close()...\n";

    root->close();

    std::cout
        << "[9] After root->close().\n";


    // ========================================================================
    // 10. Validate JOIN output
    // ========================================================================

    std::cout
        << "[10] Validating results...\n";

    assert(
        results.size() == 4
    );

    for (const auto& row : results) {
        assert(
            row.size() == 2
        );
    }


    // Row 0: Alice, 100
    assert(
        std::get<std::string>(
            results[0][0]
        ) == "Alice"
    );

    assert(
        std::get<int64_t>(
            results[0][1]
        ) == 100
    );


    // Row 1: Bob, 200
    assert(
        std::get<std::string>(
            results[1][0]
        ) == "Bob"
    );

    assert(
        std::get<int64_t>(
            results[1][1]
        ) == 200
    );


    // Row 2: Bob, 300
    assert(
        std::get<std::string>(
            results[2][0]
        ) == "Bob"
    );

    assert(
        std::get<int64_t>(
            results[2][1]
        ) == 300
    );


    // Row 3: Carol, 150
    assert(
        std::get<std::string>(
            results[3][0]
        ) == "Carol"
    );

    assert(
        std::get<int64_t>(
            results[3][1]
        ) == 150
    );


    // ========================================================================
    // 11. Success
    // ========================================================================

    std::cout
        << "JOIN runtime integration test passed.\n";

    std::cout
        << "Result count: "
        << results.size()
        << "\n";

    for (const auto& row : results) {

        std::cout
            << std::get<std::string>(
                   row[0]
               )
            << " "
            << std::get<int64_t>(
                   row[1]
               )
            << "\n";
    }


    pager.flush_all();

    std::remove(db_file);

    return 0;
}