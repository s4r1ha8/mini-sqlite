// ============================================================================
// main.cpp  —  Mini SQLite REPL
//
// Drives the full pipeline through DatabaseEngine:
//   SQL text
//     -> Tokenizer  -> vector<Token>
//     -> Parser     -> Statement (AST)
//     -> SemanticAnalyzer (validates + updates Catalog)
//     -> DatabaseEngine::executeParsed()
//        -> CREATE TABLE: BTree::create(), save catalog
//        -> CREATE INDEX: IndexManager::create(), backfill, save catalog
//        -> INSERT:       BTreeCursor::insert(), propagate root, save catalog
//        -> SELECT:       Planner -> ExecutorAdapter -> Operator* -> rows
//        -> BEGIN/COMMIT/ROLLBACK: TxnManager
//
// Special REPL commands:
//   .exit             — quit
//   .tables           — list all tables in the persisted catalog
//   .schema <table>   — show a table's column definitions and indexes
// ============================================================================

#include <iostream>
#include <string>

#include "engine/db_engine.h"
#include "frontend/sema.h"   // for SemanticError
#include "frontend/parser.h" // for ParseError

int main(int argc, char* argv[]) {
    // Allow passing a custom db path: ./mini_sqlite mydb.db
    std::string db_path = "mini_sqlite.db";
    if (argc >= 2) db_path = argv[1];

    std::cout << "╔══════════════════════════════════════════╗\n"
              << "║       mini-sqlite REPL  v1.0             ║\n"
              << "║  .exit | .tables | .schema <table>       ║\n"
              << "║  db: " << db_path;
    // pad to width 36
    int pad = 36 - static_cast<int>(db_path.size());
    for (int i = 0; i < pad; i++) std::cout << ' ';
    std::cout << "║\n"
              << "╚══════════════════════════════════════════╝\n\n";

    try {
        DatabaseEngine db(db_path);

        std::string line;
        while (true) {
            std::cout << "mini-sqlite> ";
            if (!std::getline(std::cin, line)) break;

            // Trim leading/trailing whitespace
            size_t s = line.find_first_not_of(" \t\r\n");
            size_t e = line.find_last_not_of(" \t\r\n");
            if (s == std::string::npos) continue;
            line = line.substr(s, e - s + 1);

            // ── Dot commands ──────────────────────────────────────────────
            if (line == ".exit") break;

            if (line == ".tables") {
                db.printTables();
                continue;
            }

            if (line.substr(0, 7) == ".schema") {
                std::string tbl = line.substr(7);
                size_t ts = tbl.find_first_not_of(" \t");
                if (ts == std::string::npos) {
                    std::cout << "Usage: .schema <table_name>\n";
                    continue;
                }
                db.printSchema(tbl.substr(ts));
                continue;
            }

            // ── SQL pipeline ──────────────────────────────────────────────
            try {
                db.execute(line);
            } catch (const ParseError& e) {
                std::cerr << "Parse error: " << e.what() << "\n";
            } catch (const SemanticError& e) {
                std::cerr << e.what() << "\n";
            } catch (const CatalogError& e) {
                std::cerr << "Catalog error: " << e.what() << "\n";
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }

    std::cout << "bye.\n";
    return 0;
}
