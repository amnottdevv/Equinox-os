#ifndef TVG_PORT_H
#define TVG_PORT_H
/* tvgPort.h — compatibility layer ThorVG di kernel Equinox.
 * D included dari tvgCommon.h (semua file tvg kena). */
#include <stdint.h>
#include <stddef.h>
#include <math2.h>       /* math float/double kernel (library/header) */
#include <string.h>      /* libc kernel: memcpy/memset/strlen */
#include <ctype.h>       /* libc kernel: isspace/isdigit/tolower */

#define TVG_MIN(a, b) ((a) < (b) ? (a) : (b))
#define TVG_MAX(a, b) ((a) > (b) ? (a) : (b))

/* pengganti std::swap (kernel: -fno-exceptions, tanpa libstdc++) */
template <typename T>
static inline void tvgSwap(T& a, T& b) { T t = a; a = b; b = t; }

#include <libstring.h>  /* snprintf dsb. (libc kernel) */

namespace tvg {

/* tvgPort: pengganti std::list<T> — doubly-linked list minimal.
 * Metode yang dipakai subset: push_back, remove, clear, empty, size,
 * front, begin, end + iterator (==, !=, ++, *, ->). */
template <typename T>
class list {
    struct node {
        T v;
        node* prev;
        node* next;
        explicit node(const T& _v) : v(_v), prev(nullptr), next(nullptr) {}
    };
    node*  _head = nullptr;
    node*  _tail = nullptr;
    size_t _size = 0;

public:
    list() = default;
    ~list() { clear(); }
    list(const list&) = delete;
    list& operator=(const list&) = delete;

    void push_back(const T& v) {
        node* n = new node(v);
        if (!_tail) { _head = _tail = n; }
        else { n->prev = _tail; _tail->next = n; _tail = n; }
        _size++;
    }

    void remove(const T& v) {
        node* n = _head;
        while (n) {
            if (n->v == v) {
                if (n->prev) n->prev->next = n->next; else _head = n->next;
                if (n->next) n->next->prev = n->prev; else _tail = n->prev;
                delete n;
                _size--;
                return;
            }
            n = n->next;
        }
    }

    void clear() {
        node* n = _head;
        while (n) { node* nx = n->next; delete n; n = nx; }
        _head = _tail = nullptr;
        _size = 0;
    }

    size_t size() const { return _size; }
    bool   empty() const { return _size == 0; }
    T&     front() { return _head->v; }

    class iterator {
    public:
        node* n = nullptr;
        iterator() = default;
        explicit iterator(node* _n) : n(_n) {}
        T& operator*() { return n->v; }
        T* operator->() { return &n->v; }
        iterator& operator++() { n = n->next; return *this; }
        iterator operator++(int) { iterator t(*this); n = n->next; return t; }
        bool operator!=(const iterator& o) const { return n != o.n; }
        bool operator==(const iterator& o) const { return n == o.n; }
    };

    iterator begin() { return iterator(_head); }
    iterator end() { return iterator(nullptr); }

    /* hapus node di pos; return iterator ke node BERIKUTNYA */
    iterator erase(iterator pos) {
        node* n = pos.n;
        if (!n) return end();
        node* nx = n->next;
        if (n->prev) n->prev->next = n->next; else _head = n->next;
        if (n->next) n->next->prev = n->prev; else _tail = n->prev;
        delete n;
        _size--;
        return iterator(nx);
    }

    /* sisipkan v SEBELUM pos */
    iterator insert(iterator pos, const T& v) {
        node* n = new node(v);
        node* at = pos.n;
        if (!at) {                 /* insert di akhir */
            if (!_tail) { _head = _tail = n; }
            else { n->prev = _tail; _tail->next = n; _tail = n; }
        } else {
            n->next = at;
            n->prev = at->prev;
            if (at->prev) at->prev->next = n; else _head = n;
            at->prev = n;
        }
        _size++;
        return iterator(n);
    }
};

/* tvgPort: pengganti std::lower_bound / std::upper_bound (untuk
 * Array<T> SwRaster — iterator-nya pointer mentah). */
template <typename It, typename T, typename Comp>
static inline It lower_bound(It first, It last, const T& val, Comp comp) {
    ptrdiff_t len = last - first;
    while (len > 0) {
        ptrdiff_t half = len / 2;
        It mid = first + half;
        if (comp(*mid, val)) { first = mid + 1; len -= half + 1; }
        else len = half;
    }
    return first;
}

template <typename It, typename T, typename Comp>
static inline It upper_bound(It first, It last, const T& val, Comp comp) {
    ptrdiff_t len = last - first;
    while (len > 0) {
        ptrdiff_t half = len / 2;
        It mid = first + half;
        if (comp(val, *mid)) len = half;
        else { first = mid + 1; len -= half + 1; }
    }
    return first;
}

} /* namespace tvg */

#endif /* TVG_PORT_H */
