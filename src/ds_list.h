#pragma once
// Fast typed dynamic arrays (List) — stb_ds/kvec-inspired best practice:
// contiguous exponential-growth (init 8, 2x), O(1) amortized push,
// cache-friendly, inline + restrict, heap (malloc/realloc) and arena (bump) variants.
// Usage:
//   VEC_DECLARE(PointList, Point)  // typed list of Point (by value)
//   VEC_DECLARE(PtrList, void*)    // pointer list (compiler AST nodes)
//   PointList xs; vec_PointList_init(&xs);
//   vec_PointList_push_heap(&xs, p);      // malloc-backed
//   vec_PointList_push_arena(&xs, p, arena); // bump-backed (no free; grow copies)
// Keep N small in hot paths: linear scan beats hashing under ~32 elems.
#include "types.h"
#include "arena.h"
#include <stdlib.h>
#include <string.h>

#define VEC_DECLARE(Name, T)                                                     \
typedef struct { T *data; u64 len, cap; } Name;                                   \
static inline void vec_##Name##_init(Name *v) { v->data = NULL; v->len = 0; v->cap = 0; } \
static inline u64 vec_##Name##_len(const Name *v) { return v->len; }              \
static inline T *vec_##Name##_get(Name *v, u64 i) { return &v->data[i]; }         \
static inline int vec_##Name##_reserve_heap(Name *v, u64 ncap) {                  \
    if (ncap <= v->cap) return 0;                                                \
    T *nd = (T*)realloc(v->data, (size_t)(ncap * sizeof(T)));                     \
    if (!nd) return -1;                                                          \
    v->data = nd; v->cap = ncap; return 0;                                       \
}                                                                                \
static inline int vec_##Name##_push_heap(Name *v, T val) {                       \
    if (v->len == v->cap) {                                                      \
        u64 nc = v->cap ? v->cap * 2 : 8;                                        \
        if (vec_##Name##_reserve_heap(v, nc) != 0) return -1;                     \
    }                                                                            \
    v->data[v->len++] = val; return 0;                                           \
}                                                                                \
static inline int vec_##Name##_reserve_arena(Name *v, u64 ncap, mem_arena *a) {   \
    if (ncap <= v->cap) return 0;                                                \
    T *nd = (T*)arena_push(a, ncap * sizeof(T), 0);                               \
    if (!nd) return -1;                                                          \
    if (v->data && v->len) memcpy(nd, v->data, (size_t)(v->len * sizeof(T)));     \
    v->data = nd; v->cap = ncap; return 0;                                       \
}                                                                                \
static inline int vec_##Name##_push_arena(Name *v, T val, mem_arena *a) {         \
    if (v->len == v->cap) {                                                      \
        u64 nc = v->cap ? v->cap * 2 : 8;                                        \
        if (vec_##Name##_reserve_arena(v, nc, a) != 0) return -1;                 \
    }                                                                            \
    v->data[v->len++] = val; return 0;                                           \
}                                                                                \
static inline void vec_##Name##_pop(Name *v) { if (v->len) v->len--; }            \
static inline void vec_##Name##_clear(Name *v) { v->len = 0; }                    \
static inline void vec_##Name##_free_heap(Name *v) { free(v->data); v->data=NULL; v->len=v->cap=0; }
