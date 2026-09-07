#pragma once
#include "wal.h"
#include "pager.h"
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <stdexcept>
#include <atomic>

// ─────────────────────────────────────────────────────────────────
//  TxnManager — Transaction Manager
//
//  Coordinates BEGIN / COMMIT / ROLLBACK.
//  Every write must go through here so the WAL is kept consistent.
//
//  How to use:
//    TxnManager txn(pager, wal);
//
//    uint64_t tid = txn.begin();
//    txn.write_page(tid, page_id, old_bytes, new_bytes);
//    // ... modify the page via pager ...
//    txn.commit(tid);
//
//    // If something goes wrong:
//    txn.rollback(tid);  // undoes all writes for this txn
//
//  Locking: table-level, exclusive for writes.
//  We don't attempt MVCC or row-level locking — that's PhD territory.
// ─────────────────────────────────────────────────────────────────

using TxnId = uint64_t;

class TxnManager {
public:
    TxnManager(Pager& pager, WAL& wal)
        : pager_(pager), wal_(wal), next_txn_id_(1) {}

    // ── BEGIN: start a new transaction ──
    // Returns a transaction ID you must pass to all subsequent calls.
    TxnId begin() {
        TxnId tid = next_txn_id_.fetch_add(1);
        wal_.write_begin(tid);
        active_txns_.insert(tid);
        return tid;
    }

    // ── Write a page within a transaction ──
    // Call this BEFORE modifying the page.
    // Logs before+after images to WAL, then applies the new bytes to the page.
    void write_page(TxnId tid, PageId page_id,
                    const std::array<std::byte, PAGE_SIZE>& after_bytes) {
        if (active_txns_.find(tid) == active_txns_.end()) {
            throw std::runtime_error("TxnManager: write_page on non-active txn");
        }

        // Capture the current (before) state of the page
        Page* page = pager_.get_page(page_id);
        auto before = page->data; // copy current bytes

        // Write WAL record FIRST (before touching the page)
        wal_.write_page(tid, page_id, before, after_bytes);

        // Now apply the new bytes to the in-memory page
        page->data = after_bytes;
        pager_.mark_dirty(page_id);

        // Track which pages this transaction touched (for rollback)
        txn_pages_[tid].push_back({page_id, before});
    }

    // ── COMMIT: make the transaction permanent ──
    void commit(TxnId tid) {
        if (active_txns_.find(tid) == active_txns_.end()) {
            throw std::runtime_error("TxnManager: commit on non-active txn");
        }
        // Write COMMIT record (includes fsync inside WAL)
        wal_.write_commit(tid);
        // Flush dirty pages to the main db file
        pager_.flush_all();
        // Clean up
        active_txns_.erase(tid);
        txn_pages_.erase(tid);
    }

    // ── ROLLBACK: undo all writes for this transaction ──
    void rollback(TxnId tid) {
        if (active_txns_.find(tid) == active_txns_.end()) {
            throw std::runtime_error("TxnManager: rollback on non-active txn");
        }
        wal_.write_rollback(tid);

        // Apply before-images in reverse order to undo all writes
        auto it = txn_pages_.find(tid);
        if (it != txn_pages_.end()) {
            auto& pages = it->second;
            for (auto rit = pages.rbegin(); rit != pages.rend(); ++rit) {
                Page* page = pager_.get_page(rit->first);
                page->data = rit->second; // restore before-image
                pager_.mark_dirty(rit->first);
            }
            pager_.flush_all();
            txn_pages_.erase(tid);
        }
        active_txns_.erase(tid);
    }

    bool is_active(TxnId tid) const {
        return active_txns_.count(tid) > 0;
    }

private:
    Pager&    pager_;
    WAL&      wal_;
    std::atomic<TxnId> next_txn_id_;

    // Active transaction IDs
    std::unordered_set<TxnId> active_txns_;

    // Pages touched by each transaction: (page_id, before_image)
    std::unordered_map<TxnId,
        std::vector<std::pair<PageId, std::array<std::byte, PAGE_SIZE>>>> txn_pages_;
};
