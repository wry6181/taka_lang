# 14. Not yet: the roadmap

Taka is honest about its edges. This is the complete list of things
that *parse* (the grammar and validator accept them, so syntax
highlighters and the `sexp` dump handle them) but do **not build** —
each is a stub, a `TODO`, or a deliberate rejection. If you hit one,
you will get `core backend: TODO (…) — validation OK` or `cannot
build this file`, never silent wrong code.

## Highest priority

- **Lambdas and closures.** Syntax exists (`[k](i32 x) => x + k`,
  `fn(T) -> U` types); backends emit non-escaping stubs only. No
  captures, no closure values yet. This is the biggest missing piece.
- **Traits and static methods.** `trait`/`impl` parse; vtables do not
  exist. Methods (`obj.method()`) fail in the native backend; only
  `xs.push(v)` is special-cased.
- **`dyn` trait objects.** `dyn Trait*` parses to `void*`; dispatch
  does not exist.

## Further out

- **Effects and handlers** (`effect`, `perform`, `handle`, `resume`):
  checked conservatively, emitted as stubs that print
  `<effect-stub>`. No handler runtime, no resume semantics.
- **Modules and imports.** `import foo::bar;` parses and is silently
  dropped by codegen. One file per program for now.
- **`for (v in xs)` loops.** Parses; the C backend runs the body once
  without iterating, native errors. Write `while` + index.
- **Slices, tuples-as-values, `[x; N]` fills, `Map<K,V>`, generic
  functions, `sizeof`/`offsetof` in anger, labeled `break`.**
  Reserved or stubbed; the corpus avoids them and so should you.

## How to read "validation OK"

When `taka file.tk` prints `core backend: TODO (…) — validation OK`,
it means: *your syntax is fine, the feature isn't implemented.* Treat
it as a compile error with a roadmap attached, and check this chapter
before restructuring your program — the limitation is usually real
and usually listed here.
