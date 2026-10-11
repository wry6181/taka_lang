# 5. Control flow

Conditions need parentheses; bodies need braces. This program exercises
every loop and branch:

```taka
extern fn tk_print_i32(i32 x);

fn main() {
    mut i32 i = 0;
    mut i32 total = 0;
    while (i < 5) {
        total = total + i;
        i = i + 1;
    }
    tk_print_i32(total);
    for (mut i32 j = 0; j < 3; j++) {
        tk_print_i32(j * 10);
    }
    i32 n = 2;
    if (n == 1) {
        tk_print_i32(100);
    } else if (n == 2) {
        tk_print_i32(200);
    } else {
        tk_print_i32(300);
    }
}
```

```text
10
0
10
20
200
```

- `while (cond) { … }` loops; `while (true)` with `break` is the
  idiom for infinite loops.
- `for (init; cond; incr) { … }` is the single loop form — C-style
  only. (A `for (v in xs)` form parses but does not build yet; see
  Chapter 14. Write `while` with an index instead.)
- `if` / `else if` / `else` work as expected. `break` and `continue`
  work inside loops; `return` (with or without a value) exits the
  function.

## `match`

`match` on enums is a statement with block arms. Patterns bind payloads
by value, alternatives use `|`, and `_` catches everything:

```taka
extern fn tk_print_i32(i32 x);

enum Color { Red = 1, Green = 2, Blue = 4, }
enum Opt { Some(i32), None, }

fn main() {
    Color c = Color::Green;
    mut i32 out = 0;
    match c {
        Color::Red => { out = 1; },
        Color::Green | Color::Blue => { out = 2; },
        _ => { out = 3; },
    }
    tk_print_i32(out);
    Opt o = Opt::Some(5);
    match o {
        Opt::Some(x) => { out = x; },
        Opt::None => { out = 0 - 1; },
    }
    tk_print_i32(out);
}
```

```text
2
5
```

`match` is also an expression in `return` position (and in
initializers), with value arms — including literal patterns:

```taka
extern fn tk_print_i32(i32 x);

fn classify(i32 v) -> i32 {
    return match v {
        0 => 0,
        _ => 1,
    };
}

fn main() {
    tk_print_i32(classify(0));
}
```

```text
0
```

Guards (`Opt::Some(x) if x > 10 => …`) and `&`-bindings in patterns
(`Opt::Some(&v)`, Chapter 6) round out the feature.

## Block values

A bare block in expression position evaluates to its trailing
expression — no `return` needed inside:

```taka
extern fn tk_print_i32(i32 x);

fn main() {
    i32 doubled = { mut i32 t = 21; t * 2 };
    tk_print_i32(doubled);
    tk_print_i32({ 10 } + { 20 });
}
```

```text
42
30
```

## `defer`: code that runs at scope exit

`defer` schedules a statement or block to run when the current function
returns — on *every* path, including early returns. Defers run
last-in-first-out:

```taka
extern fn tk_print_i32(i32 x);

fn early(i32 c) -> i32 {
    defer tk_print_i32(99);
    if (c == 1) { return 7; }
    return 9;
}

fn main() {
    tk_print_i32(early(1));
    defer tk_print_i32(3);
    tk_print_i32(1);
    defer { tk_print_i32(2); }
}
```

```text
99
7
1
2
3
```

`early(1)` prints `99` (the defer) and *then* `7` (the return value):
the drain runs before the value leaves. In `main`, `1` prints first,
then the block defer `2`, then the simple defer `3` — LIFO.

Two rules to internalize:

1. **Deferred bodies read variables when they run, not when they are
   registered** (Zig-style). If a loop mutates `i` after `defer f(i)`,
   every firing sees the final `i`. Copy to a fresh binding first when
   you need a snapshot.
2. **`return`, `break`, and `continue` are forbidden inside defer
   bodies** — the checker rejects them (`sema error: return inside
   defer is not allowed`). A defer observes the exit; it cannot cause
   one.
