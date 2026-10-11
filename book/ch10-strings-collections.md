# 10. Strings and collections

Taka has no `String` class. Text comes in two C-honest forms:

- `char*` — a NUL-terminated C string. String *literals* have this
  type, which is why `tk_print_str("hi")` just works.
- `sview` — a fat view `{char* ptr; i64 len}` for substrings and
  binary-safe slices. Build one with `tk_sview(p, n)` / `tk_sview_u8`.

```taka
extern fn tk_print_i32(i32 x);
extern fn tk_print_str(char* s);
extern fn tk_sview(char* p, u64 n) -> sview;

fn slen(sview s) -> i32 {
    return s.len as i32;
}

fn main() {
    auto s = "hi";
    tk_print_str(s);
    sview v = tk_sview("hello", 5);
    tk_print_i32(slen(v));
    tk_print_i32(v.ptr[1] as i32);
}
```

```text
hi
5
101
```

(`101` is `'e'` — `v.ptr[1]` indexes the raw bytes.) There is no
concatenation operator and no formatting; compare lengths and bytes
directly, push bytes into a `List<u8>`, and NUL-scan `char*` with
`while (s[i] != 0)`. Escapes (`\n`, `\\`, …) pass straight through to
C. A literal is `char*`, *not* `sview` — `sview s = "hello";` does not
build; construct the view explicitly so the length is never a secret.

## Arrays

`[a, b, c]` literals need a `List<T>` (or fixed-array) context to give
them a type — `List<i32> xs = [10, 20, 30];` allocates and copies.
Indexing is `.data[i]` for lists and `[i]` for raw pointers. There are
no slices (`xs[a..b]` does not exist); pass `(ptr, len)` pairs or
`sview`s instead.
