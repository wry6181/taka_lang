#pragma once
// Minimal taka runtime (C stdlib only).
// Signatures must match `extern fn` decls in taka exactly:
// taka `u8*` = uint8_t*, `char*` = char*, `u64/i64/i32` = uint64_t/int64_t/int32_t.
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *ptr; int64_t len; } TkSView; // mutable: views over arena buffers

static inline void tk_print_i32(int32_t x) { printf("%d\n", (int)x); }
static inline void tk_print_str(char *s) { printf("%s\n", (const char*)s); }
static inline int tk_effect_stub(void) { printf("<effect-stub>\n"); return 0; }

// heap + file helpers for the taka-written arena/file layer (std/file_test.tk)
static inline uint8_t *tk_malloc(uint64_t n) { return (uint8_t*)malloc((size_t)n); }
static inline void tk_free(uint8_t *p) { free((void*)p); }
// FILE* is opaque in taka (spelled `u8*`); casts live here, not in taka code.
static inline uint8_t *tk_fopen(char *path, char *mode) { return (uint8_t*)fopen((const char*)path, (const char*)mode); }
static inline int32_t tk_fclose(uint8_t *f) { return (int32_t)fclose((FILE*)f); }
static inline int32_t tk_fseek(uint8_t *f, int64_t off, int32_t whence) { return (int32_t)fseek((FILE*)f, (long)off, (int)whence); }
static inline int64_t tk_ftell(uint8_t *f) { return (int64_t)ftell((FILE*)f); }
static inline uint64_t tk_fread(uint8_t *dst, uint64_t sz, uint64_t n, uint8_t *f) { return (uint64_t)fread((void*)dst, (size_t)sz, (size_t)n, (FILE*)f); }
static inline uint64_t tk_fwrite(uint8_t *src, uint64_t sz, uint64_t n, uint8_t *f) { return (uint64_t)fwrite((const void*)src, (size_t)sz, (size_t)n, (FILE*)f); }
static inline uint64_t tk_strlen(char *s) { return (uint64_t)strlen((const char*)s); }
static inline bool tk_streq(char *a, char *b) { return strcmp((const char*)a, (const char*)b) == 0; }
// process args (set by generated main; enables self-hosted CLIs)
static int tk_g_argc;
static char **tk_g_argv;
static inline int32_t tk_argc(void) { return (int32_t)tk_g_argc; }
static inline char *tk_argv(int32_t i) {
    if (i < 0 || i >= tk_g_argc) return NULL;
    return tk_g_argv[i];
}
// process spawn for self-hosted build/run (returns wait status like system)
static inline int32_t tk_system(char *cmd) { return (int32_t)system((const char*)cmd); }
// diagnostics + exit status (self-hosted compilers must report errors)
static inline void tk_eprint(uint8_t *p, uint64_t n) { fwrite((const void*)p, 1, (size_t)n, stderr); }
static int32_t tk_g_exit;
static inline void tk_set_exit(int32_t c) { tk_g_exit = c; }
static inline uint8_t *tk_stdout(void) { return (uint8_t*)stdout; }
// Non-inline (emitted standalone): too big for reliable inline expansion,
// and the native backend calls it through the PLT via this exact symbol.
// Parses decimal text exactly like strtod; returns the double bits.
uint64_t tk_strtod_bits(char *s) {
    double d = strtod((const char*)s, NULL);
    uint64_t b = 0;
    memcpy(&b, &d, 8);
    return b;
}
static inline TkSView tk_sview(char *p, uint64_t n) { TkSView v = {p, (int64_t)n}; return v; }
static inline TkSView tk_sview_u8(uint8_t *p, uint64_t n) { TkSView v = {(char*)p, (int64_t)n}; return v; }
