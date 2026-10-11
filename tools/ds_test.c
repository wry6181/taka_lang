// Tests + microbench for ds_list / ds_map / ds_set.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "ds_list.h"
#include "ds_map.h"
#include "ds_set.h"

VEC_DECLARE(IntVec, int);
VEC_DECLARE(PtrVec, void*);

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static void test_list(void) {
    IntVec v; vec_IntVec_init(&v);
    for (int i = 0; i < 1000; i++) CHECK(vec_IntVec_push_heap(&v, i) == 0, "list push");
    CHECK(vec_IntVec_len(&v) == 1000, "list len");
    for (int i = 0; i < 1000; i++) CHECK(*vec_IntVec_get(&v, (u64)i) == i, "list get");
    vec_IntVec_pop(&v); CHECK(vec_IntVec_len(&v) == 999, "list pop");
    vec_IntVec_clear(&v); CHECK(vec_IntVec_len(&v) == 0, "list clear");
    vec_IntVec_free_heap(&v);
    // arena variant
    mem_arena *a = arena_create(MByte(1));
    IntVec w; vec_IntVec_init(&w);
    for (int i = 0; i < 500; i++) CHECK(vec_IntVec_push_arena(&w, i, a) == 0, "arena push");
    CHECK(vec_IntVec_len(&w) == 500, "arena len");
    arena_destroy(a);
    printf("list OK\n");
}

static void test_strmap(void) {
    StrMap m; strmap_init_heap(&m);
    char key[64];
    for (int i = 0; i < 2000; i++) {
        snprintf(key, sizeof key, "sym_%d", i);
        CHECK(strmap_put(&m, key, (void*)(long)(i + 1)) == 0, "strmap put");
    }
    CHECK(strmap_len(&m) == 2000, "strmap len");
    for (int i = 0; i < 2000; i++) {
        snprintf(key, sizeof key, "sym_%d", i);
        void *v = NULL;
        CHECK(strmap_get(&m, key, &v) && (long)v == i + 1, "strmap get");
    }
    CHECK(!strmap_has(&m, "nope_missing"), "strmap miss");
    // update
    CHECK(strmap_put(&m, "sym_0", (void*)999) == 0, "strmap update");
    void *v = NULL; CHECK(strmap_get(&m, "sym_0", &v) && (long)v == 999, "strmap updated");
    CHECK(strmap_len(&m) == 2000, "strmap len after update");
    // remove + backward-shift integrity
    for (int i = 0; i < 1000; i++) { snprintf(key, sizeof key, "sym_%d", i); CHECK(strmap_remove(&m, key), "strmap remove"); }
    CHECK(strmap_len(&m) == 1000, "strmap len after remove");
    for (int i = 1000; i < 2000; i++) { snprintf(key, sizeof key, "sym_%d", i); CHECK(strmap_has(&m, key), "strmap survivor"); }
    for (int i = 0; i < 1000; i++) { snprintf(key, sizeof key, "sym_%d", i); CHECK(!strmap_has(&m, key), "strmap gone"); }
    strmap_free_heap(&m);
    // arena variant (stable keys, no free)
    mem_arena *a = arena_create(MByte(1));
    StrMap am; strmap_init_arena(&am, a);
    for (int i = 0; i < 500; i++) {
        char *k = (char*)arena_push(a, 32, 0); snprintf(k, 32, "a_%d", i);
        CHECK(strmap_put(&am, k, (void*)(long)i) == 0, "arena map put");
    }
    void *vv = NULL; CHECK(strmap_get(&am, "a_7", &vv) && (long)vv == 7, "arena map get");
    arena_destroy(a);
    printf("strmap OK\n");
}

static void test_intmap_set(void) {
    IntMap m; intmap_init_heap(&m);
    for (u64 i = 0; i < 2000; i++) CHECK(intmap_put(&m, i * 2654435761ULL, (void*)(long)i) == 0, "intmap put");
    for (u64 i = 0; i < 2000; i++) { void *v=NULL; CHECK(intmap_get(&m, i*2654435761ULL, &v) && (long)v == (long)i, "intmap get"); }
    CHECK(!intmap_has(&m, 0xdeadbeefULL), "intmap miss");
    for (u64 i = 0; i < 1000; i++) CHECK(intmap_remove(&m, i*2654435761ULL), "intmap remove");
    CHECK(intmap_len(&m) == 1000, "intmap len");
    intmap_free(&m);
    StrSet s; strset_init_heap(&s);
    CHECK(strset_add(&s, "red") == 0, "set add"); CHECK(strset_add(&s, "green") == 0, "set add");
    CHECK(strset_has(&s, "red") && !strset_has(&s, "blue"), "set has");
    CHECK(strset_remove(&s, "red") && !strset_has(&s, "red"), "set remove");
    strset_free_heap(&s);
    printf("intmap/set OK\n");
}

static void bench(void) {
    StrMap m; strmap_init_heap(&m);
    char key[64];
    clock_t t0 = clock();
    const int N = 50000;
    for (int i = 0; i < N; i++) { snprintf(key, sizeof key, "k_%d", i); strmap_put(&m, key, (void*)(long)i); }
    for (int i = 0; i < N; i++) { snprintf(key, sizeof key, "k_%d", i); void *v; strmap_get(&m, key, &v); }
    double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("bench: %d put+get in %.3fs (%.0f ops/s)\n", N, dt, (double)(2*N)/ (dt>0?dt:1));
    strmap_free_heap(&m);
}

int main(void) {
    test_list(); test_strmap(); test_intmap_set(); bench();
    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("ds ALL OK\n");
    return 0;
}
