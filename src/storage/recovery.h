#pragma once
#include "wal.h"
#include "pager.h"
#include <unordered_map>
#include <unordered_set>
#include <iostream>

// ─────────────────────────────────────────────────────────────────
//  Recovery
//
//  Called once at startup, BEFORE any queries run.
//  Scans the WAL and either:
//    - REDOs committed transactions (re-applies after-images)
//    - UNDOes incomplete transactions (applies before-images)
//
//  This is the ARIES recovery algorithm simplified:
//    1. Analysis pass: find which txns committed, which didn't
//    2. Redo pass: re-apply all committed writes (idempotent)
//    3. Undo pass: roll back all uncommitted writes
//
//  After recovery the database is in a consistent state as if
//  only the committed transactions ever happened.
// ─────────────────────────────────────────────────────────────────

class Recovery {
public:
    // Run recovery on startup.
    // Returns how many transactions were redone/undone.
    static std::pair<int,int> run(Pager& pager, WAL& wal, bool verbose = false) {
        auto records = wal.read_all();
        if (records.empty()) return {0, 0};

        if (verbose) {
            std::cout << "[Recovery] Found " << records.size()
                      << " WAL records\n";
        }

        // ── Analysis pass: which txns committed? ──
        std::unordered_set<uint64_t> committed;
        std::unordered_set<uint64_t> rolledback;
        for (const auto& rec : records) {
            if (rec.type == WalRecordType::Commit)   committed.insert(rec.txn_id);
            if (rec.type == WalRecordType::Rollback)  rolledback.insert(rec.txn_id);
        }

        // Incomplete = started but no commit or rollback record found
        std::unordered_set<uint64_t> incomplete;
        for (const auto& rec : records) {
            if (rec.type == WalRecordType::Begin) {
                if (!committed.count(rec.txn_id) && !rolledback.count(rec.txn_id)) {
                    incomplete.insert(rec.txn_id);
                }
            }
        }

        if (verbose) {
            std::cout << "[Recovery] Committed: " << committed.size()
                      << "  Incomplete: " << incomplete.size() << "\n";
        }

        // ── Redo pass: re-apply committed writes ──
        // This is safe to do even if the page already has the right data —
        // writing the after-image twice produces the same result (idempotent).
        int redone = 0;
        for (const auto& rec : records) {
            if (rec.type == WalRecordType::PageWrite
                && committed.count(rec.txn_id)) {
                Page* page = pager.get_page(rec.page_id);
                page->data = rec.after;
                pager.mark_dirty(rec.page_id);
                redone++;
            }
        }

        // ── Undo pass: roll back incomplete transactions ──
        // Apply before-images in REVERSE order so multi-page txns unwind correctly.
        int undone = 0;
        for (auto rit = records.rbegin(); rit != records.rend(); ++rit) {
            if (rit->type == WalRecordType::PageWrite
                && incomplete.count(rit->txn_id)) {
                Page* page = pager.get_page(rit->page_id);
                page->data = rit->before;
                pager.mark_dirty(rit->page_id);
                undone++;
            }
        }

        // Flush recovered state to disk
        pager.flush_all();

        if (verbose) {
            std::cout << "[Recovery] Redone: " << redone
                      << " page writes, Undone: " << undone << " page writes\n";
        }

        // Clear the WAL after successful recovery (checkpoint)
        wal.truncate();

        return {redone, undone};
    }
};
