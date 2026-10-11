# 6. Structs and enums

## Structs

```taka,ignore
struct Point { i32 x, i32 y, }
struct Arena { u8* buf, u64 cap, u64 pos, }
struct Outer { Inner inn, u64 tag, }
```

Fields are `type name` pairs; the trailing comma is optional but
idiomatic. Construction is a literal with `name: value` fields and
nests freely; access is `.` (which auto-dereferences one pointer
level, so there is no `->`):

```taka,ignore
Point p = Point { x: 10, y: 20, };
Outer o = Outer { inn: Inner { a: a, b: b, }, tag: t, };
tk_print_i32(p.x + p.y);
tk_print_i32(o.inn.a as i32);
```

Structs may contain `List` fields, pointers, and other structs — but
never themselves *by value* (`struct Bad { Bad inner, … }` is rejected
as infinite size; use `Bad*` for recursive types).

## Enums

Plain enums hold discriminants and compare with `==`:

```taka,ignore
enum Color { Red = 1, Green = 2, Blue = 4, }

fn is_red(Color c) -> bool {
    return c == Color::Red;
}
```

Payload enums carry data in tuple or struct variants — the Rust-style
part of taka:

```taka,ignore
enum Opt { Some(i32), None, }
enum Wrap { Hold(Inner), Empty, }
enum Shape { Circle(i32), Rect { i32 w, i32 h, }, Empty, }

Opt a = Opt::Some(5);
Shape s = Shape::Rect { w: 3, h: 4, };
Opt n = Opt::None;
```

One rule: payload and `= const` never mix in one enum — the checker
rejects `enum Mixed { A(i32), C = 3, }`.

## Patterns in depth

Value bindings copy the payload out:

```taka,ignore
match o {
    Opt::Some(x) => { out = x; },
    Opt::None => { out = 0 - 1; },
}
match s {
    Shape::Rect { w, h } => { out = w + h; },
    _ => { out = 0; },
}
```

Reference bindings borrow it with `&` and read through `*` — no copy,
no move:

```taka
extern fn tk_print_i32(i32 x);

enum Opt { Some(i32), None, }

fn main() {
    Opt o = Opt::Some(5);
    mut i32 out = 0;
    match o {
        Opt::Some(&v) => { out = *v + 100; },
        Opt::None => { out = 0 - 2; },
    }
    tk_print_i32(out);
}
```

```text
105
```

Alternatives must stay binding-free (`Color::Green | Color::Blue`
binds nothing — if branches bound different names, which one would
you get?). Guards add conditions: `Opt::Some(x) if x > 10 => …`.
