# The Taka Language Book

A Rust-book-style guide to taka: a small, compiled, self-hosting systems
language. Every program in this book compiles with the `taka` driver from
this repository; every shown output was produced by running it.

## User guide

- [Introduction](ch00-introduction.md)
- [1. Getting started](ch01-getting-started.md)
- [2. Variables and mutability](ch02-variables.md)
- [3. Basic types and operators](ch03-types.md)
- [4. Functions](ch04-functions.md)
- [5. Control flow](ch05-control-flow.md)
- [6. Structs and enums](ch06-structs-enums.md)
- [7. Pointers and memory](ch07-pointers-memory.md)
- [8. Safety rules](ch08-safety.md)
- [9. Generics and `List`](ch09-generics-list.md)
- [10. Strings and collections](ch10-strings-collections.md)
- [11. Projects: files, args, testing](ch11-projects.md)
- [12. Foreign functions and the runtime](ch12-ffi-runtime.md)
- [13. Under the hood: backends and performance](ch13-under-the-hood.md)
- [14. Not yet: the roadmap](ch14-not-yet.md)

## Reference

- [Appendix: keywords, operators, CLI](appendix.md)

> Conventions: `taka` code blocks are complete programs you can save as
> `prog.tk` and run with `taka run prog.tk`. Blocks tagged `taka,ignore`
> are fragments or intentional errors — they are not standalone programs.
> `book/check.sh` re-checks every standalone block with `taka check`.
