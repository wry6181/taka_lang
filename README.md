# my_language (taka)

A small, compiled, self-hosting systems language. Start with
`book/` (the language guide) or `make taka && ./taka help`.

## Layout

| Directory | Contents |
|---|---|
| `src/` | compiler C sources: lexer, parser, builder, checker, C + x86-64 backends |
| `runtime/` | `taka_runtime.h`, the C runtime linked into generated code |
| `tests/` | `.tk` corpus: behavior tests, negative fixtures (`bad_*`), `syntax_test.tk` |
| `selfhost/` | self-hosting stages: `self_lex`, `self_parse`, `self_emit` (taka in taka) |
| `tools/` | developer tools: `lexdump`, `ds_test`, `gen_*.py` kind-table generators |
| `docs/` | `grammar.ebnf`, the language grammar + design rules |
| `book/` | the language book (verified examples; `book/check.sh` re-checks them) |
| `editors/` | Zed extension + Tree-sitter grammar |

Build products (`taka`, test binaries, `*.gen.c`, `*.s`, fixtures) are
never committed — `make clean` removes them, `.gitignore` excludes them.
Run everything from the repo root: `make test test-native …`.
