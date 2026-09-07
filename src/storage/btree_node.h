#pragma once
#include "page.h"
#include "../common/row.h"
#include <cstring>
#include <cassert>
#include <vector>

// Header byte offsets
static constexpr size_t OFFSET_NODE_TYPE   = 0;
static constexpr size_t OFFSET_IS_ROOT     = 1;
static constexpr size_t OFFSET_NUM_KEYS    = 2;
static constexpr size_t OFFSET_PARENT_PAGE = 4;
static constexpr size_t OFFSET_NEXT_LEAF   = 8;
static constexpr size_t HEADER_SIZE        = 16;
static constexpr PageId NO_PAGE = UINT32_MAX;

static constexpr size_t LEAF_CELL_KEY_SIZE   = 8;
static constexpr size_t LEAF_CELL_SIZE_FIELD = 2;
static constexpr uint16_t LEAF_MAX_CELLS     = 32;

// Internal node cell layout (per key slot):
//   [child_page: 4 bytes][key: 8 bytes]
// After the last key there is one more child_page (the rightmost child).
// So for N keys: child_0, key_0, child_1, key_1, ..., key_{N-1}, child_N
static constexpr size_t INTERNAL_CHILD_SIZE = 4;
static constexpr size_t INTERNAL_KEY_SIZE   = 8;
// Size of one (child + key) pair
static constexpr size_t INTERNAL_PAIR_SIZE  = INTERNAL_CHILD_SIZE + INTERNAL_KEY_SIZE;

enum class NodeType : uint8_t { Internal = 0, Leaf = 1 };

class BTreeNode {
public:
    explicit BTreeNode(Page* page) : page_(page) { assert(page_); }

    // ── Header getters/setters ──
    NodeType node_type()    const { return static_cast<NodeType>(read_byte(OFFSET_NODE_TYPE)); }
    bool     is_root()      const { return read_byte(OFFSET_IS_ROOT) != 0; }
    uint16_t num_keys()     const { return read_u16(OFFSET_NUM_KEYS); }
    PageId   parent_page()  const { return read_u32(OFFSET_PARENT_PAGE); }
    PageId   next_leaf()    const { return read_u32(OFFSET_NEXT_LEAF); }

    void set_node_type(NodeType t)   { write_byte(OFFSET_NODE_TYPE, static_cast<uint8_t>(t)); }
    void set_is_root(bool r)         { write_byte(OFFSET_IS_ROOT, r ? 1 : 0); }
    void set_num_keys(uint16_t n)    { write_u16(OFFSET_NUM_KEYS, n); }
    void set_parent_page(PageId id)  { write_u32(OFFSET_PARENT_PAGE, id); }
    void set_next_leaf(PageId id)    { write_u32(OFFSET_NEXT_LEAF, id); }

    void init_leaf(bool is_root_node = false) {
        set_node_type(NodeType::Leaf);
        set_is_root(is_root_node);
        set_num_keys(0);
        set_parent_page(0);
        set_next_leaf(NO_PAGE);
    }
    void init_internal(bool is_root_node = false) {
        set_node_type(NodeType::Internal);
        set_is_root(is_root_node);
        set_num_keys(0);
        set_parent_page(0);
        set_next_leaf(NO_PAGE);
    }

    // ── Leaf cell access ──
    // Layout: [key:8][row_size:2][row_bytes:row_size] packed from HEADER_SIZE
    size_t leaf_cell_offset(uint16_t slot) const {
        size_t offset = HEADER_SIZE;
        for (uint16_t i = 0; i < slot; i++) {
            uint16_t row_size;
            std::memcpy(&row_size, page_->data.data() + offset + 8, 2);
            offset += 8 + 2 + row_size;
        }
        return offset;
    }

    int64_t leaf_key(uint16_t slot) const {
        int64_t key;
        std::memcpy(&key, page_->data.data() + leaf_cell_offset(slot), 8);
        return key;
    }

    std::vector<std::byte> leaf_row_bytes(uint16_t slot) const {
        size_t off = leaf_cell_offset(slot) + 8;
        uint16_t row_size;
        std::memcpy(&row_size, page_->data.data() + off, 2);
        off += 2;
        return std::vector<std::byte>(
            page_->data.data() + off,
            page_->data.data() + off + row_size);
    }

    void leaf_write_cell(uint16_t slot, int64_t key,
                         const std::vector<std::byte>& row_bytes) {
        size_t off = leaf_cell_offset(slot);
        std::memcpy(page_->data.data() + off, &key, 8);
        uint16_t sz = static_cast<uint16_t>(row_bytes.size());
        std::memcpy(page_->data.data() + off + 8, &sz, 2);
        std::memcpy(page_->data.data() + off + 10, row_bytes.data(), sz);
    }

    bool leaf_is_full() const { return num_keys() >= LEAF_MAX_CELLS; }

    // ── Internal cell access ──
    // Layout from HEADER_SIZE:
    //   child_0(4), key_0(8), child_1(4), key_1(8), ..., key_{N-1}(8), child_N(4)
    //
    // Offset of child[i] = HEADER_SIZE + i * (4 + 8)
    // Offset of key[i]   = HEADER_SIZE + i * (4 + 8) + 4
    // Offset of rightmost child (child[N]) = HEADER_SIZE + N * (4 + 8)

    size_t internal_child_offset(uint16_t idx) const {
        return HEADER_SIZE + static_cast<size_t>(idx) * INTERNAL_PAIR_SIZE;
    }
    size_t internal_key_offset(uint16_t idx) const {
        return internal_child_offset(idx) + INTERNAL_CHILD_SIZE;
    }

    // child to the LEFT of key[slot]
    PageId internal_child(uint16_t slot) const {
        return read_u32(internal_child_offset(slot));
    }

    int64_t internal_key(uint16_t slot) const {
        int64_t key;
        std::memcpy(&key, page_->data.data() + internal_key_offset(slot), 8);
        return key;
    }

    // rightmost child = child AFTER the last key
    PageId internal_rightmost_child() const {
        return read_u32(internal_child_offset(num_keys()));
    }

    void internal_set_child(uint16_t idx, PageId child) {
        write_u32(internal_child_offset(idx), child);
    }
    void internal_set_key(uint16_t idx, int64_t key) {
        std::memcpy(page_->data.data() + internal_key_offset(idx), &key, 8);
    }
    void internal_set_rightmost_child(PageId child) {
        write_u32(internal_child_offset(num_keys()), child);
    }

    PageId page_id() const { return page_->id; }

private:
    Page* page_;

    uint8_t  read_byte(size_t o) const { return static_cast<uint8_t>(page_->data[o]); }
    void     write_byte(size_t o, uint8_t v) { page_->data[o] = static_cast<std::byte>(v); }
    uint16_t read_u16(size_t o) const { uint16_t v; std::memcpy(&v, page_->data.data()+o, 2); return v; }
    void     write_u16(size_t o, uint16_t v) { std::memcpy(page_->data.data()+o, &v, 2); }
    uint32_t read_u32(size_t o) const { uint32_t v; std::memcpy(&v, page_->data.data()+o, 4); return v; }
    void     write_u32(size_t o, uint32_t v) { std::memcpy(page_->data.data()+o, &v, 4); }
};

// Sentinel value meaning "no next leaf page" — we can't use 0
// because page 0 is a valid page (the first leaf).
