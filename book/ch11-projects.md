# 11. Projects: files, args, testing

## Reading and writing files

Files are `u8*`-disguised `FILE*` handles with direct C semantics.
This program writes two bytes and reports success:

```taka
extern fn tk_print_i32(i32 x);
extern fn tk_fopen(char* path, char* mode) -> u8*;
extern fn tk_fwrite(u8* src, u64 sz, u64 n, u8* f) -> u64;
extern fn tk_fclose(u8* f) -> i32;

fn main() {
    u8* f = tk_fopen("/tmp/book_out.txt", "w");
    if (f == null) { tk_print_i32(0 - 1); return; }
    tk_fwrite("hi" as u8*, 1, 2, f);
    tk_fclose(f);
    tk_print_i32(1);
}
```

```text
1
```

The full set — `tk_fopen/fclose/fseek/ftell/fread/fwrite` — mirrors
stdio one-to-one (signatures in Chapter 12). Errors are values: a null
handle, a short count. The repository's `tests/file_test.tk` shows the read
path (`fseek` to end, `ftell` for size, one `fread`).

## Command-line arguments

```taka,ignore
extern fn tk_argc() -> i32;
extern fn tk_argv(i32 i) -> char*;

fn main() {
    // tk_argv(i) is NULL past either end — always bounds-check.
    char* name = tk_argv(1);
    ...
}
```

`tk_argc()` is the count, `tk_argv(i)` the i-th argument or NULL out
of range (the driver fills these from the real `argc`/`argv`). The
repository's `tests/args_test.tk` is the canonical example:

```text
$ ./taka run tests/args_test.tk hello
2
1
1
0
```

## Testing with `taka test`

List the programs that must pass; the driver builds, runs, and reports
each one, then a summary, exiting 1 on any failure:

```text
$ ./taka test hello.tk vars.tk
test hello.tk ... Hello, taka!
ok
test vars.tk ... 1
3
42
ok
test result: 2 passed; 0 failed
```

(Program output streams inline, à la `cargo test -- --nocapture`.)
A failing file — a sema error, a crash, a nonzero exit — prints
`FAILED (exit N)` and counts against the summary. Keep a `tests/`
folder of small behavior programs like the ones in this book, run
`taka test tests/*.tk` before every commit, and you have CI-friendly
coverage with zero framework. The compiler's own gates do exactly this
at larger scale (`make test-native` diffs both backends' outputs).
