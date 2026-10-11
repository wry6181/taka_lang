# 2. Variables and mutability

Declarations have one shape: optional `mut`, a type, a name, and a
required initializer.

```taka
extern fn tk_print_i32(i32 x);

fn main() {
    i32 x = 1;
    mut i32 y = 2;
    y = y + x;
    auto w = 40 + 2;
    tk_print_i32(x);
    tk_print_i32(y);
    tk_print_i32(w);
}
```

```text
1
3
42
```

- `i32 x = 1;` — immutable. Assigning to `x` later is a checker error
  (Chapter 8).
- `mut i32 y = 2;` — mutable. Only `mut` bindings may be assigned.
- `auto w = 40 + 2;` — the type is inferred (`i32` here). Use `auto`
  when the initializer makes the type obvious.

## Scopes and shadowing

Blocks introduce scopes. A binding may be shadowed in an inner block;
the outer one is untouched:

```taka
extern fn tk_print_i32(i32 x);

fn main() {
    i32 x = 1;
    tk_print_i32(x);
    {
        i32 x = 2;
        tk_print_i32(x);
    }
    tk_print_i32(x);
}
```

```text
1
2
1
```

## Constants

`const` bindings cannot be mutated, like immutable bindings, and
document intent:

```taka
extern fn tk_print_i32(i32 x);

fn main() {
    const i32 c = 5;
    tk_print_i32(c * 2);
}
```

```text
10
```

## Globals

Top-level bindings are globals, visible to every function. A global
that functions assign must be `mut`:

```taka
extern fn tk_print_i32(i32 x);

mut i32 g_count = 0;

fn bump() {
    g_count = g_count + 1;
}

fn main() {
    bump();
    bump();
    tk_print_i32(g_count);
}
```

```text
2
```

Forgetting `mut` gives `sema error: mutation of non-mut binding
'g_count'` — the same rule as locals, applied uniformly.
