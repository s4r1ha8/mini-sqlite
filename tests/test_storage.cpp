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

static void testEmptyLeavesAndOverflow() {
    cleanup();
    Pager pager(temp_filename);
    PageId root = BTree::create(pager);
    BTree tree(pager, root, 2);

    // Insert enough rows to create at least 3 leaves
    // LEAF_MAX_CELLS = 32. 100 rows will create 4 leaves.
    for (int i = 1; i <= 100; i++) {
        tree.insert(make_row(i, "data" + std::to_string(i)));
    }

    BTreeCursor cursor(pager, tree.root_page_id(), 2);

    // A. Empty first leaf traversal
    // Delete rows 1 to 32 (the first leaf)
    for (int i = 1; i <= 32; i++) {
        CHECK(cursor.seek(i), "seek to delete");
        cursor.delete_current_row();
    }
    cursor.rewind();
    CHECK(!cursor.is_exhausted(), "cursor not exhausted after emptying first leaf");
    CHECK(std::get<int64_t>(cursor.current()[0]) == 33, "rewind skips empty first leaf to row 33");

    // B. Multiple consecutive empty leaves
    // Delete rows 33 to 96 (next two leaves, assuming 32 per leaf)
    for (int i = 33; i <= 96; i++) {
        CHECK(cursor.seek(i), "seek to delete consecutive");
        cursor.delete_current_row();
    }
    cursor.rewind();
    CHECK(!cursor.is_exhausted(), "cursor not exhausted after multiple empty leaves");
    CHECK(std::get<int64_t>(cursor.current()[0]) == 97, "rewind skips multiple empty leaves");

    // C. Delete-current transition
    // Delete 97 to 99, 100 is left. Then delete 100 and verify exhaustion.
    CHECK(cursor.seek(97LL), "seek to 97");
    cursor.delete_current_row(); // cursor now at 98
    CHECK(std::get<int64_t>(cursor.current()[0]) == 98, "points to 98");
    cursor.delete_current_row(); // cursor now at 99
    CHECK(std::get<int64_t>(cursor.current()[0]) == 99, "points to 99");
    cursor.delete_current_row(); // cursor now at 100
    CHECK(std::get<int64_t>(cursor.current()[0]) == 100, "points to 100");
    cursor.delete_current_row(); // deletes 100, which is the last row in the entire DB
    CHECK(cursor.is_exhausted(), "cursor exhausted after deleting last row across leaves");

    // D. Seek through empty leaf
    // Insert new rows to test seek (tree is empty now, but internal nodes might still route to empty leaves)
    // Actually, tree might have many empty leaves now. Let's insert 101 and 102.
    tree.insert(make_row(101, "new101"));
    tree.insert(make_row(102, "new102"));
    
    // Seek 50 (routes to an empty leaf). It should return false, but NOT be exhausted, and point to 101!
    bool found = cursor.seek(50LL);
    CHECK(!found, "50 not found");
    CHECK(!cursor.is_exhausted(), "cursor not exhausted seeking 50");
    CHECK(std::get<int64_t>(cursor.current()[0]) == 101, "seek 50 flows to 101");

    // F. Update page overflow
    CHECK(cursor.seek(101LL), "seek 101");
    bool threw = false;
    try {
        std::string huge_string(4096, 'x'); // Exceeds PAGE_SIZE (4096)
        cursor.update_current_row(make_row(101, huge_string));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw, "page overflow throws");
    
    // Original row should be intact
    CHECK(std::get<std::string>(cursor.current()[1]) == "new101", "original row intact after failed update");
}

static void testPersistence() {
    cleanup();
    {
        Pager pager(temp_filename);
        PageId root = BTree::create(pager);
        BTree tree(pager, root, 2);

        tree.insert(make_row(1, "one"));
        tree.insert(make_row(2, "two"));
        tree.insert(make_row(3, "three"));

        BTreeCursor cursor(pager, tree.root_page_id(), 2);
        CHECK(cursor.seek(2LL), "seek 2");
        cursor.update_current_row(make_row(2, "two_updated"));

        CHECK(cursor.seek(1LL), "seek 1");
        cursor.delete_current_row();

        pager.flush_all();
    } // pager closes

    {
        Pager pager(temp_filename);
        // Assuming root is page 0 (first allocated page)
        BTreeCursor cursor(pager, 0, 2);
        cursor.rewind();
        
        CHECK(!cursor.is_exhausted(), "persistence cursor not exhausted");
        CHECK(std::get<int64_t>(cursor.current()[0]) == 2, "first row is 2 (1 was deleted)");
        CHECK(std::get<std::string>(cursor.current()[1]) == "two_updated", "row 2 is updated");

        cursor.next();
        CHECK(!cursor.is_exhausted(), "second row exists");
        CHECK(std::get<int64_t>(cursor.current()[0]) == 3, "second row is 3");
    }
}

int main() {
    std::cout << "Running Storage Tests...\n";
    testUpdateAndSizeChanges();
    testDeletionPrimitives();
    testAcrossLeafSplit();
    testEmptyLeavesAndOverflow();
    testPersistence();

    std::cout << "Storage: " << passed << " passed, " << failed << " failed.\n";
    cleanup();
    return failed > 0 ? 1 : 0;
}
