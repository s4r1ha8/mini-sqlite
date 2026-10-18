#include "pager.h"
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <share.h>
#include <io.h> // for _commit

Pager::Pager(const std::string& filename) : num_pages_(0) {
    // Open exclusively so no other process can read or write it while we have it.
    file_ = _fsopen(filename.c_str(), "r+b", _SH_DENYRW);
    if (file_ == nullptr) {
        if (errno == EACCES) {
            throw std::runtime_error("Database is locked by another process (cannot open file).");
        }
        // Try creating it
        file_ = _fsopen(filename.c_str(), "w+b", _SH_DENYRW);
    }
    if (file_ == nullptr) {
        if (errno == EACCES) {
            throw std::runtime_error("Database is locked by another process (cannot open file).");
        }
        throw std::runtime_error("Pager: could not open or create file " + filename + " (errno: " + std::to_string(errno) + ")");
    }

    // Figure out how many whole pages already exist in the file by
    // checking the file's total size.
    fseek(file_, 0, SEEK_END);
    long file_size = ftell(file_);
    num_pages_ = static_cast<uint32_t>(file_size / PAGE_SIZE);
}

Pager::~Pager() {
    // DO NOT flush_all() here.
    // This violated NO-STEAL. Only explicit txn_mgr.commit() or checkpoints
    // are allowed to write dirty pages to disk.
    if (file_) fclose(file_);
}

Page* Pager::get_page(PageId id) {
    // 1. Already in memory? Just hand back a pointer to it.
    auto it = cache_.find(id);
    if (it != cache_.end()) {
        return &it->second;
    }

    // 2. Not cached — create a slot for it and try to load from disk.
    Page& page = cache_[id];  // inserts a fresh zeroed Page
    page.id = id;

    if (id < num_pages_) {
        // This page exists on disk — seek to its exact byte offset and
        // read exactly PAGE_SIZE bytes into our buffer.
        fseek(file_, static_cast<long>(id) * PAGE_SIZE, SEEK_SET);
        fread(page.data.data(), 1, PAGE_SIZE, file_);
    }
    // else: it's a brand-new page beyond the current end of file —
    // the zeroed buffer from Page{} is already correct, nothing to read.

    return &page;
}

void Pager::mark_dirty(PageId id) {
    cache_[id].dirty = true;
}

void Pager::flush_page(PageId id) {
    auto it = cache_.find(id);
    if (it == cache_.end() || !it->second.dirty) {
        return;  // nothing to do
    }
    Page& page = it->second;
    fseek(file_, static_cast<long>(id) * PAGE_SIZE, SEEK_SET);
    fwrite(page.data.data(), 1, PAGE_SIZE, file_);
    fflush(file_);  // push from C's buffer into the OS
    _commit(_fileno(file_)); // push from OS buffer to hardware
    page.dirty = false;
}

void Pager::flush_all() {
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        Page& page = it->second;
        if (page.dirty) {
            fseek(file_, static_cast<long>(it->first) * PAGE_SIZE, SEEK_SET);
            fwrite(page.data.data(), 1, PAGE_SIZE, file_);
            page.dirty = false;
        }
    }
    if (file_) {
        fflush(file_);
        _commit(_fileno(file_));
    }
}

PageId Pager::allocate_page() {
    PageId new_id = num_pages_;
    num_pages_++;
    // Touch it via get_page so it exists in the cache as a zeroed page.
    get_page(new_id);
    return new_id;
}

void Pager::refresh_page_count_from_file() {
    if (!file_) return;
    fseek(file_, 0, SEEK_END);
    long file_size = ftell(file_);
    num_pages_ = static_cast<uint32_t>(file_size / PAGE_SIZE);
}
