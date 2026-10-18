#include "../src/engine/db_engine.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

void copy_file(const std::string& src, const std::string& dst) {
    std::ifstream  source(src, std::ios::binary);
    std::ofstream  dest(dst, std::ios::binary);
    dest << source.rdbuf();
}

void simulate_crash_after_wal(const std::string& db_path) {
    // We simulate a crash where WAL is durable but DB is not,
    // by restoring the DB file to a backup made before the transaction.
    // The WAL file (.wal) is left intact.
    std::string db_bak = db_path + ".bak";
    copy_file(db_bak, db_path);
}

void backup_db(const std::string& db_path) {
    std::string db_bak = db_path + ".bak";
    copy_file(db_path, db_bak);
}

void clear_files(const std::string& db_path) {
    fs::remove(db_path);
    fs::remove(db_path + ".wal");
    fs::remove(db_path + ".bak");
}

void assert_select(DatabaseEngine& db, const std::string& query, const std::string& expected_output_contains) {
    std::stringstream buffer;
    std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
    
    db.execute(query);
    
    std::cout.rdbuf(old);
    std::string output = buffer.str();
    if (output.find(expected_output_contains) == std::string::npos) {
        std::cerr << "Expected '" << expected_output_contains << "' in output, but got:\n" << output << "\n";
        std::exit(1);
    }
}

void test_insert_recovery() {
    std::string db_name = "test_rec1.db";
    clear_files(db_name);
    {
        DatabaseEngine db(db_name);
        db.execute("CREATE TABLE users (id INTEGER, name TEXT);");
        backup_db(db_name);
        db.execute("INSERT INTO users VALUES (1, 'Alice');");
    }
    simulate_crash_after_wal(db_name);
    {
        DatabaseEngine db(db_name);
        assert_select(db, "SELECT * FROM users WHERE id = 1;", "Alice");
        db.execute("INSERT INTO users VALUES (2, 'Bob');");
        assert_select(db, "SELECT * FROM users WHERE id = 2;", "Bob");
    }
    std::cout << "PASS: test_insert_recovery\n";
}

void test_update_recovery() {
    std::string db_name = "test_rec2.db";
    clear_files(db_name);
    {
        DatabaseEngine db(db_name);
        db.execute("CREATE TABLE users (id INTEGER, name TEXT);");
        db.execute("INSERT INTO users VALUES (1, 'Alice');");
        backup_db(db_name);
        db.execute("UPDATE users SET name = 'AliceUpdated' WHERE id = 1;");
    }
    simulate_crash_after_wal(db_name);
    {
        DatabaseEngine db(db_name);
        assert_select(db, "SELECT * FROM users WHERE id = 1;", "AliceUpdated");
    }
    std::cout << "PASS: test_update_recovery\n";
}

void test_delete_recovery() {
    std::string db_name = "test_rec3.db";
    clear_files(db_name);
    {
        DatabaseEngine db(db_name);
        db.execute("CREATE TABLE users (id INTEGER, name TEXT);");
        db.execute("INSERT INTO users VALUES (1, 'Alice');");
        backup_db(db_name);
        db.execute("DELETE FROM users WHERE id = 1;");
    }
    simulate_crash_after_wal(db_name);
    {
        DatabaseEngine db(db_name);
        // Expecting 0 rows
        std::stringstream buffer;
        std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
        db.execute("SELECT * FROM users WHERE id = 1;");
        std::cout.rdbuf(old);
        std::string output = buffer.str();
        assert(output.find("Alice") == std::string::npos);
        
        db.execute("INSERT INTO users VALUES (1, 'AliceBack');");
        assert_select(db, "SELECT * FROM users WHERE id = 1;", "AliceBack");
    }
    std::cout << "PASS: test_delete_recovery\n";
}

void test_uncommitted_update_undo() {
    std::string db_name = "test_rec4.db";
    clear_files(db_name);
    {
        DatabaseEngine db(db_name);
        db.execute("CREATE TABLE users (id INTEGER, name TEXT);");
        db.execute("INSERT INTO users VALUES (1, 'Alice');");
        db.execute("BEGIN;");
        db.execute("UPDATE users SET name = 'AliceUpdated' WHERE id = 1;");
        // No COMMIT
    }
    {
        DatabaseEngine db(db_name);
        assert_select(db, "SELECT * FROM users WHERE id = 1;", "Alice");
    }
    std::cout << "PASS: test_uncommitted_update_undo\n";
}

void test_multi_statement_transaction() {
    std::string db_name = "test_rec5.db";
    clear_files(db_name);
    {
        DatabaseEngine db(db_name);
        db.execute("CREATE TABLE users (id INTEGER, name TEXT);");
        backup_db(db_name);
        db.execute("BEGIN;");
        db.execute("INSERT INTO users VALUES (1, 'A');");
        db.execute("INSERT INTO users VALUES (2, 'B');");
        db.execute("INSERT INTO users VALUES (3, 'C');");
        db.execute("COMMIT;");
    }
    simulate_crash_after_wal(db_name);
    {
        DatabaseEngine db(db_name);
        assert_select(db, "SELECT * FROM users WHERE id = 1;", "A");
        assert_select(db, "SELECT * FROM users WHERE id = 2;", "B");
        assert_select(db, "SELECT * FROM users WHERE id = 3;", "C");
    }
    std::cout << "PASS: test_multi_statement_transaction\n";
}

void test_indexed_delete_recovery() {
    std::string db_name = "test_rec6.db";
    clear_files(db_name);
    {
        DatabaseEngine db(db_name);
        db.execute("CREATE TABLE users (id INTEGER, name TEXT);");
        db.execute("CREATE INDEX idx_name ON users (name);");
        db.execute("INSERT INTO users VALUES (1, 'Alice');");
        db.execute("INSERT INTO users VALUES (2, 'Bob');");
        backup_db(db_name);
        db.execute("DELETE FROM users WHERE name = 'Alice';");
    }
    simulate_crash_after_wal(db_name);
    {
        DatabaseEngine db(db_name);
        // Expecting 0 rows
        std::stringstream buffer;
        std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
        db.execute("SELECT * FROM users WHERE name = 'Alice';");
        std::cout.rdbuf(old);
        std::string output = buffer.str();
        assert(output.find("Alice") == std::string::npos);
        assert_select(db, "SELECT * FROM users WHERE id = 2;", "Bob");
    }
    std::cout << "PASS: test_indexed_delete_recovery\n";
}

void test_btree_root_split_recovery() {
    std::string db_name = "test_rec7.db";
    clear_files(db_name);
    {
        DatabaseEngine db(db_name);
        db.execute("CREATE TABLE users (id INTEGER, name TEXT);");
        backup_db(db_name);
        db.execute("BEGIN;");
        for (int i = 0; i < 400; i++) {
            db.execute("INSERT INTO users VALUES (" + std::to_string(i) + ", 'LongName" + std::to_string(i) + "');");
        }
        db.execute("COMMIT;");
    }
    simulate_crash_after_wal(db_name);
    {
        DatabaseEngine db(db_name);
        assert_select(db, "SELECT * FROM users WHERE id = 50;", "LongName50");
        assert_select(db, "SELECT * FROM users WHERE id = 250;", "LongName250");
        assert_select(db, "SELECT * FROM users WHERE id = 399;", "LongName399");
        // Insert enough new rows to force ANOTHER split
        for (int i = 400; i < 800; i++) {
            db.execute("INSERT INTO users VALUES (" + std::to_string(i) + ", 'LongName" + std::to_string(i) + "');");
        }
        assert_select(db, "SELECT * FROM users WHERE id = 750;", "LongName750");

        // Update
        db.execute("UPDATE users SET name = 'UpdatedName' WHERE id = 50;");
        assert_select(db, "SELECT * FROM users WHERE id = 50;", "UpdatedName");

        // Delete
        db.execute("DELETE FROM users WHERE id = 250;");
    }
    // Reopen and verify final state
    {
        DatabaseEngine db(db_name);
        assert_select(db, "SELECT * FROM users WHERE id = 50;", "UpdatedName");
        assert_select(db, "SELECT * FROM users WHERE id = 750;", "LongName750");
        
        std::stringstream buffer;
        std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
        db.execute("SELECT * FROM users WHERE id = 250;");
        std::cout.rdbuf(old);
        std::string output = buffer.str();
        if (output.find("LongName250") != std::string::npos) std::exit(1);
    }
    std::cout << "PASS: test_btree_root_split_recovery\n";
}

void test_indexed_split_recovery() {
    std::string db_name = "test_rec8.db";
    clear_files(db_name);
    {
        DatabaseEngine db(db_name);
        db.execute("CREATE TABLE users (id INTEGER, name TEXT);");
        db.execute("CREATE INDEX idx_name ON users (name);");
        backup_db(db_name);
        db.execute("BEGIN;");
        for (int i = 0; i < 400; i++) {
            db.execute("INSERT INTO users VALUES (" + std::to_string(i) + ", 'LongName" + std::to_string(i) + "');");
        }
        db.execute("COMMIT;");
    }
    simulate_crash_after_wal(db_name);
    {
        DatabaseEngine db(db_name);
        assert_select(db, "SELECT * FROM users WHERE name = 'LongName50';", "50");
        assert_select(db, "SELECT * FROM users WHERE name = 'LongName399';", "399");

        // Force another index split
        for (int i = 400; i < 800; i++) {
            db.execute("INSERT INTO users VALUES (" + std::to_string(i) + ", 'LongName" + std::to_string(i) + "');");
        }
        assert_select(db, "SELECT * FROM users WHERE name = 'LongName750';", "750");

        // Indexed UPDATE
        db.execute("UPDATE users SET name = 'UpdatedName' WHERE name = 'LongName50';");
        assert_select(db, "SELECT * FROM users WHERE name = 'UpdatedName';", "50");

        // Indexed DELETE
        db.execute("DELETE FROM users WHERE name = 'LongName250';");
    }
    {
        DatabaseEngine db(db_name);
        assert_select(db, "SELECT * FROM users WHERE name = 'UpdatedName';", "50");
        assert_select(db, "SELECT * FROM users WHERE name = 'LongName750';", "750");
        
        std::stringstream buffer;
        std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
        db.execute("SELECT * FROM users WHERE name = 'LongName250';");
        std::cout.rdbuf(old);
        std::string output = buffer.str();
        if (output.find("250") != std::string::npos) std::exit(1);
    }
    std::cout << "PASS: test_indexed_split_recovery\n";
}

void test_indexed_update_crash_test() {
    std::string db_name = "test_rec9.db";
    clear_files(db_name);
    {
        DatabaseEngine db(db_name);
        db.execute("CREATE TABLE users (id INTEGER, name TEXT);");
        db.execute("CREATE INDEX idx_name ON users (name);");
        backup_db(db_name);
        db.execute("BEGIN;");
        db.execute("INSERT INTO users VALUES (1, 'Duplicate');");
        db.execute("INSERT INTO users VALUES (2, 'Duplicate');");
        db.execute("INSERT INTO users VALUES (3, 'Duplicate');");
        db.execute("COMMIT;");
        backup_db(db_name); // Checkpoint
        
        db.execute("BEGIN;");
        db.execute("UPDATE users SET name = 'Unique' WHERE id = 2;");
        db.execute("COMMIT;");
    }
    simulate_crash_after_wal(db_name);
    {
        DatabaseEngine db(db_name);
        
        std::stringstream buffer;
        std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
        db.execute("SELECT * FROM users WHERE name = 'Duplicate';");
        std::string output = buffer.str();
        if (output.find("2 | Duplicate") != std::string::npos) {
            std::cerr << "Found stale row!\n";
            std::exit(1);
        }
        std::cout.rdbuf(old);

        assert_select(db, "SELECT * FROM users WHERE name = 'Unique';", "2");
        
        // Another UPDATE/DELETE
        db.execute("UPDATE users SET name = 'Unique2' WHERE id = 1;");
        db.execute("DELETE FROM users WHERE id = 3;");
    }
    {
        DatabaseEngine db(db_name);
        assert_select(db, "SELECT * FROM users WHERE name = 'Unique2';", "1");
        
        std::stringstream buffer;
        std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
        db.execute("SELECT * FROM users WHERE name = 'Duplicate';");
        std::cout.rdbuf(old);
        std::string output = buffer.str();
        if (output.find("3") != std::string::npos) std::exit(1);
    }
    std::cout << "PASS: test_indexed_update_crash_test\n";
}

void setup_reproduction_dataset(DatabaseEngine& db) {
    db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, payload TEXT);");
    db.execute("CREATE INDEX idx_payload ON t(payload);");

    for (int i = 1; i <= 16; ++i) {
        db.execute("INSERT INTO t VALUES (" + std::to_string(i) + ", 'short_" + std::to_string(i) + "');");
    }
    for (int i = 17; i <= 23; ++i) {
        db.execute("INSERT INTO t VALUES (" + std::to_string(i) + ", 'short_" + std::to_string(i) + "');");
    }
    std::string p300(300, 'Y');
    for (int i = 24; i <= 33; ++i) {
        db.execute("INSERT INTO t VALUES (" + std::to_string(i) + ", '" + p300 + "');");
    }
}

void test_explicit_txn_abort_reproduction() {
    std::string db_name = "test_abort_repro.db";
    clear_files(db_name);
    std::string z800(800, 'Z');
    {
        DatabaseEngine db(db_name);
        setup_reproduction_dataset(db);

        db.execute("BEGIN;");
        assert(db.in_transaction());
        assert(db.is_explicit_txn());
        assert(!db.is_txn_aborted());

        // Target failing update: Row 1 succeeds on Page 1, Row 17 overflows Page 2
        bool update_threw = false;
        try {
            db.execute("UPDATE t SET payload = '" + z800 + "' WHERE id = 1 OR id = 17;");
        } catch (const std::exception& e) {
            update_threw = true;
            std::string msg = e.what();
            assert(msg.find("exceeds available page space") != std::string::npos);
        }
        assert(update_threw);

        // 1. Transaction marked aborted
        assert(db.is_txn_aborted());
        assert(db.in_transaction());

        // 2. SELECT is rejected until ROLLBACK
        bool select_rejected = false;
        try {
            db.execute("SELECT * FROM t WHERE id = 1;");
        } catch (const std::exception& e) {
            select_rejected = true;
            std::string msg = e.what();
            assert(msg.find("aborted") != std::string::npos);
        }
        assert(select_rejected);

        // 3. COMMIT is rejected
        bool commit_rejected = false;
        try {
            db.execute("COMMIT;");
        } catch (const std::exception& e) {
            commit_rejected = true;
            std::string msg = e.what();
            assert(msg.find("cannot COMMIT") != std::string::npos || msg.find("aborted") != std::string::npos);
        }
        assert(commit_rejected);
        assert(db.is_txn_aborted());

        // 4. A second BEGIN is rejected
        bool begin_rejected = false;
        try {
            db.execute("BEGIN;");
        } catch (const std::exception& e) {
            begin_rejected = true;
            std::string msg = e.what();
            assert(msg.find("cannot BEGIN") != std::string::npos || msg.find("aborted") != std::string::npos);
        }
        assert(begin_rejected);
        assert(db.is_txn_aborted());

        // 5. ROLLBACK succeeds
        db.execute("ROLLBACK;");
        assert(!db.is_txn_aborted());
        assert(!db.in_transaction());
    }

    // 6. Close and reopen database: verify consistency
    {
        DatabaseEngine db2(db_name);
        // Row 1 and Row 17 still have original payloads
        assert_select(db2, "SELECT * FROM t WHERE id = 1;", "1 | short_1");
        assert_select(db2, "SELECT * FROM t WHERE id = 17;", "17 | short_17");

        // Index lookups
        assert_select(db2, "SELECT * FROM t WHERE payload = 'short_1';", "1 | short_1");
        assert_select(db2, "SELECT * FROM t WHERE payload = 'short_17';", "17 | short_17");

        // Index lookup for replacement string returns 0 rows (no phantom entry)
        {
            std::stringstream buffer;
            std::streambuf* old = std::cout.rdbuf(buffer.rdbuf());
            db2.execute("SELECT * FROM t WHERE payload = '" + z800 + "';");
            std::cout.rdbuf(old);
            assert(buffer.str().find("(0 rows)") != std::string::npos);
        }

        // 7. Fresh transaction can begin and commit successfully after rollback
        db2.execute("BEGIN;");
        db2.execute("INSERT INTO t VALUES (999, 'fresh_val');");
        db2.execute("COMMIT;");
        assert_select(db2, "SELECT * FROM t WHERE id = 999;", "999 | fresh_val");
    }
    clear_files(db_name);
    std::cout << "PASS: test_explicit_txn_abort_reproduction\n";
}

void test_auto_txn_failure_preserves_state() {
    std::string db_name = "test_abort_auto.db";
    clear_files(db_name);
    std::string z800(800, 'Z');
    {
        DatabaseEngine db(db_name);
        setup_reproduction_dataset(db);

        // Auto-transaction (no BEGIN)
        bool auto_threw = false;
        try {
            db.execute("UPDATE t SET payload = '" + z800 + "' WHERE id = 1 OR id = 17;");
        } catch (const std::exception&) {
            auto_threw = true;
        }
        assert(auto_threw);
        assert(!db.in_transaction());
        assert(!db.is_txn_aborted());
    }
    {
        DatabaseEngine db2(db_name);
        assert_select(db2, "SELECT * FROM t WHERE id = 1;", "1 | short_1");
        assert_select(db2, "SELECT * FROM t WHERE id = 17;", "17 | short_17");
    }
    clear_files(db_name);
    std::cout << "PASS: test_auto_txn_failure_preserves_state\n";
}

void test_explicit_txn_success_commit() {
    std::string db_name = "test_abort_commit.db";
    clear_files(db_name);
    {
        DatabaseEngine db(db_name);
        setup_reproduction_dataset(db);

        db.execute("BEGIN;");
        db.execute("UPDATE t SET payload = 'updated_1' WHERE id = 1;");
        db.execute("COMMIT;");
        assert(!db.in_transaction());
        assert(!db.is_txn_aborted());
    }
    {
        DatabaseEngine db2(db_name);
        assert_select(db2, "SELECT * FROM t WHERE id = 1;", "1 | updated_1");
        assert_select(db2, "SELECT * FROM t WHERE payload = 'updated_1';", "1 | updated_1");
    }
    clear_files(db_name);
    std::cout << "PASS: test_explicit_txn_success_commit\n";
}

void test_explicit_txn_failing_update_and_rollback() {
    std::string db_name = "test_abort_rb.db";
    clear_files(db_name);
    std::string z800(800, 'Z');
    {
        DatabaseEngine db(db_name);
        setup_reproduction_dataset(db);

        db.execute("BEGIN;");
        try {
            db.execute("UPDATE t SET payload = '" + z800 + "' WHERE id = 1 OR id = 17;");
        } catch (...) {}
        assert(db.is_txn_aborted());

        db.execute("ROLLBACK;");
        assert(!db.is_txn_aborted());
        assert(!db.in_transaction());
    }
    {
        DatabaseEngine db2(db_name);
        assert_select(db2, "SELECT * FROM t WHERE id = 1;", "1 | short_1");
        assert_select(db2, "SELECT * FROM t WHERE id = 17;", "17 | short_17");
    }
    clear_files(db_name);
    std::cout << "PASS: test_explicit_txn_failing_update_and_rollback\n";
}

void test_explicit_txn_failing_update_attempt_commit() {
    std::string db_name = "test_abort_try_commit.db";
    clear_files(db_name);
    std::string z800(800, 'Z');
    {
        DatabaseEngine db(db_name);
        setup_reproduction_dataset(db);

        db.execute("BEGIN;");
        try {
            db.execute("UPDATE t SET payload = '" + z800 + "' WHERE id = 1 OR id = 17;");
        } catch (...) {}

        bool commit_failed = false;
        try {
            db.execute("COMMIT;");
        } catch (const std::exception& e) {
            commit_failed = true;
            std::string msg = e.what();
            assert(msg.find("cannot COMMIT") != std::string::npos || msg.find("aborted") != std::string::npos);
        }
        assert(commit_failed);
        assert(db.is_txn_aborted());

        db.execute("ROLLBACK;");
        assert(!db.is_txn_aborted());
    }
    {
        DatabaseEngine db2(db_name);
        assert_select(db2, "SELECT * FROM t WHERE id = 1;", "1 | short_1");
        assert_select(db2, "SELECT * FROM t WHERE id = 17;", "17 | short_17");
    }
    clear_files(db_name);
    std::cout << "PASS: test_explicit_txn_failing_update_attempt_commit\n";
}

void test_explicit_txn_failing_update_attempt_select_then_rollback() {
    std::string db_name = "test_abort_try_select.db";
    clear_files(db_name);
    std::string z800(800, 'Z');
    {
        DatabaseEngine db(db_name);
        setup_reproduction_dataset(db);

        db.execute("BEGIN;");
        try {
            db.execute("UPDATE t SET payload = '" + z800 + "' WHERE id = 1 OR id = 17;");
        } catch (...) {}

        bool select_failed = false;
        try {
            db.execute("SELECT * FROM t WHERE id = 1;");
        } catch (const std::exception& e) {
            select_failed = true;
            std::string msg = e.what();
            assert(msg.find("aborted") != std::string::npos);
        }
        assert(select_failed);
        assert(db.is_txn_aborted());

        db.execute("ROLLBACK;");
        assert(!db.is_txn_aborted());
    }
    {
        DatabaseEngine db2(db_name);
        assert_select(db2, "SELECT * FROM t WHERE id = 1;", "1 | short_1");
        assert_select(db2, "SELECT * FROM t WHERE id = 17;", "17 | short_17");
    }
    clear_files(db_name);
    std::cout << "PASS: test_explicit_txn_failing_update_attempt_select_then_rollback\n";
}

int main() {
    test_insert_recovery();
    test_update_recovery();
    test_delete_recovery();
    test_uncommitted_update_undo();
    test_multi_statement_transaction();
    test_indexed_delete_recovery();
    test_btree_root_split_recovery();
    test_indexed_split_recovery();
    test_indexed_update_crash_test();
    
    // Explicit transaction abort & atomicity regression tests
    test_explicit_txn_abort_reproduction();
    test_auto_txn_failure_preserves_state();
    test_explicit_txn_success_commit();
    test_explicit_txn_failing_update_and_rollback();
    test_explicit_txn_failing_update_attempt_commit();
    test_explicit_txn_failing_update_attempt_select_then_rollback();

    std::cout << "ALL DURABILITY & TRANSACTION ABORT TESTS PASSED.\n";
    return 0;
}
