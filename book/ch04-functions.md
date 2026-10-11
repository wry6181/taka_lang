# 4. Functions

```taka,ignore
fn add(i32 a, i32 b) -> i32 {
    return a + b;
}

fn sum(List<i32> xs) -> i32 {
    mut i32 total = 0;
    mut u64 i = 0;
    while (i < xs.len) {
        total = total + xs.data[i];
        i = i + 1;
    }
    return total;
}

fn greet() {
    tk_print_str("hi");
    return;
}
```

- `fn name(params) -> Ret { … }`. No `->` means the function returns
  nothing; a bare `return;` exits early.
- Parameters are `[mut] type name`, comma-separated (a trailing comma
  is allowed). Pass by value; pass `&x` or pointers for everything
  else (Chapter 7).
- Functions can be declared in any order — call `bump()` before its
  definition if you like; there are no prototypes in taka source.

## `main`

Every runnable program has exactly one entry point:

```taka,ignore
fn main() {
    ...
    return;
}
```

No parameters, no return type. The driver wraps it into a real C
`main`, which is how command-line arguments and the exit code work
(Chapter 11): read args with `tk_argc()` / `tk_argv(i)`, set the
process status with `tk_set_exit(c)`:

```taka
extern fn tk_set_exit(i32 c);

fn main() {
    tk_set_exit(3);
}
```

```text
$ ./taka run exitcode.tk; echo "exit=$?"
exit=3
```

## External functions

`extern fn` declares a function implemented outside taka — in practice,
in C. The runtime library (Chapter 12) is all `extern fn`s, and so is
any C code you link:

```taka,ignore
extern fn tk_print_i32(i32 x);
extern fn tk_malloc(u64 n) -> u8*;
extern fn tk_argv(i32 i) -> char*;
```

The declaration must match the C signature. Taka checks the *call*
(types of arguments) but trusts the declaration — an incorrect `extern`
block is the one place where you can lie to the compiler, so keep
these declarations next to the C headers they mirror.
