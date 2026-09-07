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
    Tokenizer tokenizer(sql);
    auto tokens = tokenizer.tokenize();

    Parser parser(std::move(tokens));
    return parser.parse();
}


int main() {
    const char* db_file =
        "integration_runtime_test.db";

    std::remove(db_file);


    // ========================================================================
    // 1. Create Pager + BTree
    // ========================================================================

    Pager pager(db_file);

    PageId root_page =
        BTree::create(pager);

    BTree tree(
        pager,
        root_page,
        3
    );


    // ========================================================================
    // 2. Insert test rows
    // ========================================================================

    tree.insert(
        Row{
            Value(int64_t(1)),
            Value(std::string("Alice")),
            Value(int64_t(10))
        }
    );

    tree.insert(
        Row{
            Value(int64_t(2)),
            Value(std::string("Bob")),
            Value(int64_t(20))
        }
    );

    tree.insert(
        Row{
            Value(int64_t(3)),
            Value(std::string("Carol")),
            Value(int64_t(30))
        }
    );

    pager.flush_all();


    // ========================================================================
    // 3. Create matching in-memory Catalog schema
    // ========================================================================

    Catalog catalog;

    TableSchema schema;

    schema.name =
        "users";

    schema.root_page_id =
        static_cast<int>(
            tree.root_page_id()
        );

    schema.primary_key =
        "id";


    ColumnInfo id;
    id.name = "id";
    id.type = DataType::INTEGER;
    id.ordinal = 0;
    id.is_not_null = true;


    ColumnInfo name;
    name.name = "name";
    name.type = DataType::TEXT;
    name.ordinal = 1;


    ColumnInfo score;
    score.name = "score";
    score.type = DataType::INTEGER;
    score.ordinal = 2;


    schema.columns.push_back(id);
    schema.columns.push_back(name);
    schema.columns.push_back(score);


    catalog.add_table(
        std::move(schema)
    );


    // ========================================================================
    // 4. Parse SQL
    // ========================================================================

    Statement statement =
        parseSql(
            "SELECT name "
            "FROM users "
            "WHERE score = 20"
        );


    // ========================================================================
    // 5. Semantic analysis
    // ========================================================================

    SemanticAnalyzer semantic_analyzer(
        catalog
    );

    semantic_analyzer.analyze(
        statement
    );


    // ========================================================================
    // 6. Extract validated SELECT statement
    // ========================================================================

    const SelectStmt& select =
        std::get<SelectStmt>(
            statement
        );


    // ========================================================================
    // 7. Build planner tree
    // ========================================================================

    Planner planner(
        catalog
    );

    std::unique_ptr<OperatorNode> plan =
        planner.plan(
            select
        );


    // ========================================================================
    // 8. Build runtime executor tree
    // ========================================================================

    ExecutorAdapter executor(
        pager,
        catalog
    );

    OperatorPtr root =
        executor.build(
            *plan
        );


    // ========================================================================
    // 9. Execute
    // ========================================================================

    root->open();


    std::vector<Row> results;

    while (auto row = root->next()) {
        results.push_back(
            std::move(*row)
        );
    }


    root->close();


    // ========================================================================
    // 10. Validate result
    //
    // Input:
    //   1 Alice 10
    //   2 Bob   20
    //   3 Carol 30
    //
    // Query:
    //   SELECT name FROM users WHERE score = 20
    //
    // Expected:
    //   Bob
    // ========================================================================

    assert(
        results.size() == 1
    );

    assert(
        results[0].size() == 1
    );

    assert(
        std::get<std::string>(
            results[0][0]
        ) == "Bob"
    );


    // ========================================================================
    // 11. Report success
    // ========================================================================

    std::cout
        << "Runtime integration test passed.\n";

    std::cout
        << "Result count: "
        << results.size()
        << "\n";

    std::cout
        << "Projected name: "
        << std::get<std::string>(
               results[0][0]
           )
        << "\n";


    pager.flush_all();

    std::remove(db_file);

    return 0;
}