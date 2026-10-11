# 3. Basic types and operators

## Primitive types

| Type | Meaning |
|---|---|
| `i8 i16 i32 i64` | signed integers |
| `u8 u16 u32 u64` | unsigned integers |
| `f32 f64` | floats |
| `char` | a single byte character |
| `bool` | `true` / `false` |
| `sview` | a string view `{ptr, len}` (Chapter 10) |
| `never` | the uninhabited type |

## Literals

Integers accept underscores and `0x` / `0b` prefixes; floats need digits
around the `.`; strings are double-quoted C-style; characters are
single-quoted:

```taka,ignore
i32 dec = 42;
i32 under = 1_000_000;
i32 hex = 0xFF_00;
f64 pi = 3.14;
sview note = tk_sview("hi 42\n", 6);
char c = 'x';
bool flag = true;
```

There are **no negative literals**: write `0 - 7`, exactly as you would
think it. This program:

```taka
extern fn tk_print_i32(i32 x);

fn main() {
    tk_print_i32(7 / 2);
    tk_print_i32((0 - 7) / 2);
    tk_print_i32((0 - 7) % 2);
    tk_print_i32((16 >> 2) as i32);
    tk_print_i32((1 << 3) as i32);
    mut i32 sq = 16;
    sq >>= 2;
    tk_print_i32(sq);
    u64 big = 100;
    tk_print_i32(big as i32 + 1);
}
```

prints:

```text
3
-3
-1
4
8
4
101
```

Integer division truncates toward zero and `%` takes the sign of the
left side — plain C semantics, because taka *is* C under the hood here.

## Casts

Conversions are always explicit with `as`: `big as i32`, `x as u64`,
`buf as char*`. There are no implicit conversions (widening included).
If a cast looks noisy, that is the point — every conversion is a
decision you can see.

## Operators

Arithmetic `+ - * / %`, bitwise `& | ^ ~ << >>`, comparison
`== != < <= > >=`, logical `&& || !`, and the ternary `cond ? a : b`:

```taka
extern fn tk_print_i32(i32 x);

fn main() {
    i32 a = 5;
    tk_print_i32(a > 3 ? 1 : 0);
}
```

```text
1
```

Comparisons do not chain (`a < b < c` is rejected — say what you mean).
All eleven compound assignments exist and work on variables, fields,
indexes, and dereferences:

```taka,ignore
x += 2; x -= 3; x *= 4; x /= 5; x %= 6;
x <<= 1; x >>= 1; x &= 255; x |= 15; x ^= 7;
```

`++` and `--` exist as well (`i++` in loop steps is idiomatic).
The full operator table with precedence is in the Appendix.
