# 1. Getting started

## Building taka

You need a C compiler (`cc`) and `make`. Then:

```text
$ git clone <taka-repo> && cd taka
$ make taka
cc -Wall -Wextra -g lexer.c parser.c arena.c ast_build.c sema.c cgen.c x64.c -o taka
```

That single `taka` binary is the whole toolchain: lexer, parser,
checker, both backends, and the cargo-like driver.

## Hello, taka

Save this as `hello.tk`:

```taka
extern fn tk_print_str(char* s);

fn main() {
    tk_print_str("Hello, taka!");
}
```

Two things to notice. Printing is not a keyword — it is an `extern fn`
from the C runtime (Chapter 12 lists them all). And `fn main()` takes
no arguments and returns nothing; the driver wraps it into a real C
`main` for you.

Build and run it:

```text
$ ./taka build hello.tk -o hello
built hello
$ ./hello
Hello, taka!
```

Or do both steps at once with `run`:

```text
$ ./taka run hello.tk
Hello, taka!
```

## The driver: cargo-like commands

`./taka help` prints the full reference (also in the Appendix), but the
daily commands mirror cargo:

| Command | What it does |
|---|---|
| `taka build file.tk [-o out]` | compile to a native binary |
| `taka run file.tk [args..]` | build to a temp dir and execute |
| `taka test a.tk [b.tk …]` | build+run each file, report `ok`/`FAILED` |
| `taka check file.tk` | typecheck only, like `cargo check` |
| `taka emit-c file.tk [-o f.c]` | write the generated C for inspection |
| `taka sexp file.tk` | dump the canonical AST (compiler debugging) |
| `taka file.tk` | parse, check, emit `file.gen.c` / `file.s` |

Flags go *before* the subcommand:

```text
$ ./taka test hello.tk vars.tk
test hello.tk ... ok
test vars.tk ... ok
test result: 2 passed; 0 failed
```

```text
$ ./taka check hello.tk
parse OK
```

- `--backend=c|native` selects the C backend (default) or the direct
  x86-64 backend. Chapter 13 explains when to use which.
- `--release` optimizes the C build (`-O2`); the default is `--dev`
  (`-O0`, faster builds). Measured on a recursive `fib(35)`: 134ms
  dev vs 12ms release.
- `taka check` on a bad program fails loudly with exit code 1:

```text
$ ./taka check bad_mut.tk
parse OK
sema error: mutation of non-mut binding 'a'
```

(Chapter 8 tours every safety rule and its message.)

## Troubleshooting

- A file whose name ends in `mut.tk` (like `bad_mut.tk`) takes a legacy
  code path in bare `taka file.tk` mode and may report a confusing
  parse error. Use `taka check` / `taka build` / `taka run`, which all
  use the real pipeline.
- `taka run` forwards program arguments after the filename:
  `taka run prog.tk hello` (or `taka run prog.tk -- --weird-arg`).
