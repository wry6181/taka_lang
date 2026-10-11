# 12. Foreign functions and the runtime

Everything taka cannot do itself — printing, files, argv, the process
exit code — arrives through `extern fn` declarations backed by
`taka_runtime.h`, a small header of `static inline` C over stdio,
stdlib, and string. There is no linking step to configure: generated C
`#include`s the runtime, and the native backend calls the same symbols.

## The complete runtime

| Declaration | What it does |
|---|---|
| `tk_print_i32(i32 x)` | print an `i32` + newline to stdout |
| `tk_print_str(char* s)` | print a NUL string + newline |
| `tk_malloc(u64 n) -> u8*` | `malloc` (opaque bytes) |
| `tk_free(u8* p)` | `free` |
| `tk_fopen(path, mode) -> u8*` | `fopen` (`FILE*` as `u8*`) |
| `tk_fclose(f) -> i32` | `fclose` |
| `tk_fseek(f, off, whence) -> i32` | `fseek` |
| `tk_ftell(f) -> i64` | `ftell` |
| `tk_fread(dst, sz, n, f) -> u64` | `fread` |
| `tk_fwrite(src, sz, n, f) -> u64` | `fwrite` |
| `tk_strlen(s) -> u64` | `strlen` |
| `tk_streq(a, b) -> bool` | `strcmp(a,b)==0` |
| `tk_argc() -> i32` | program argument count |
| `tk_argv(i32 i) -> char*` | argument `i`, or NULL out of range |
| `tk_system(char* cmd) -> i32` | `system()` |
| `tk_eprint(u8* p, u64 n)` | write bytes to stderr |
| `tk_set_exit(i32 c)` | set the process exit status |
| `tk_stdout() -> u8*` | `stdout` as `u8*` |
| `tk_strtod_bits(char* s) -> u64` | `strtod`, returned as `double` bits |
| `tk_sview(p, n) -> sview` | build a view from `char*` + length |
| `tk_sview_u8(p, n) -> sview` | build a view from `u8*` + length |

Plus the `TkSView` struct `{char* ptr; i64 len}` itself. Every entry
above is used by the repository's own tests and tools, so none of it
is aspirational.

## Calling your own C

Add your own `extern fn` declarations and compile the generated C
alongside your C files — `taka emit-c` (Chapter 13) gives you the C
to link. Match the signatures exactly: taka trusts `extern` blocks,
and a lie here is the one unsoundness the checker cannot catch. Pass
structs by pointer, keep ownership rules in comments, and prefer
returning error values over callbacks. (Closures as callbacks do not
exist yet — Chapter 14.)
