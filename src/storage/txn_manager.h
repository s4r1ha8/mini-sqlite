#pragma once
#include "wal.h"
#include "pager.h"
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <stdexcept>
#include <atomic>
#include <vector>

// ─────────────────────────────────────────────────────────────────
//  TxnManager — Transaction Manager
//
//  Coordinates BEGIN / COMMIT / ROLLBACK.
//  Every write must go through here so the WAL is kept consistent.
// ─────────────────────────────────────────────────────────────────

using TxnId = uint64_t;

class TxnManager {
public:
    TxnManager(Pager& pager, WAL& wal)
        : pager_(pager), wal_(wal), next_txn_id_(1) {}

    TxnId begin() {
        TxnId tid = next_txn_id_.fetch_add(1);
        wal_.write_begin(tid);
        active_txns_.insert(tid);
        
        uint32_t current_pages = pager_.page_count();
        txn_initial_page_count_[tid] = current_pages;
        
        for (uint32_t pg = 0; pg < current_pages; ++pg) {
            snapshot_page(tid, pg);
        }
        
        return tid;
    }

    // Capture the before-image of an existing page.
    // Call this at the start of a transaction, before any mutations.
    void snapshot_page(TxnId tid, PageId page_id) {
        if (!is_active(tid)) {
            throw std::runtime_error("TxnManager: snapshot_page on non-active txn");
        }
        auto& seen = txn_first_seen_[tid];
        if (seen.find(page_id) == seen.end()) {
            Page* page = pager_.get_page(page_id);
            txn_pages_[tid].push_back({page_id, page->data});
            seen.insert(page_id);
        }
    }

    void commit(TxnId tid) {
        if (!is_active(tid)) {
            throw std::runtime_error("TxnManager: commit on non-active txn");
        }

        // 1. Collect and write PAGE_WRITE for all existing modified pages
        auto it = txn_pages_.find(tid);
        if (it != txn_pages_.end()) {
            for (const auto& [page_id, before_image] : it->second) {
                const auto& after_image = pager_.get_page(page_id)->data;
                // Optimization & Correctness: only log if actually modified
                if (before_image != after_image) {
                    wal_.write_page(tid, page_id, before_image, after_image);
                }
            }
        }

        // 2. Collect and write PAGE_WRITE for all newly allocated pages
        uint32_t initial_count = txn_initial_page_count_[tid];
        uint32_t current_count = pager_.page_count();
        std::array<std::byte, PAGE_SIZE> zeroes{};
        
        for (uint32_t pg = initial_count; pg < current_count; ++pg) {
            const auto& after_image = pager_.get_page(pg)->data;
            // A new page's before image is always all-zeroes.
            wal_.write_page(tid, pg, zeroes, after_image);
        }

        // 3. Write COMMIT record (includes fsync inside WAL)
        wal_.write_commit(tid);

        // 4. Flush dirty pages to the main db file
        pager_.flush_all();

        // Clean up state
        active_txns_.erase(tid);
        txn_pages_.erase(tid);
        txn_first_seen_.erase(tid);
        txn_initial_page_count_.erase(tid);
    }

    void rollback(TxnId tid) {
        if (!is_active(tid)) {
            throw std::runtime_error("TxnManager: rollback on non-active txn");
        }
        wal_.write_rollback(tid);

        // Apply before-images in reverse order to undo all writes to existing pages
        auto it = txn_pages_.find(tid);
        if (it != txn_pages_.end()) {
            auto& pages = it->second;
            for (auto rit = pages.rbegin(); rit != pages.rend(); ++rit) {
                Page* page = pager_.get_page(rit->first);
                page->data = rit->second; // restore before-image
                pager_.mark_dirty(rit->first);
            }
        }
        
        // Note: New pages allocated during this txn remain in the file (orphaned)
        // but since the parent nodes (and Page 0) are rolled back, they are unreachable.
        // We do NOT rollback page_count_ because Pager has no truncate capability.

        pager_.flush_all(); // Harmless, flushes restored state

        txn_pages_.erase(tid);
        txn_first_seen_.erase(tid);
        txn_initial_page_count_.erase(tid);
        active_txns_.erase(tid);
    }

    bool is_active(TxnId tid) const {
        return active_txns_.count(tid) > 0;
    }

private:
    Pager&    pager_;
    WAL&      wal_;
    std::atomic<TxnId> next_txn_id_;

    std::unordered_set<TxnId> active_txns_;
    
    // Base page count when the txn started
    std::unordered_map<TxnId, uint32_t> txn_initial_page_count_;

    // Pages touched by each transaction: (page_id, before_image)
    std::unordered_map<TxnId,
        std::vector<std::pair<PageId, std::array<std::byte, PAGE_SIZE>>>> txn_pages_;

    std::unordered_map<TxnId, std::unordered_set<PageId>> txn_first_seen_;
};
