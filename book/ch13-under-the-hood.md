# 13. Under the hood: backends and performance

## Two backends, one language

`--backend=c` (default) translates taka to readable C and lets `cc`
optimize it. `--backend=native` emits x86-64 assembly directly with no
C involved. The repository's gates build every test program with both
and diff the outputs — same language, same behavior, different
trade-offs:

- **C backend**: release-grade speed (below), portable wherever a C
  compiler exists, slower builds, needs `cc`.
- **Native backend**: fast builds, no C dependency at codegen time,
  currently unoptimized (8–500× slower on compute-heavy code).

Rule of thumb: develop with either, ship `--release` on the C
backend. Select with `--backend=` before the subcommand:
`taka --backend=native run prog.tk`.

## Reading the generated C

`taka emit-c` writes the translation for inspection or hand-tuning:

```text
$ ./taka emit-c addmini.tk -o addmini.c
parse OK
sema OK
wrote addmini.c
```

For `fn add(i32 a, i32 b) -> i32 { return a + b; }`, the body compiles
to (return-value temporary, defer drain, return — all visible, nothing
magic):

```c
int32_t add(int32_t a,int32_t b){
    __attribute__((unused)) void **__dbuf = NULL;
    __attribute__((unused)) size_t __dlen = 0, __dcap = 0;
    { __auto_type __rv = ((a+b)); while (__dlen) ((void(*)())__dbuf[--__dlen])(); free(__dbuf); return __rv; }
    ...
}
```

Types map one-to-one (`i32`→`int32_t`, `bool`→`bool`, structs→
`struct`s, `List<T>`→`{data,len,cap}`), and the file links against
`taka_runtime.h` with any C compiler. This directness is deliberate:
there is no runtime system to learn, and `perf`/`gdb` show you code
 shaped like your source.

## Measured performance

Same machine, same programs (recursive `fib(35)` and a 3×10⁸ loop
sum), best of runs:

| Benchmark | handwritten C `-O2` | taka → C `-O2` | taka native |
|---|---|---|---|
| fib(35) | 12ms | 12ms | 94ms |
| loop sum | ~1ms | ~1ms | 528ms |

taka→C is *identical* to handwritten C — the same optimizer sees
equivalent code. (This is the old Cfront strategy: early C++ compiled
to C for exactly this reason.) And the `--dev`/`--release` lever is
real: the same `fib` builds at 134ms dev vs 12ms release.

## Self-hosting

The compiler compiles itself in stages, each checked byte-for-byte:
`self_lex` (lexer parity), `self_parse` (parser + checker parity),
`self_emit` (assembly parity), closed by `make selfhost`, which builds
a binary using *only* self-hosted stages and runs it. If you change
the language, these gates tell you which stage drifted.
