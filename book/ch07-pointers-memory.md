# 7. Pointers and memory

`&x` takes an address, `*p` reads or writes through it, `T*` is a
pointer type. Pointer arithmetic works on byte and element pointers:

```taka
extern fn tk_print_i32(i32 x);

fn main() {
    i32 a = 8;
    mut i32 x = 32;
    i32* p = &a;
    tk_print_i32(*p);
    *p = 10;
    tk_print_i32(a);
    x = x + 1;
    tk_print_i32(x);
}
```

```text
8
10
33
```

Field access through a pointer needs no special operator — `.`
auto-dereferences, so `m.len` works whether `m` is a `StrMap` or a
`StrMap*`.

## The memory model

There is no garbage collector, and the language never allocates behind
your back:

- **Locals, parameters, and temporaries live in stack frames.** A
  function call reserves its frame; return releases it. This is why
  `defer` bodies can freely read locals — they run before the frame
  is torn down.
- **`List` buffers live on the heap** (`malloc` at creation, doubling
  growth from capacity 8) and are *never freed* by generated code —
  process-lifetime storage, arena-style. The same holds for `defer`
  bookkeeping nodes (freed as each chain drains) and string/file
  buffers you allocate.
- **For manual management**, `tk_malloc(n) -> u8*` and `tk_free(p)`
  are straight through to C. Cast between `u8*` and your pointer
  types with `as`.

In practice this means: don't worry about freeing `List`s in short-
lived tools (the OS reclaims everything at exit, like an arena), and
reach for `tk_malloc`/`tk_free` or file-backed storage for anything
with a stricter budget. Future taka work (Chapter 14) may add explicit
ownership; today the rule is one sentence: *stack by default, explicit
heap when you ask for it, no collector, no surprises.*

## References

`T&` reference bindings (`i32& r = x;`) exist for borrowed access, and
`&v` patterns borrow match payloads (Chapter 6). One restriction to
know: `mut` on `&` is rejected — `mut i32& r = x;` fails with
`` `mut` on `&` is a checker error ``. Mutability lives on the binding
(`mut i32 x`), not on the reference.
