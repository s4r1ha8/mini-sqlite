#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// A "page" is just a fixed-size chunk of raw bytes — 4096 of them.
// Real databases use this size because it usually matches the operating
// system's own disk block size, which makes reads/writes efficient.
//
// We are NOT putting any structure (like "this is a row" or "this is a
// tree node") into this type. Page is deliberately dumb — just bytes.
// Higher-level code (BTreeNode, later) will interpret those bytes.
constexpr size_t PAGE_SIZE = 4096;

// A page identifier. Page 0 is the very first 4096 bytes of the file,
// page 1 is the next 4096 bytes, and so on.
using PageId = uint32_t;

struct Page {
    std::array<std::byte, PAGE_SIZE> data{};  // the raw bytes, zero-initialized
    PageId id = 0;
    bool dirty = false;  // true = changed in memory but not yet saved to disk
};
