#include <cassert>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

#include "common/ast.h"
#include "common/cursor_interface.h"

#include "frontend/catalog.h"
#include "frontend/parser.h"
#include "frontend/planner.h"
#include "frontend/sema.h"
#include "frontend/tokenizer.h"

#include "exec/executor_adapter.h"

#include "storage/btree.h"
#include "storage/index_manager.h"
#include "storage/pager.h"

// ============================================================================
// Helpers
// ============================================================================

static Statement parseSql(const std::string& sql) {
    std::cout << "[parse] SQL: " << sql << "\n";

    Tokenizer tokenizer(sql);
    auto tokens = tokenizer.tokenize();

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

static int64_t getInt(const Value& value) {
    return std::get<int64_t>(value);
}

static double getDouble(const Value& value) {
    return std::get<double>(value);
}

static std::string getString(const Value& value) {
    return std::get<std::string>(value);
}

// ============================================================================
// Execute helper
// ============================================================================

static std::vector<Row> executeSql(
    const std::string& sql,
    Pager& pager,
    Catalog& catalog
) {
    std::cout << "\n============================================================\n";
    std::cout << "Executing: " << sql << "\n";
    std::cout << "============================================================\n";

    Statement statement = parseSql(sql);

    SemanticAnalyzer semantic_analyzer(catalog);
    semantic_analyzer.analyze(statement);

    const SelectStmt& select =
        std::get<SelectStmt>(statement);

    Planner planner(catalog);

    std::unique_ptr<OperatorNode> plan =
        planner.plan(select);

    std::cout << "Plan:\n"
              << Planner::printTree(*plan)
              << "\n";

    ExecutorAdapter executor(pager, catalog);

    OperatorPtr root =
        executor.build(*plan);

    root->open();

    std::vector<Row> results;

    while (auto row = root->next()) {
        results.push_back(std::move(*row));

        if (results.size() > 1000) {
            root->close();
            throw std::runtime_error(
                "Integration test returned more than 1000 rows."
            );
        }
    }

    root->close();

    return results;
}

// ============================================================================
// Main
// ============================================================================

int main() {

    const char* db_file =
        "integration_runtime_test.db";

    std::remove(db_file);

    // ========================================================================
    // 1. Create Pager + users B-tree
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

    BTree users_tree(
        pager,
        users_root,
        3
    );

    // ========================================================================
    // 2. Insert users
    //
    // users(id, name, score)
    //
    // 1 Alice 100
    // 2 Bob   200
    // 3 Carol 150
    // ========================================================================

    std::cout
        << "[2] Inserting users...\n";

    users_tree.insert(
        Row{
            Value(int64_t(1)),
            Value(std::string("Alice")),
            Value(double(100.0))
        }
    );

    users_tree.insert(
        Row{
            Value(int64_t(2)),
            Value(std::string("Bob")),
            Value(double(200.0))
        }
    );

    users_tree.insert(
        Row{
            Value(int64_t(3)),
            Value(std::string("Carol")),
            Value(double(150.0))
        }
    );

    std::cout
        << "[2] Users inserted.\n";

    // ========================================================================
    // 3. Create secondary index on users.name
    //
    // Alice -> 1
    // Bob   -> 2
    // Carol -> 3
    // ========================================================================

    std::cout
        << "[3] Creating users.name index...\n";

    PageId users_index_root =
        IndexManager::create(pager);

    users_index_root =
        IndexManager::insert_entry(
            pager,
            users_index_root,
            Value(std::string("Alice")),
            1
        );

    users_index_root =
        IndexManager::insert_entry(
            pager,
            users_index_root,
            Value(std::string("Bob")),
            2
        );

    users_index_root =
        IndexManager::insert_entry(
            pager,
            users_index_root,
            Value(std::string("Carol")),
            3
        );

    std::cout
        << "[3] users.name index root = "
        << users_index_root
        << "\n";

    // ========================================================================
    // 4. Flush storage
    // ========================================================================

    std::cout
        << "[4] Flushing Pager...\n";

    pager.flush_all();

    std::cout
        << "[4] Pager flushed.\n";

    // ========================================================================
    // 5. Build Catalog
    // ========================================================================

    std::cout
        << "[5] Building Catalog...\n";

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

    addColumn(
        users_schema,
        "score",
        DataType::REAL,
        2
    );

    users_schema.indexes.push_back(
        IndexInfo{
            "idx_users_name",
            "name",
            static_cast<int>(
                users_index_root
            )
        }
    );

    catalog.add_table(
        std::move(users_schema)
    );

    std::cout
        << "[5] Catalog ready.\n";

    // ========================================================================
    // 6. IndexScan runtime integration
    // ========================================================================

    {
        const std::string sql =
            "SELECT * "
            "FROM users "
            "WHERE name = 'Bob'";

        std::vector<Row> results =
            executeSql(
                sql,
                pager,
                catalog
            );

        assert(results.size() == 1);
        assert(results[0].size() == 3);

        assert(
            getInt(results[0][0]) == 2
        );

        assert(
            getString(results[0][1]) == "Bob"
        );

        assert(
            getDouble(results[0][2]) == 200.0
        );

        std::cout
            << "IndexScan runtime integration test passed.\n";
    }

    // ========================================================================
    // 7. COUNT(*) runtime integration
    //
    // SELECT COUNT(*) FROM users
    //
    // Expected:
    //     3
    // ========================================================================

    {
        const std::string sql =
            "SELECT COUNT(*) "
            "FROM users";

        std::vector<Row> results =
            executeSql(
                sql,
                pager,
                catalog
            );

        assert(results.size() == 1);
        assert(results[0].size() == 1);

        assert(
            getInt(results[0][0]) == 3
        );

        std::cout
            << "COUNT(*) runtime integration test passed.\n";
    }

    // ========================================================================
    // 8. COUNT(column) runtime integration
    //
    // SELECT COUNT(id) FROM users
    //
    // Expected:
    //     3
    // ========================================================================

    {
        const std::string sql =
            "SELECT COUNT(id) "
            "FROM users";

        std::vector<Row> results =
            executeSql(
                sql,
                pager,
                catalog
            );

        assert(results.size() == 1);
        assert(results[0].size() == 1);

        assert(
            getInt(results[0][0]) == 3
        );

        std::cout
            << "COUNT(id) runtime integration test passed.\n";
    }

    // ========================================================================
    // 9. SUM / AVG / MIN / MAX runtime integration
    //
    // SELECT SUM(score), AVG(score), MIN(score), MAX(score) FROM users
    //
    // Expected:
    //     SUM = 450
    //     AVG = 150
    //     MIN = 100
    //     MAX = 200
    // ========================================================================

    {
        const std::string sql =
            "SELECT SUM(score), AVG(score), MIN(score), MAX(score) "
            "FROM users";

        std::vector<Row> results =
            executeSql(
                sql,
                pager,
                catalog
            );

        assert(results.size() == 1);
        assert(results[0].size() == 4);

        assert(
            getDouble(results[0][0]) == 450.0
        );

        assert(
            getDouble(results[0][1]) == 150.0
        );

        assert(
            getDouble(results[0][2]) == 100.0
        );

        assert(
            getDouble(results[0][3]) == 200.0
        );

        std::cout
            << "SUM/AVG/MIN/MAX runtime integration test passed.\n";
    }

    // ========================================================================
    // 10. GROUP BY + COUNT(*)
    //
    // Use name as the grouping column so every row is its own group.
    //
    // Expected three rows:
    //     Alice | 1
    //     Bob   | 1
    //     Carol | 1
    //
    // unordered_map means output order is not guaranteed.
    // ========================================================================

    {
        const std::string sql =
            "SELECT name, COUNT(*) "
            "FROM users "
            "GROUP BY name";

        std::vector<Row> results =
            executeSql(
                sql,
                pager,
                catalog
            );

        assert(results.size() == 3);

        std::vector<std::string> names;

        for (const Row& row : results) {
            assert(row.size() == 2);
            assert(getInt(row[1]) == 1);
            names.push_back(getString(row[0]));
        }

        std::sort(names.begin(), names.end());

        assert(names.size() == 3);
        assert(names[0] == "Alice");
        assert(names[1] == "Bob");
        assert(names[2] == "Carol");

        std::cout
            << "GROUP BY + COUNT(*) runtime integration test passed.\n";
    }



    // ========================================================================
    // 11. GROUP BY + COUNT(*) + HAVING
    //
    // orders(id, user_id, total)
    //
    // 1 | 1 | 10
    // 2 | 1 | 20
    // 3 | 2 |  5
    //
    // Expected:
    //     user_id = 1 | COUNT(*) = 2
    // ========================================================================

    {
        PageId orders_root = BTree::create(pager);

        BTree orders_tree(
            pager,
            orders_root,
            3
        );

        orders_tree.insert(
            Row{
                Value(int64_t(1)),
                Value(int64_t(1)),
                Value(double(10.0))
            }
        );

        orders_tree.insert(
            Row{
                Value(int64_t(2)),
                Value(int64_t(1)),
                Value(double(20.0))
            }
        );

        orders_tree.insert(
            Row{
                Value(int64_t(3)),
                Value(int64_t(2)),
                Value(double(5.0))
            }
        );

        TableSchema orders_schema;

        orders_schema.name = "orders";
        orders_schema.primary_key = "id";
        orders_schema.root_page_id =
            static_cast<int>(orders_tree.root_page_id());

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
            DataType::REAL,
            2
        );

        catalog.add_table(
            std::move(orders_schema)
        );

        pager.flush_all();

        const std::string sql =
            "SELECT user_id, COUNT(*) "
            "FROM orders "
            "GROUP BY user_id "
            "HAVING COUNT(*) > 1";

        std::vector<Row> results =
            executeSql(
                sql,
                pager,
                catalog
            );

        assert(results.size() == 1);
        assert(results[0].size() == 2);

        assert(getInt(results[0][0]) == 1);
        assert(getInt(results[0][1]) == 2);

        std::cout
            << "GROUP BY + HAVING runtime integration test passed.\n";
    }


    // ========================================================================
    // 12. Cleanup
    // ========================================================================

    pager.flush_all();

    std::remove(db_file);

    std::cout
        << "\nAll runtime integration tests passed.\n";

    return 0;
}