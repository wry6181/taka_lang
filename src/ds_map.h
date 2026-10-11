#pragma once
// Fast hash maps — SwissTable/khash/robin_hood-inspired best practice:
// open addressing, power-of-two, Robin Hood probing, load <= 0.7,
// flat entries (no per-node malloc), stored hashes (no rehash on resize),
// backward-shift delete (no tombstones), FNV-1a (str) / splitmix64 (u64).
// Two alloc policies: heap (strdup/free keys) and arena (stable keys, bump grow).
// Small tables (<~32 entries): prefer linear scan — hashing wins only when large.
#include "types.h"
#include "arena.h"
#include <stdlib.h>
#include <string.h>

static inline u64 ds_hash_str(const char *s) {
    u64 h = 1469598103934665603ULL; // FNV-1a 64 offset
    for (const unsigned char *p = (const unsigned char*)s; *p; p++) {
        h ^= (u64)*p;
        h *= 1099511628211ULL;
    }
    h ^= h >> 29; h *= 0xbf58476d1ce4e5b9ULL; h ^= h >> 32;
    return h ? h : 1;
}
static inline u64 ds_hash_u64(u64 x) {
    x += 0x9e3779b97f4a7c15ULL; x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL; x ^= x >> 31;
    return x ? x : 1;
}
static inline u64 ds_next_pow2(u64 n) { u64 p = 16; while (p < n) p *= 2; return p; }

// ---------- StrMap: const char* -> void* ----------
typedef struct { const char *key; u64 hash; void *val; u8 dist, used; } DsStrEntry;
typedef struct { DsStrEntry *tab; u64 cap, mask, len; mem_arena *arena; b32 own_keys; } StrMap;

static inline void strmap_init_heap(StrMap *m) { m->tab=NULL; m->cap=m->mask=m->len=0; m->arena=NULL; m->own_keys=1; }
static inline void strmap_init_arena(StrMap *m, mem_arena *a) { m->tab=NULL; m->cap=m->mask=m->len=0; m->arena=a; m->own_keys=0; }
static inline u64 strmap_len(const StrMap *m) { return m->len; }

static inline int strmap_grow(StrMap *m, u64 ncap) {
    ncap = ds_next_pow2(ncap);
    DsStrEntry *ntab = m->arena
        ? (DsStrEntry*)arena_push(m->arena, ncap * sizeof(DsStrEntry), 1)
        : (DsStrEntry*)calloc((size_t)ncap, sizeof(DsStrEntry));
    if (!ntab) return -1;
    DsStrEntry *otab = m->tab; u64 ocap = m->cap;
    m->tab = ntab; m->cap = ncap; m->mask = ncap - 1; m->len = 0;
    for (u64 i = 0; i < ocap; i++) if (otab[i].used) {
        u64 h = otab[i].hash, idx = h & m->mask; u8 dist = 0;
        const char *k = otab[i].key; void *v = otab[i].val;
        for (;;) {
            DsStrEntry *e = &m->tab[idx];
            if (!e->used) { e->used=1; e->dist=dist; e->hash=h; e->key=k; e->val=v; m->len++; break; }
            if (e->dist < dist) {
                const char *tk=e->key; u64 th=e->hash; void *tv=e->val; u8 td=e->dist;
                e->key=k; e->hash=h; e->val=v; e->dist=dist;
                k=tk; h=th; v=tv; dist=td;
            }
            idx = (idx+1) & m->mask; dist++;
        }
    }
    if (!m->arena) free(otab); // heap keys moved, not freed
    return 0;
}

// Insert or update. Heap: dups the key once per distinct key (freed on update-path / remove / destroy).
// Arena: key pointer stored as-is (caller must keep it stable, e.g. arena string).
static inline int strmap_put(StrMap *m, const char *key, void *val) {
    u64 h = ds_hash_str(key);
    if ((m->len + 1) * 10 > m->cap * 7) {
        if (strmap_grow(m, m->cap ? m->cap * 2 : 16) != 0) return -1;
    }
    const char *k = key;
    if (m->own_keys) {
        size_t n = strlen(key)+1; char *c = (char*)malloc(n);
        if (!c) return -1;
        memcpy(c, key, n); k = c;
    }
    u64 idx = h & m->mask; u8 dist = 0;
    void *v = val;
    for (;;) {
        DsStrEntry *e = &m->tab[idx];
        if (!e->used) { e->used=1; e->dist=dist; e->hash=h; e->key=k; e->val=v; m->len++; return 0; }
        if (e->hash == h && strcmp(e->key, k) == 0) {
            if (m->own_keys) free((void*)k); // dup not needed: key already stored
            e->val = v; return 0;
        }
        if (e->dist < dist) { // Robin Hood swap: ownership moves with the tuple
            const char *tk=e->key; u64 th=e->hash; void *tv=e->val; u8 td=e->dist;
            e->key=k; e->hash=h; e->val=v; e->dist=dist;
            k=tk; h=th; v=tv; dist=td;
        }
        idx = (idx+1) & m->mask; dist++;
    }
}

static inline int strmap_get(StrMap *m, const char *key, void **out) {
    if (!m->cap) return 0;
    u64 h = ds_hash_str(key);
    u64 idx = h & m->mask; u8 dist = 0;
    for (;;) {
        DsStrEntry *e = &m->tab[idx];
        if (!e->used || e->dist < dist) return 0;
        if (e->hash == h && strcmp(e->key, key) == 0) { if (out) *out = e->val; return 1; }
        idx = (idx+1) & m->mask; dist++;
    }
}
static inline int strmap_has(StrMap *m, const char *key) { return strmap_get(m, key, NULL); }

// Backward-shift delete (Knuth 6.4R): keeps probe chains intact, no tombstones.
static inline int strmap_remove(StrMap *m, const char *key) {
    if (!m->cap) return 0;
    u64 h = ds_hash_str(key);
    u64 idx = h & m->mask; u8 dist = 0;
    for (;;) {
        DsStrEntry *e = &m->tab[idx];
        if (!e->used || e->dist < dist) return 0;
        if (e->hash == h && strcmp(e->key, key) == 0) {
            if (m->own_keys) free((void*)e->key);
            e->used = 0; m->len--;
            // shift followers backward while they are displaced (dist > 0)
            u64 j = idx;
            for (;;) {
                u64 nxt = (j + 1) & m->mask;
                DsStrEntry *n = &m->tab[nxt];
                if (!n->used || n->dist == 0) break;
                m->tab[j] = *n; m->tab[j].dist--;
                j = nxt;
            }
            m->tab[j].used = 0;
            return 1;
        }
        idx = (idx+1) & m->mask; dist++;
    }
}
static inline void strmap_free_heap(StrMap *m) {
    if (m->arena) { m->tab=NULL; m->cap=m->mask=m->len=0; return; } // bump: arena_destroy frees all
    if (m->tab) for (u64 i=0;i<m->cap;i++) if (m->tab[i].used) free((void*)m->tab[i].key);
    free(m->tab); m->tab=NULL; m->cap=m->mask=m->len=0;
}

// ---------- IntMap: u64 -> void* ----------
typedef struct { u64 key; u64 hash; void *val; u8 dist, used; } DsIntEntry;
typedef struct { DsIntEntry *tab; u64 cap, mask, len; mem_arena *arena; } IntMap;

static inline void intmap_init_heap(IntMap *m) { m->tab=NULL; m->cap=m->mask=m->len=0; m->arena=NULL; }
static inline void intmap_init_arena(IntMap *m, mem_arena *a) { m->tab=NULL; m->cap=m->mask=m->len=0; m->arena=a; }
static inline u64 intmap_len(const IntMap *m) { return m->len; }

static inline int intmap_grow(IntMap *m, u64 ncap) {
    ncap = ds_next_pow2(ncap);
    DsIntEntry *ntab = m->arena
        ? (DsIntEntry*)arena_push(m->arena, ncap * sizeof(DsIntEntry), 1)
        : (DsIntEntry*)calloc((size_t)ncap, sizeof(DsIntEntry));
    if (!ntab) return -1;
    DsIntEntry *otab = m->tab; u64 ocap = m->cap;
    m->tab = ntab; m->cap = ncap; m->mask = ncap - 1; m->len = 0;
    for (u64 i = 0; i < ocap; i++) if (otab[i].used) {
        u64 h = otab[i].hash, idx = h & m->mask; u8 dist = 0;
        u64 k = otab[i].key; void *v = otab[i].val;
        for (;;) {
            DsIntEntry *e = &m->tab[idx];
            if (!e->used) { e->used=1; e->dist=dist; e->hash=h; e->key=k; e->val=v; m->len++; break; }
            if (e->dist < dist) {
                u64 tk=e->key; u64 th=e->hash; void *tv=e->val; u8 td=e->dist;
                e->key=k; e->hash=h; e->val=v; e->dist=dist;
                k=tk; h=th; v=tv; dist=td;
            }
            idx = (idx+1) & m->mask; dist++;
        }
    }
    if (!m->arena) free(otab);
    return 0;
}

static inline int intmap_put(IntMap *m, u64 key, void *val) {
    u64 h = ds_hash_u64(key);
    if ((m->len + 1) * 10 > m->cap * 7) {
        if (intmap_grow(m, m->cap ? m->cap * 2 : 16) != 0) return -1;
    }
    u64 idx = h & m->mask; u8 dist = 0;
    u64 k = key; void *v = val;
    for (;;) {
        DsIntEntry *e = &m->tab[idx];
        if (!e->used) { e->used=1; e->dist=dist; e->hash=h; e->key=k; e->val=v; m->len++; return 0; }
        if (e->key == k) { e->val = v; return 0; }
        if (e->dist < dist) {
            u64 tk=e->key; u64 th=e->hash; void *tv=e->val; u8 td=e->dist;
            e->key=k; e->hash=h; e->val=v; e->dist=dist;
            k=tk; h=th; v=tv; dist=td;
        }
        idx = (idx+1) & m->mask; dist++;
    }
}
static inline int intmap_get(IntMap *m, u64 key, void **out) {
    if (!m->cap) return 0;
    u64 h = ds_hash_u64(key);
    u64 idx = h & m->mask; u8 dist = 0;
    for (;;) {
        DsIntEntry *e = &m->tab[idx];
        if (!e->used || e->dist < dist) return 0;
        if (e->key == key) { if (out) *out = e->val; return 1; }
        idx = (idx+1) & m->mask; dist++;
    }
}
static inline int intmap_has(IntMap *m, u64 key) { return intmap_get(m, key, NULL); }
static inline int intmap_remove(IntMap *m, u64 key) {
    if (!m->cap) return 0;
    u64 h = ds_hash_u64(key);
    u64 idx = h & m->mask; u8 dist = 0;
    for (;;) {
        DsIntEntry *e = &m->tab[idx];
        if (!e->used || e->dist < dist) return 0;
        if (e->key == key) {
            e->used = 0; m->len--;
            u64 j = idx;
            for (;;) {
                u64 nxt = (j + 1) & m->mask;
                DsIntEntry *n = &m->tab[nxt];
                if (!n->used || n->dist == 0) break;
                m->tab[j] = *n; m->tab[j].dist--;
                j = nxt;
            }
            m->tab[j].used = 0;
            return 1;
        }
        idx = (idx+1) & m->mask; dist++;
    }
}
static inline void intmap_free(IntMap *m) {
    if (m->arena) { m->tab=NULL; m->cap=m->mask=m->len=0; return; }
    free(m->tab); m->tab=NULL; m->cap=m->mask=m->len=0;
}
