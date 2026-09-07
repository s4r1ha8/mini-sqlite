#pragma once
#include "../common/cursor_interface.h"
#include "btree.h"
#include "btree_node.h"
#include "pager.h"

class BTreeCursor : public Cursor {
public:
    BTreeCursor(Pager& pager, PageId root_page, size_t num_cols)
        : pager_(pager)
        , root_page_id_(root_page)
        , num_cols_(num_cols)
        , current_page_id_(root_page)
        , current_slot_(0)
        , exhausted_(true) {}

    bool seek(Key key) override {
        int64_t target = std::get<int64_t>(key);
        PageId leaf_id = find_leaf(target);
        Page* page = pager_.get_page(leaf_id);
        BTreeNode node(page);
        uint16_t n = node.num_keys();
        uint16_t lo = 0, hi = n;
        while (lo < hi) {
            uint16_t mid = lo + (hi - lo) / 2;
            int64_t mk = node.leaf_key(mid);
            if (mk == target) {
                current_page_id_ = leaf_id;
                current_slot_    = mid;
                exhausted_       = false;
                return true;
            } else if (mk < target) lo = mid + 1;
            else hi = mid;
        }
        current_page_id_ = leaf_id;
        current_slot_    = lo;
        exhausted_       = (lo >= n);
        return false;
    }

    bool next() override {
        if (exhausted_) return false;
        Page* page = pager_.get_page(current_page_id_);
        BTreeNode node(page);
        current_slot_++;
        if (current_slot_ < node.num_keys()) return true;
        PageId next_page = node.next_leaf();
        if (next_page == NO_PAGE) { exhausted_ = true; return false; }
        current_page_id_ = next_page;
        current_slot_    = 0;
        Page* np = pager_.get_page(next_page);
        BTreeNode nn(np);
        if (nn.num_keys() == 0) { exhausted_ = true; return false; }
        return true;
    }

    Row current() override {
        if (exhausted_)
            throw std::runtime_error("BTreeCursor::current() on exhausted cursor");
        Page* page = pager_.get_page(current_page_id_);
        BTreeNode node(page);
        auto bytes = node.leaf_row_bytes(current_slot_);
        return RowSerializer::deserialize(bytes, num_cols_);
    }

    void insert(Row row) override {
        BTree tree(pager_, root_page_id_, num_cols_);
        tree.insert(row);
        root_page_id_ = tree.root_page_id(); // update in case of split
        exhausted_ = true;
    }

    void remove() override {
        throw std::runtime_error("BTreeCursor::remove() not implemented until M2");
    }

    // Position at the very first (leftmost) row in key order.
    void rewind() {
        current_page_id_ = leftmost_leaf();
        current_slot_    = 0;
        Page* page = pager_.get_page(current_page_id_);
        BTreeNode node(page);
        exhausted_ = (node.num_keys() == 0);
    }

    bool is_exhausted() const { return exhausted_; }

private:
    Pager&   pager_;
    PageId   root_page_id_;
    size_t   num_cols_;
    PageId   current_page_id_;
    uint16_t current_slot_;
    bool     exhausted_;

    // Descend from root through internal nodes to the correct leaf for `key`
    PageId find_leaf(int64_t key) {
        PageId cur = root_page_id_;
        while (true) {
            Page* p = pager_.get_page(cur);
            BTreeNode n(p);
            if (n.node_type() == NodeType::Leaf) return cur;
            uint16_t nk = n.num_keys();
            uint16_t i  = 0;
            while (i < nk && key >= n.internal_key(i)) i++;
            cur = (i < nk) ? n.internal_child(i) : n.internal_rightmost_child();
        }
    }

    // Walk the leftmost path to the smallest-key leaf
    PageId leftmost_leaf() {
        PageId cur = root_page_id_;
        while (true) {
            Page* p = pager_.get_page(cur);
            BTreeNode n(p);
            if (n.node_type() == NodeType::Leaf) return cur;
            cur = n.internal_child(0);
        }
    }
};