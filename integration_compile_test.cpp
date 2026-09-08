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
#include "storage/index_manager.h"
#include "storage/pager.h"


// ============================================================================
// Helpers
// ============================================================================

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


// ============================================================================
// Main
// ============================================================================

int main() {

    const char* db_file =
        "integration_runtime_test.db";

    std::remove(db_file);


    // ========================================================================
    // 1. Create Pager + table B-tree
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
        2
    );


    // ========================================================================
    // 2. Insert users
    //
    // users(id, name)
    //
    // 1 Alice
    // 2 Bob
    // 3 Carol
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
    // 3. Create secondary index on users.name
    //
    // index:
    //     name -> primary key
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


    // Register secondary index in the catalog.
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
    // 6. Parse SQL
    //
    // The planner should recognize:
    //
    //     name = 'Bob'
    //
    // as an indexed equality and choose IndexScan.
    // ========================================================================

    const std::string sql =
        "SELECT * "
        "FROM users "
        "WHERE name = 'Bob'";


    std::cout
        << "[6] SQL:\n"
        << "     "
        << sql
        << "\n";


    std::cout
        << "[6] Before parseSql...\n";


    Statement statement =
        parseSql(sql);


    std::cout
        << "[6] After parseSql.\n";


    // ========================================================================
    // 7. Semantic analysis
    // ========================================================================

    std::cout
        << "[7] Before semantic analysis...\n";


    SemanticAnalyzer semantic_analyzer(
        catalog
    );


    semantic_analyzer.analyze(
        statement
    );


    std::cout
        << "[7] After semantic analysis.\n";


    const SelectStmt& select =
        std::get<SelectStmt>(
            statement
        );


    // ========================================================================
    // 8. Planner
    // ========================================================================

    std::cout
        << "[8] Before planning...\n";


    Planner planner(
        catalog
    );


    std::unique_ptr<OperatorNode> plan =
        planner.plan(
            select
        );


    std::cout
        << "[8] After planning.\n";


    std::cout
        << "[8] Plan:\n"
        << Planner::printTree(*plan)
        << "\n";


    // ========================================================================
    // 9. Verify that Planner selected IndexScan
    // ========================================================================

    bool has_index_scan =
        false;


    std::visit(
        [&](const auto& op) {

            using T =
                std::decay_t<
                    decltype(op)
                >;

            if constexpr (
                std::is_same_v<
                    T,
                    ProjectOp
                >
            ) {

                if (!op.child) {
                    return;
                }


                std::visit(
                    [&](const auto& child_op) {

                        using ChildT =
                            std::decay_t<
                                decltype(child_op)
                            >;

                        if constexpr (
                            std::is_same_v<
                                ChildT,
                                IndexScanOp
                            >
                        ) {
                            has_index_scan = true;
                        }

                        else if constexpr (
                            std::is_same_v<
                                ChildT,
                                FilterOp
                            >
                        ) {

                            if (!child_op.child) {
                                return;
                            }

                            std::visit(
                                [&](const auto& grandchild_op) {

                                    using GrandchildT =
                                        std::decay_t<
                                            decltype(grandchild_op)
                                        >;

                                    if constexpr (
                                        std::is_same_v<
                                            GrandchildT,
                                            IndexScanOp
                                        >
                                    ) {
                                        has_index_scan = true;
                                    }

                                },
                                child_op.child->v
                            );
                        }

                    },
                    op.child->v
                );
            }

        },
        plan->v
    );


    assert(
        has_index_scan
    );


    std::cout
        << "[9] Planner selected IndexScan.\n";


    // ========================================================================
    // 10. Executor construction
    // ========================================================================

    std::cout
        << "[10] Before ExecutorAdapter construction...\n";


    ExecutorAdapter executor(
        pager,
        catalog
    );


    std::cout
        << "[10] Before executor.build...\n";


    OperatorPtr root =
        executor.build(
            *plan
        );


    std::cout
        << "[10] After executor.build.\n";


    // ========================================================================
    // 11. Execute
    // ========================================================================

    std::cout
        << "[11] Before root->open()...\n";


    root->open();


    std::cout
        << "[11] After root->open().\n";


    std::vector<Row> results;


    std::cout
        << "[11] Before next() loop...\n";


    size_t iteration = 0;


    while (auto row = root->next()) {

        ++iteration;


        std::cout
            << "[11] next() returned row "
            << iteration
            << " with "
            << row->size()
            << " columns.\n";


        results.push_back(
            std::move(*row)
        );


        if (iteration > 100) {

            std::cerr
                << "[11] ERROR: more than 100 rows returned.\n";

            root->close();

            pager.flush_all();

            std::remove(db_file);

            return 2;
        }
    }


    std::cout
        << "[11] next() returned nullopt.\n";


    root->close();


    std::cout
        << "[11] Executor closed.\n";


    // ========================================================================
    // 12. Validate result
    //
    // Expected:
    //
    //     2 | Bob
    // ========================================================================

    std::cout
        << "[12] Validating results...\n";


    assert(
        results.size() == 1
    );


    assert(
        results[0].size() == 2
    );


    assert(
        std::get<int64_t>(
            results[0][0]
        ) == 2
    );


    assert(
        std::get<std::string>(
            results[0][1]
        ) == "Bob"
    );


    std::cout
        << "[12] Result is correct.\n";


    std::cout
        << "IndexScan runtime integration test passed.\n";


    std::cout
        << "Result count: "
        << results.size()
        << "\n";


    std::cout
        << "Row: "
        << std::get<int64_t>(
               results[0][0]
           )
        << " "
        << std::get<std::string>(
               results[0][1]
           )
        << "\n";


    // ========================================================================
    // 13. Cleanup
    // ========================================================================

    pager.flush_all();

    std::remove(db_file);

    return 0;
}