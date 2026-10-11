# Introduction

Taka is a small systems programming language. It looks like C, borrows
enums-with-data from Rust, and compiles to native binaries through two
backends: readable C (optimized by your C compiler) or direct x86-64
machine code. The compiler itself is written in taka — it typechecks and
compiles its own source.

## What taka is for

Writing fast, explicit programs with no hidden costs: command-line tools,
file crunchers, data-structure experiments, runtimes for other languages.
If you like knowing exactly what the machine will do, you will feel at
home.

## Design rules

The grammar file (`docs/grammar.ebnf`) states what taka deliberately does
*not* have, and these rules shape everything in this book:

- **No overloading, no implicit conversions** (use `as`), no operator
  overloading, no inheritance.
- **No exceptions.** Errors are values: nullable pointers, booleans,
  enums you define yourself.
- **No textual macros, no headers.** One module per file for now.
- **No hidden allocation.** Heap use is explicit (`List.push`,
  `tk_malloc`, `defer` bookkeeping) — the language never allocates
  behind your back.
- **Left-to-right evaluation everywhere**, one way to do each thing:
  one declaration syntax, one cast syntax, one `for` loop shape.

## What taka is not (yet)

Taka is young. There are no closures, no traits, no effects runtime, no
modules — Chapter 14 lists exactly what parses but does not build yet,
so you always know where the edge is. Everything else in this book
works today and is tested by the repository's gate suite
(`make test test-native …`), which builds and runs every example
through *both* backends and diffs the results.

## How to read this book

Chapters 1–7 teach the core language. Chapters 8–10 cover the standard
patterns (safety rules, `List`, strings). Chapters 11–12 build real
programs. Chapter 13 opens the hood. Start at Chapter 1 with a working
build (five minutes) and go in order — or jump to any chapter; each
one's programs stand alone.
