#ifndef VECTOR_H
#define VECTOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Note: struct Vector deliberately has a named tag (not an anonymous
// struct) so it can be forward-declared in other headers (see
// mrp_user/mrp_api.h which uses `struct Vector;` to expose Vector* to
// .mrp programs without exposing the internal layout to userland).
typedef struct Vector {
    void* data;          // pointer to the element array
    size_t elem_size;    // size of each element (bytes)
    size_t size;         // current number of elements
    size_t capacity;     // allocated capacity
} Vector;

// --- Lifecycle ---
Vector* vector_create(size_t elem_size);
void vector_free(Vector* v);

// --- Modification ---
int vector_push(Vector* v, const void* elem);
void vector_pop(Vector* v);
int vector_set(Vector* v, size_t index, const void* elem);
int vector_resize(Vector* v, size_t new_size);
void vector_clear(Vector* v);

// --- Access ---
void* vector_get(const Vector* v, size_t index);

// --- Information ---
size_t vector_size(const Vector* v);
int vector_empty(const Vector* v);

#ifdef __cplusplus
}
#endif

#endif
