#pragma once
#include "pager.h"
#include "btree_node.h"
#include "row_serializer.h"
#include <optional>
#include <stdexcept>
#include <vector>

class BTree {
public:
    BTree(Pager& pager, PageId root_page, size_t num_cols)
        : pager_(pager), root_page_id_(root_page), num_cols_(num_cols) {}

    static PageId create(Pager& pager) {
        PageId root_id = pager.allocate_page();
        Page* root = pager.get_page(root_id);
        BTreeNode node(root);
        node.init_leaf(true);
        pager.mark_dirty(root_id);
        return root_id;
    }

    void insert(const Row& row) {
        int64_t key       = std::get<int64_t>(row[0]);
        auto    row_bytes = RowSerializer::serialize(row);
        PageId  leaf_id   = find_leaf(key);
        Page*   lp        = pager_.get_page(leaf_id);
        BTreeNode leaf(lp);

        if (leaf.leaf_is_full())
            split_and_insert(leaf_id, key, row_bytes);
        else
            insert_into_leaf(leaf_id, key, row_bytes);
    }

    std::optional<Row> search(int64_t key) {
        PageId leaf_id = find_leaf(key);
        Page* page = pager_.get_page(leaf_id);
        BTreeNode node(page);
        uint16_t n = node.num_keys();
        uint16_t lo = 0, hi = n;
        while (lo < hi) {
            uint16_t mid = lo + (hi - lo) / 2;
            int64_t mk = node.leaf_key(mid);
            if (mk == key)  return RowSerializer::deserialize(node.leaf_row_bytes(mid), num_cols_);
            if (mk < key)   lo = mid + 1;
            else            hi = mid;
        }
        return std::nullopt;
    }

    std::vector<Row> scan_all() {
        std::vector<Row> result;
        PageId leaf_id = leftmost_leaf();
        while (leaf_id != NO_PAGE) {
            Page* page = pager_.get_page(leaf_id);
            BTreeNode node(page);
            for (uint16_t i = 0; i < node.num_keys(); i++)
                result.push_back(RowSerializer::deserialize(node.leaf_row_bytes(i), num_cols_));
            leaf_id = node.next_leaf();
        }
        return result;
    }

    PageId root_page_id() const { return root_page_id_; }

private:
    Pager&  pager_;
    PageId  root_page_id_;
    size_t  num_cols_;

    // ── Descend from root to the correct leaf for `key` ──
    PageId find_leaf(int64_t key) {
        PageId cur = root_page_id_;
        while (true) {
            Page* p = pager_.get_page(cur);
            BTreeNode n(p);
            if (n.node_type() == NodeType::Leaf) return cur;
            // Find child: child[i] covers keys < key[i]
            // child[num_keys] covers keys >= key[num_keys-1]
            uint16_t nk = n.num_keys();
            uint16_t i  = 0;
            while (i < nk && key >= n.internal_key(i)) i++;
            cur = (i < nk) ? n.internal_child(i) : n.internal_rightmost_child();
        }
    }

    // ── Walk left-most path to reach smallest-key leaf ──
    PageId leftmost_leaf() {
        PageId cur = root_page_id_;
        while (true) {
            Page* p = pager_.get_page(cur);
            BTreeNode n(p);
            if (n.node_type() == NodeType::Leaf) return cur;
            cur = n.internal_child(0);
        }
    }

    // ── Insert key+bytes into a leaf that has room ──
    void insert_into_leaf(PageId leaf_id, int64_t key,
                          const std::vector<std::byte>& row_bytes) {
        Page* page = pager_.get_page(leaf_id);
        BTreeNode node(page);
        uint16_t n = node.num_keys();

        uint16_t slot = lower_bound(node, n, key);
        if (slot < n && node.leaf_key(slot) == key)
            throw std::runtime_error("Duplicate PK: " + std::to_string(key));

        if (slot < n) {
            // Read all cells, open a gap at slot, rewrite everything
            std::vector<std::pair<int64_t,std::vector<std::byte>>> cells;
            for (uint16_t i = 0; i < n; i++)
                cells.emplace_back(node.leaf_key(i), node.leaf_row_bytes(i));
            cells.insert(cells.begin()+slot, {key, row_bytes});
            for (uint16_t i = 0; i < (uint16_t)cells.size(); i++)
                node.leaf_write_cell(i, cells[i].first, cells[i].second);
        } else {
            node.leaf_write_cell(slot, key, row_bytes);
        }
        node.set_num_keys(n + 1);
        pager_.mark_dirty(leaf_id);
    }

    // ── Split a full leaf, then insert the new key into the right half ──
    void split_and_insert(PageId old_id, int64_t new_key,
                          const std::vector<std::byte>& new_bytes) {
        Page* op = pager_.get_page(old_id);
        BTreeNode old_node(op);
        uint16_t n = old_node.num_keys();

        // Collect all existing cells + new one into sorted order
        std::vector<std::pair<int64_t,std::vector<std::byte>>> cells;
        cells.reserve(n + 1);
        for (uint16_t i = 0; i < n; i++)
            cells.emplace_back(old_node.leaf_key(i), old_node.leaf_row_bytes(i));
        uint16_t ins = 0;
        while (ins < (uint16_t)cells.size() && cells[ins].first < new_key) ins++;
        cells.insert(cells.begin()+ins, {new_key, new_bytes});

        uint16_t total       = static_cast<uint16_t>(cells.size());
        uint16_t left_count  = total / 2;
        uint16_t right_count = total - left_count;

        // Allocate right leaf
        PageId new_id = pager_.allocate_page();
        Page* np = pager_.get_page(new_id);
        BTreeNode new_node(np);
        new_node.init_leaf(false);

        // Write left half back into old page
        old_node.set_num_keys(0);
        for (uint16_t i = 0; i < left_count; i++)
            old_node.leaf_write_cell(i, cells[i].first, cells[i].second);
        old_node.set_num_keys(left_count);

        // Write right half into new page
        for (uint16_t i = 0; i < right_count; i++)
            new_node.leaf_write_cell(i, cells[left_count+i].first,
                                        cells[left_count+i].second);
        new_node.set_num_keys(right_count);

        // Link leaf chain: old → new → whatever old pointed to before
        new_node.set_next_leaf(old_node.next_leaf());
        old_node.set_next_leaf(new_id);

        // Separator = first key of the right half
        int64_t sep = cells[left_count].first;

        pager_.mark_dirty(old_id);
        pager_.mark_dirty(new_id);

        if (old_node.is_root()) {
            create_new_root(old_id, sep, new_id);
        } else {
            PageId parent_id = old_node.parent_page();
            push_up_to_parent(parent_id, sep, old_id, new_id);
        }
    }

    // ── Create a brand-new internal root above two children ──
    // Internal layout: child_0(4) | key_0(8) | child_1(4)
    //   child_0 = left (keys < sep)
    //   key_0   = sep
    //   child_1 = right (keys >= sep)  ← this is the rightmost child
    void create_new_root(PageId left_child, int64_t sep, PageId right_child) {
        PageId new_root = pager_.allocate_page();
        Page*  rp       = pager_.get_page(new_root);
        BTreeNode root_node(rp);
        root_node.init_internal(true);

        // child[0] = left, key[0] = sep, rightmost = right
        root_node.internal_set_child(0, left_child);
        root_node.internal_set_key(0, sep);
        root_node.set_num_keys(1);
        // rightmost child is at internal_child_offset(num_keys) = offset of child[1]
        root_node.internal_set_rightmost_child(right_child);

        pager_.mark_dirty(new_root);

        // Update children's parent pointers
        {
            Page* lp = pager_.get_page(left_child);
            BTreeNode ln(lp);
            ln.set_is_root(false);
            ln.set_parent_page(new_root);
            pager_.mark_dirty(left_child);
        }
        {
            Page* rcp = pager_.get_page(right_child);
            BTreeNode rn(rcp);
            rn.set_parent_page(new_root);
            pager_.mark_dirty(right_child);
        }

        root_page_id_ = new_root;
    }

    // ── Push separator key up into an existing internal parent ──
    // After splitting a leaf, the parent gains one new key + one new right child.
    // `left_child`  = the old leaf (already in parent's child array)
    // `right_child` = the new leaf (needs to be added to the right of sep)
    void push_up_to_parent(PageId parent_id, int64_t sep,
                           PageId /*left_child*/, PageId right_child) {
        Page* pp = pager_.get_page(parent_id);
        BTreeNode pn(pp);
        uint16_t n = pn.num_keys();

        // Collect existing (key, right-child) pairs into vectors.
        // Internal node with N keys has N+1 children:
        //   child[0], key[0], child[1], key[1], ..., key[N-1], child[N]
        // We represent as parallel arrays:
        //   keys[0..N-1]  and  children[0..N]
        std::vector<int64_t> keys;
        std::vector<PageId>  children;
        keys.reserve(n + 1);
        children.reserve(n + 2);

        children.push_back(pn.internal_child(0));
        for (uint16_t i = 0; i < n; i++) {
            keys.push_back(pn.internal_key(i));
            children.push_back(pn.internal_child(i + 1) != 0
                                ? pn.internal_child(i + 1)
                                : pn.internal_rightmost_child());
        }
        // Fix: the last children entry should always be the rightmost child
        children.back() = pn.internal_rightmost_child();

        // Find insertion position for sep
        uint16_t pos = 0;
        while (pos < (uint16_t)keys.size() && keys[pos] < sep) pos++;

        // Insert new key and right_child at pos
        keys.insert(keys.begin() + pos, sep);
        children.insert(children.begin() + pos + 1, right_child);

        // Rewrite the internal node from scratch
        for (uint16_t i = 0; i < (uint16_t)keys.size(); i++) {
            pn.internal_set_child(i, children[i]);
            pn.internal_set_key(i, keys[i]);
        }
        // Last child (rightmost)
        pn.set_num_keys(static_cast<uint16_t>(keys.size()));
        pn.internal_set_rightmost_child(children.back());

        // Update right child's parent pointer
        Page* rcp = pager_.get_page(right_child);
        BTreeNode rcn(rcp);
        rcn.set_parent_page(parent_id);
        pager_.mark_dirty(right_child);
        pager_.mark_dirty(parent_id);
    }

    // ── Binary search: first slot where leaf_key(slot) >= key ──
    uint16_t lower_bound(BTreeNode& node, uint16_t n, int64_t key) {
        uint16_t lo = 0, hi = n;
        while (lo < hi) {
            uint16_t mid = lo + (hi - lo) / 2;
            if (node.leaf_key(mid) < key) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }
};
