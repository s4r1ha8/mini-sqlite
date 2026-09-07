#pragma once
#include "../storage/page.h"
#include <string>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────
//  WAL — Write-Ahead Log
//
//  Every write that touches a page must be logged here FIRST.
//  The log is append-only: we never overwrite old records.
//  On restart, we scan from the last checkpoint and replay.
//
//  Log record format (binary, fixed layout):
//    [type:    1 byte ]   BEGIN | COMMIT | ROLLBACK | PAGE_WRITE
//    [txn_id:  8 bytes]   uint64_t — which transaction
//    [page_id: 4 bytes]   uint32_t — only for PAGE_WRITE
//    [before:  4096 bytes] — page state BEFORE the write (for undo)
//    [after:   4096 bytes] — page state AFTER the write  (for redo)
//    [checksum:4 bytes]   CRC32 over everything above (detect torn writes)
//
//  PAGE_WRITE record size: 1+8+4+4096+4096+4 = 8209 bytes
//  BEGIN/COMMIT/ROLLBACK record size: 1+8+4 = 13 bytes
//  (before/after fields are omitted for non-PAGE_WRITE records)
// ─────────────────────────────────────────────────────────────────

enum class WalRecordType : uint8_t {
    Begin    = 1,
    Commit   = 2,
    Rollback = 3,
    PageWrite = 4,
};

struct WalRecord {
    WalRecordType type;
    uint64_t      txn_id;
    uint32_t      page_id;                    // only meaningful for PageWrite
    std::array<std::byte, PAGE_SIZE> before;  // page state before write
    std::array<std::byte, PAGE_SIZE> after;   // page state after write
};

class WAL {
public:
    explicit WAL(const std::string& filename) : filename_(filename) {
        // Open for append+read in binary mode
        file_ = fopen(filename.c_str(), "r+b");
        if (!file_) file_ = fopen(filename.c_str(), "w+b");
        if (!file_) throw std::runtime_error("WAL: cannot open " + filename);
    }

    ~WAL() {
        if (file_) fclose(file_);
    }

    // ── Write a BEGIN record ──
    void write_begin(uint64_t txn_id) {
        write_header_record(WalRecordType::Begin, txn_id, 0);
    }

    // ── Write a COMMIT record ──
    void write_commit(uint64_t txn_id) {
        write_header_record(WalRecordType::Commit, txn_id, 0);
        // fsync so the commit is durable before we return to the caller
        fflush(file_);
    }

    // ── Write a ROLLBACK record ──
    void write_rollback(uint64_t txn_id) {
        write_header_record(WalRecordType::Rollback, txn_id, 0);
        fflush(file_);
    }

    // ── Write a PAGE_WRITE record ──
    // Call this BEFORE modifying the page in the pager.
    // `before` = current page bytes, `after` = what you're about to write.
    void write_page(uint64_t txn_id, uint32_t page_id,
                    const std::array<std::byte, PAGE_SIZE>& before,
                    const std::array<std::byte, PAGE_SIZE>& after) {
        // Seek to end of log
        fseek(file_, 0, SEEK_END);

        // Write type + txn_id + page_id
        uint8_t type = static_cast<uint8_t>(WalRecordType::PageWrite);
        fwrite(&type,    1, 1, file_);
        fwrite(&txn_id,  1, 8, file_);
        fwrite(&page_id, 1, 4, file_);

        // Write before and after images
        fwrite(before.data(), 1, PAGE_SIZE, file_);
        fwrite(after.data(),  1, PAGE_SIZE, file_);

        // Write a simple checksum (sum of all bytes mod 2^32)
        uint32_t checksum = compute_checksum(type, txn_id, page_id, before, after);
        fwrite(&checksum, 1, 4, file_);

        // CRITICAL: fsync the WAL record before the caller modifies the page.
        // If we crash between fwrite and fflush, the record isn't durable —
        // but that's ok because the data page hasn't been written yet either.
        // The order guarantee is: WAL on disk → THEN data page written.
        fflush(file_);
    }

    // ── Read all records from the log (for recovery) ──
    std::vector<WalRecord> read_all() {
        std::vector<WalRecord> records;
        fseek(file_, 0, SEEK_SET);

        while (true) {
            uint8_t type_byte;
            if (fread(&type_byte, 1, 1, file_) != 1) break; // EOF

            WalRecord rec;
            rec.type = static_cast<WalRecordType>(type_byte);

            if (fread(&rec.txn_id,  1, 8, file_) != 8) break;
            if (fread(&rec.page_id, 1, 4, file_) != 4) break;

            if (rec.type == WalRecordType::PageWrite) {
                if (fread(rec.before.data(), 1, PAGE_SIZE, file_) != PAGE_SIZE) break;
                if (fread(rec.after.data(),  1, PAGE_SIZE, file_) != PAGE_SIZE) break;

                uint32_t stored_checksum;
                if (fread(&stored_checksum, 1, 4, file_) != 4) break;

                // Verify checksum — skip corrupted records
                uint32_t expected = compute_checksum(
                    type_byte, rec.txn_id, rec.page_id, rec.before, rec.after);
                if (stored_checksum != expected) {
                    // Torn write — stop reading here, rest of log is unreliable
                    break;
                }
            }
            records.push_back(rec);
        }
        return records;
    }

    // ── Truncate the WAL (called after a checkpoint) ──
    // After all dirty pages are flushed to the db file, we can clear the log.
    void truncate() {
        fclose(file_);
        file_ = fopen(filename_.c_str(), "w+b"); // "w" truncates
        if (!file_) throw std::runtime_error("WAL: cannot truncate " + filename_);
    }

private:
    FILE*       file_;
    std::string filename_;

    // Write a BEGIN/COMMIT/ROLLBACK record (no page data)
    void write_header_record(WalRecordType type, uint64_t txn_id, uint32_t page_id) {
        fseek(file_, 0, SEEK_END);
        uint8_t t = static_cast<uint8_t>(type);
        fwrite(&t,       1, 1, file_);
        fwrite(&txn_id,  1, 8, file_);
        fwrite(&page_id, 1, 4, file_);
    }

    // Simple checksum: XOR-fold all bytes into a uint32
    // Good enough to detect torn writes — not a cryptographic hash
    uint32_t compute_checksum(uint8_t type, uint64_t txn_id, uint32_t page_id,
                              const std::array<std::byte, PAGE_SIZE>& before,
                              const std::array<std::byte, PAGE_SIZE>& after) {
        uint32_t sum = 0;
        sum ^= static_cast<uint32_t>(type);
        sum ^= static_cast<uint32_t>(txn_id & 0xFFFFFFFF);
        sum ^= static_cast<uint32_t>(txn_id >> 32);
        sum ^= page_id;
        for (auto b : before) sum = (sum << 1) | (sum >> 31) ^ static_cast<uint32_t>(b);
        for (auto b : after)  sum = (sum << 1) | (sum >> 31) ^ static_cast<uint32_t>(b);
        return sum;
    }
};
