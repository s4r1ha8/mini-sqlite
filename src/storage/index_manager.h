#pragma once
#include "pager.h"
#include "btree.h"
#include "../common/row.h"
#include <optional>
#include <cstring>

// ─────────────────────────────────────────────────────────────────
//  IndexManager
//
//  A secondary index = another B-tree keyed on a non-PK column.
//  Each entry stores: [index_key: int64][pk: int64]
//  (2-column row so we can recover the PK after lookup)
//
//  IMPORTANT: insert_entry returns the updated root_page_id.
//  After enough inserts the index B-tree splits too, and its root
//  page changes — always save the returned value back to your
//  idx_root variable, exactly like you do for the main table.
// ─────────────────────────────────────────────────────────────────
class IndexManager {
public:

    static PageId create(Pager& pager) {
        return BTree::create(pager);
    }

    // Insert index_val → pk mapping.
    // Returns the (possibly updated) index root page id after splits.
    static PageId insert_entry(Pager& pager, PageId idx_root,
                               const Value& indexed_val, int64_t pk) {
        int64_t key = value_to_key(indexed_val);
        BTree idx_tree(pager, idx_root, 2); // 2 cols: [key, pk]
        idx_tree.insert({ Value(key), Value(pk) });
        return idx_tree.root_page_id(); // save this!
    }

    // Look up PK for an indexed value. Returns nullopt if not found.
    static std::optional<int64_t> lookup(Pager& pager, PageId idx_root,
                                         const Value& indexed_val) {
        int64_t key = value_to_key(indexed_val);
        BTree idx_tree(pager, idx_root, 2);
        auto result = idx_tree.search(key);
        if (!result.has_value()) return std::nullopt;
        return std::get<int64_t>((*result)[1]); // col[1] is the PK
    }

private:
    // Convert any Value to an int64 key.
    // Integers: used directly.
    // Strings: FNV-1a hash (good for point lookups, not range scans).
    // Doubles: bit-cast.
    static int64_t value_to_key(const Value& v) {
        return std::visit([](const auto& val) -> int64_t {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, int64_t>) {
                return val;
            } else if constexpr (std::is_same_v<T, std::string>) {
                int64_t hash = static_cast<int64_t>(0xcbf29ce484222325ULL);
                for (char c : val) {
                    hash ^= static_cast<int64_t>(static_cast<uint8_t>(c));
                    hash *= static_cast<int64_t>(0x100000001b3ULL);
                }
                return hash;
            } else if constexpr (std::is_same_v<T, double>) {
                int64_t bits;
                std::memcpy(&bits, &val, 8);
                return bits;
            } else {
                return 0LL;
            }
        }, v);
    }
};
