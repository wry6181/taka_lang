#pragma once
// Sets as maps with unit values (same Robin Hood table, no value overhead).
// StrSet: string membership (compiler keywords, field names, variant names).
// IntSet: u64 membership (ids, tags).
#include "ds_map.h"

typedef struct { StrMap m; } StrSet;
typedef struct { IntMap m; } IntSet;

static inline void strset_init_heap(StrSet *s) { strmap_init_heap(&s->m); }
static inline void strset_init_arena(StrSet *s, mem_arena *a) { strmap_init_arena(&s->m, a); }
static inline int strset_add(StrSet *s, const char *key) { return strmap_put(&s->m, key, (void*)1); }
static inline int strset_has(StrSet *s, const char *key) { return strmap_has(&s->m, key); }
static inline int strset_remove(StrSet *s, const char *key) { return strmap_remove(&s->m, key); }
static inline u64 strset_len(const StrSet *s) { return strmap_len(&s->m); }
static inline void strset_free_heap(StrSet *s) { strmap_free_heap(&s->m); }

static inline void intset_init_heap(IntSet *s) { intmap_init_heap(&s->m); }
static inline void intset_init_arena(IntSet *s, mem_arena *a) { intmap_init_arena(&s->m, a); }
static inline int intset_add(IntSet *s, u64 key) { return intmap_put(&s->m, key, (void*)1); }
static inline int intset_has(IntSet *s, u64 key) { return intmap_has(&s->m, key); }
static inline int intset_remove(IntSet *s, u64 key) { return intmap_remove(&s->m, key); }
static inline u64 intset_len(const IntSet *s) { return intmap_len(&s->m); }
static inline void intset_free(IntSet *s) { intmap_free(&s->m); }
