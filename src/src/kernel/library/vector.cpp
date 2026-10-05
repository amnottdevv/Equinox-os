#include "header/vector.h"
#include "header/malloc.h"
#include "header/libstring.h"  // for memcpy
#include <stddef.h>
#include <stdint.h>

#define VECTOR_INIT_CAPACITY 4

// ============================================================
//  vector_create
// ============================================================
Vector* vector_create(size_t elem_size) {
    if (elem_size == 0) return NULL;

    Vector* v = (Vector*)malloc(sizeof(Vector));
    if (!v) return NULL;

    v->elem_size = elem_size;
    v->size = 0;
    v->capacity = VECTOR_INIT_CAPACITY;
    v->data = malloc(elem_size * v->capacity);

    if (!v->data) {
        free(v);
        return NULL;
    }

    return v;
}

// ============================================================
//  vector_free
// ============================================================
void vector_free(Vector* v) {
    if (!v) return;
    if (v->data) free(v->data);
    free(v);
}

// ============================================================
//  vector_push
// ============================================================
int vector_push(Vector* v, const void* elem) {
    if (!v || !elem) return -1;

    // If capacity is full, double it
    if (v->size >= v->capacity) {
        size_t new_cap = v->capacity * 2;
        void* new_data = malloc(v->elem_size * new_cap);
        if (!new_data) return -1;

        memcpy(new_data, v->data, v->size * v->elem_size);
        free(v->data);
        v->data = new_data;
        v->capacity = new_cap;
    }

    // Copy the element to the end
    char* dest = (char*)v->data + (v->size * v->elem_size);
    memcpy(dest, elem, v->elem_size);
    v->size++;

    return 0;
}

// ============================================================
//  vector_pop
// ============================================================
void vector_pop(Vector* v) {
    if (!v || v->size == 0) return;
    v->size--;
}

// ============================================================
//  vector_get
// ============================================================
void* vector_get(const Vector* v, size_t index) {
    if (!v || index >= v->size) return NULL;
    return (char*)v->data + (index * v->elem_size);
}

// ============================================================
//  vector_set
// ============================================================
int vector_set(Vector* v, size_t index, const void* elem) {
    if (!v || !elem || index >= v->size) return -1;

    char* dest = (char*)v->data + (index * v->elem_size);
    memcpy(dest, elem, v->elem_size);
    return 0;
}

// ============================================================
//  vector_resize
// ============================================================
int vector_resize(Vector* v, size_t new_size) {
    if (!v) return -1;
    if (new_size == v->size) return 0;

    if (new_size > v->capacity) {
        // Need bigger capacity
        size_t new_cap = new_size;
        // Align to a power of two (optional)
        while (new_cap < new_size) new_cap *= 2;

        void* new_data = malloc(v->elem_size * new_cap);
        if (!new_data) return -1;

        memcpy(new_data, v->data, v->size * v->elem_size);
        free(v->data);
        v->data = new_data;
        v->capacity = new_cap;
    }

    // If new_size is smaller, no zero-fill needed (just leave it)
    v->size = new_size;
    return 0;
}

// ============================================================
//  vector_clear
// ============================================================
void vector_clear(Vector* v) {
    if (!v) return;
    v->size = 0;
}

// ============================================================
//  vector_size
// ============================================================
size_t vector_size(const Vector* v) {
    return v ? v->size : 0;
}

// ============================================================
//  vector_empty
// ============================================================
int vector_empty(const Vector* v) {
    return v ? v->size == 0 : 1;
}
