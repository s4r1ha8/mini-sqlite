#pragma once
#include "page.h"
#include <string>
#include <unordered_map>
#include <cstdio>

// Pager: the ONLY class allowed to talk directly to the database file.
// Everything above it (B-tree, executor) asks the Pager for pages by
// number and never touches the file itself. This separation matters:
// later when we add a page cache, a WAL, or change the file format,
// only this class needs to change.
class Pager {
public:
    // Opens (or creates) the database file at `filename`.
    explicit Pager(const std::string& filename);
    ~Pager();

    // Returns a pointer to the in-memory copy of page `id`.
    // If it's not already cached, reads it from disk first.
    // If the page doesn't exist yet (file isn't that long), returns a
    // fresh zeroed page.
    Page* get_page(PageId id);

    // Marks a page as changed. Call this after you write into the bytes
    // returned by get_page().
    void mark_dirty(PageId id);

    // Writes ONE dirty page back to disk immediately.
    void flush_page(PageId id);

    // Writes ALL dirty pages back to disk. Call this before closing,
    // or whenever you want a durability checkpoint.
    void flush_all();

    // Allocates a brand new page at the end of the file and returns its id.
    PageId allocate_page();

    // How many pages currently exist in the file.
    uint32_t page_count() const { return num_pages_; }

private:
    FILE* file_;
    uint32_t num_pages_;
    std::unordered_map<PageId, Page> cache_;  // pages currently in memory
};
