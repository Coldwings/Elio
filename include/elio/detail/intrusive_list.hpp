#pragma once

#include <cassert>
#include <cstddef>

namespace elio::detail {

/// Base class for nodes in a generic intrusive doubly-linked list.
/// T must inherit from this class.
///
/// Usage:
///   class my_node : public intrusive_list_node<my_node> { ... };
///   intrusive_list<my_node> list;
///   list.push_back(&node);
template<typename T>
class intrusive_list_node {
public:
    intrusive_list_node() = default;

    ~intrusive_list_node() {
        // If this fires, the node was destroyed while still in a list.
        // Callers must remove the node before destruction, or the owning
        // list must be cleared first.
        assert(!in_list_ && "intrusive_list_node destroyed while still linked");
    }

    // Non-copyable, non-movable
    intrusive_list_node(const intrusive_list_node&) = delete;
    intrusive_list_node& operator=(const intrusive_list_node&) = delete;
    intrusive_list_node(intrusive_list_node&&) = delete;
    intrusive_list_node& operator=(intrusive_list_node&&) = delete;

    /// Check if this node is currently linked in a list
    bool is_linked() const noexcept { return in_list_; }

private:
    template<typename U> friend class intrusive_list;

    intrusive_list_node* prev_ = nullptr;
    intrusive_list_node* next_ = nullptr;
    bool in_list_ = false;
};

/// Generic intrusive doubly-linked list.
/// T must inherit from intrusive_list_node<T>.
///
/// All operations are O(1) except contains() which is O(n).
/// The list does NOT own its nodes — it only manages linkage.
/// Nodes must be removed before destruction (enforced by assert).
///
/// Thread safety: NOT thread-safe by itself. Callers must protect
/// all operations with an external mutex.
template<typename T>
class intrusive_list {
public:
    intrusive_list() = default;

    ~intrusive_list() {
        assert(empty() && "intrusive_list destroyed while not empty");
    }

    // Non-copyable
    intrusive_list(const intrusive_list&) = delete;
    intrusive_list& operator=(const intrusive_list&) = delete;

    // Movable
    intrusive_list(intrusive_list&& other) noexcept
        : head_(other.head_), tail_(other.tail_), size_(other.size_) {
        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.size_ = 0;
    }

    intrusive_list& operator=(intrusive_list&& other) noexcept {
        if (this != &other) {
            assert(empty() && "Cannot move-assign to a non-empty list");
            head_ = other.head_;
            tail_ = other.tail_;
            size_ = other.size_;
            other.head_ = nullptr;
            other.tail_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    /// Add node to the back of the list. O(1).
    void push_back(T* node) noexcept {
        assert(node != nullptr);
        assert(!node->in_list_ && "Node is already in a list");

        node->prev_ = tail_;
        node->next_ = nullptr;

        if (tail_) {
            tail_->next_ = node;
        } else {
            head_ = node;
        }

        tail_ = node;
        node->in_list_ = true;
        ++size_;
    }

    /// Add node to the front of the list. O(1).
    void push_front(T* node) noexcept {
        assert(node != nullptr);
        assert(!node->in_list_ && "Node is already in a list");

        node->prev_ = nullptr;
        node->next_ = head_;

        if (head_) {
            head_->prev_ = node;
        } else {
            tail_ = node;
        }

        head_ = node;
        node->in_list_ = true;
        ++size_;
    }

    /// Remove a specific node from the list. O(1).
    /// The node must be in THIS list (debug builds check is_linked()).
    void remove(T* node) noexcept {
        assert(node != nullptr);
        if (!node->in_list_) return;

        if (node->prev_) {
            node->prev_->next_ = node->next_;
        } else {
            head_ = node->next_;
        }

        if (node->next_) {
            node->next_->prev_ = node->prev_;
        } else {
            tail_ = node->prev_;
        }

        node->prev_ = nullptr;
        node->next_ = nullptr;
        node->in_list_ = false;
        --size_;
    }

    /// Remove and return the front node. Returns nullptr if empty. O(1).
    T* pop_front() noexcept {
        if (!head_) return nullptr;
        T* node = static_cast<T*>(head_);
        remove(node);
        return node;
    }

    /// Remove and return the back node. Returns nullptr if empty. O(1).
    T* pop_back() noexcept {
        if (!tail_) return nullptr;
        T* node = static_cast<T*>(tail_);
        remove(node);
        return node;
    }

    /// Access front without removing. Returns nullptr if empty.
    T* front() noexcept {
        return head_ ? static_cast<T*>(head_) : nullptr;
    }

    const T* front() const noexcept {
        return head_ ? static_cast<const T*>(head_) : nullptr;
    }

    /// Access back without removing. Returns nullptr if empty.
    T* back() noexcept {
        return tail_ ? static_cast<T*>(tail_) : nullptr;
    }

    const T* back() const noexcept {
        return tail_ ? static_cast<const T*>(tail_) : nullptr;
    }

    bool empty() const noexcept { return head_ == nullptr; }
    size_t size() const noexcept { return size_; }

    /// Check if a node is in this list. O(n) — use for debugging only.
    bool contains(const T* node) const noexcept {
        if (!node || !node->in_list_) return false;
        const intrusive_list_node<T>* current = head_;
        while (current) {
            if (current == node) return true;
            current = current->next_;
        }
        return false;
    }

    /// Remove all nodes from the list, marking each as unlinked.
    /// Nodes are NOT deleted — only unlinked.
    void clear() noexcept {
        while (head_) {
            pop_front();
        }
    }

private:
    intrusive_list_node<T>* head_ = nullptr;
    intrusive_list_node<T>* tail_ = nullptr;
    size_t size_ = 0;
};

} // namespace elio::detail
