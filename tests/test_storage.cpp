// ============================================================================
// test_storage.cpp — Tests for BTree storage primitives (Update/Delete)
// ============================================================================

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "storage/pager.h"
#include "storage/btree.h"
#include "storage/cursor.h"
#include "common/row.h"

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

static std::string temp_filename = "test_storage.db";

static void cleanup() {
    std::remove(temp_filename.c_str());
}

static Row make_row(int64_t id, const std::string& text) {
    Row r;
    r.push_back(id);
    r.push_back(text);
    return r;
}

static void testUpdateAndSizeChanges() {
    cleanup();
    Pager pager(temp_filename);
    PageId root = BTree::create(pager);
    BTree tree(pager, root, 2);

    // 1. Insert rows
    tree.insert(make_row(1, "short"));
    tree.insert(make_row(2, "medium"));
    tree.insert(make_row(3, "a_bit_longer"));

    // 2. Read them back via cursor
    BTreeCursor cursor(pager, root, 2);
    cursor.rewind();
    CHECK(!cursor.is_exhausted(), "cursor not exhausted");
    
    Row r1 = cursor.current();
    CHECK(std::get<int64_t>(r1[0]) == 1, "r1 id");
    CHECK(std::get<std::string>(r1[1]) == "short", "r1 text");

    cursor.next();
    Row r2 = cursor.current();
    CHECK(std::get<int64_t>(r2[0]) == 2, "r2 id");
    CHECK(std::get<std::string>(r2[1]) == "medium", "r2 text");

    cursor.next();
    Row r3 = cursor.current();
    CHECK(std::get<int64_t>(r3[0]) == 3, "r3 id");
    CHECK(std::get<std::string>(r3[1]) == "a_bit_longer", "r3 text");

    // 3. Update one row (middle row to longer size)
    cursor.rewind();
    cursor.next(); // point to 2
    cursor.update_current_row(make_row(2, "very_long_string_that_changes_size"));

    // 4. Read the updated row
    Row updated_r2 = cursor.current();
    CHECK(std::get<std::string>(updated_r2[1]) == "very_long_string_that_changes_size", "update changed size correctly");

    // 5. Confirm neighboring rows are unchanged
    cursor.rewind();
    Row check_r1 = cursor.current();
    CHECK(std::get<std::string>(check_r1[1]) == "short", "neighbor 1 unchanged");
    cursor.next(); // 2
    cursor.next(); // 3
    Row check_r3 = cursor.current();
    CHECK(std::get<std::string>(check_r3[1]) == "a_bit_longer", "neighbor 3 unchanged");

    // Test updating to a shorter size
    cursor.rewind();
    cursor.next(); // 2
    cursor.update_current_row(make_row(2, "tiny"));
    Row tiny_r2 = cursor.current();
    CHECK(std::get<std::string>(tiny_r2[1]) == "tiny", "update to shorter size");
    
    // Verify neighbors again
    cursor.next(); // 3
    Row check_r3_again = cursor.current();
    CHECK(std::get<std::string>(check_r3_again[1]) == "a_bit_longer", "neighbor 3 unchanged after shorten");

    // Try updating primary key (should throw)
    cursor.rewind();
    bool threw = false;
    try {
        cursor.update_current_row(make_row(99, "short"));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw, "updating primary key throws");
}

static void testDeletionPrimitives() {
    cleanup();
    Pager pager(temp_filename);
    PageId root = BTree::create(pager);
    BTree tree(pager, root, 2);

    for (int i = 1; i <= 5; i++) {
        tree.insert(make_row(i, "val" + std::to_string(i)));
    }

    BTreeCursor cursor(pager, root, 2);

    // Delete first row (1)
    cursor.rewind();
    cursor.delete_current_row();
    CHECK(std::get<int64_t>(cursor.current()[0]) == 2, "cursor now points to 2 after deleting first row");

    // Delete middle row (3)
    cursor.next(); // points to 3
    cursor.delete_current_row();
    CHECK(std::get<int64_t>(cursor.current()[0]) == 4, "cursor now points to 4 after deleting middle row");

    // Delete last row (5)
    cursor.next(); // points to 5
    cursor.delete_current_row();
    CHECK(cursor.is_exhausted(), "cursor is exhausted after deleting last row");

    // Read all remaining rows (2, 4)
    cursor.rewind();
    CHECK(std::get<int64_t>(cursor.current()[0]) == 2, "first remaining is 2");
    cursor.next();
    CHECK(std::get<int64_t>(cursor.current()[0]) == 4, "second remaining is 4");
    cursor.next();
    CHECK(cursor.is_exhausted(), "only 2 remaining");

    // Verify seek()
    CHECK(cursor.seek(2LL) == true, "seek to 2 succeeds");
    CHECK(cursor.seek(1LL) == false, "seek to 1 fails (deleted)");
    CHECK(cursor.seek(3LL) == false, "seek to 3 fails (deleted)");
}

static void testAcrossLeafSplit() {
    cleanup();
    Pager pager(temp_filename);
    PageId root = BTree::create(pager);
    BTree tree(pager, root, 2);

    // LEAF_MAX_CELLS is 32. We insert 40 to force a split.
    for (int i = 1; i <= 40; i++) {
        tree.insert(make_row(i, "data" + std::to_string(i)));
    }

    BTreeCursor cursor(pager, tree.root_page_id(), 2);
    
    // Delete a row near the end (forces reading from the second leaf)
    CHECK(cursor.seek(35LL), "seek to 35");
    cursor.delete_current_row();
    CHECK(std::get<int64_t>(cursor.current()[0]) == 36, "cursor points to 36 after deleting 35");

    // Update a row near the beginning (first leaf)
    CHECK(cursor.seek(5LL), "seek to 5");
    cursor.update_current_row(make_row(5, "updated_5"));
    
    // Verify
    CHECK(cursor.seek(5LL), "seek to 5 again");
    CHECK(std::get<std::string>(cursor.current()[1]) == "updated_5", "update across splits works");

    CHECK(cursor.seek(35LL) == false, "35 remains deleted");
}

int main() {
    std::cout << "Running Storage Tests...\n";
    testUpdateAndSizeChanges();
    testDeletionPrimitives();
    testAcrossLeafSplit();

    std::cout << "Storage: " << passed << " passed, " << failed << " failed.\n";
    cleanup();
    return failed > 0 ? 1 : 0;
}
