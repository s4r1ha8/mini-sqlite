#include "src/storage/index_manager.h"
#include "src/storage/pager.h"
#include <iostream>
#include <cassert>

int main() {
    Pager pager("test_index.db");
    PageId root = IndexManager::create(pager);
    
    // Insert Bob (PK=1)
    root = IndexManager::insert_entry(pager, root, Value("Bob"), 1);
    
    try {
        // Insert Bob (PK=2) - same indexed value
        root = IndexManager::insert_entry(pager, root, Value("Bob"), 2);
        std::cout << "SUCCESS: Duplicate inserted!" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "EXCEPTION: " << e.what() << std::endl;
    }

    std::remove("test_index.db");
    return 0;
}
