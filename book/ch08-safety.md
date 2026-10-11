# 8. Safety rules

Taka's checker (`taka check`) enforces a short list of rules. Every
rule fails the build with exit code 1 and a message that names the
problem. Learn these five errors and you will rarely be surprised.

## 1. Mutability is checked

```taka,ignore
i32 a = 1;
a = 2; // sema error: mutation of non-mut binding 'a'
```

Add `mut` to the binding — or don't mutate it. Globals follow the same
rule (`mut i32 g_count = 0;`).

## 2. Everything is initialized and in scope

```taka,ignore
tk_print_i32(undefined_var + 1);
// sema error: use of undefined variable 'undefined_var'
```

Declarations require initializers (`i32 x;` does not exist) and use
before declaration is an error. Shadowing in an inner block is fine;
two bindings with one name in one scope are not:

```taka,ignore
mut i32 a = 1;
mut i32 a = 2; // sema error: duplicate binding 'a'
```

## 3. Statements must do something

```taka,ignore
fn main() {
    1 + 2; // sema error: statement with no effect
}
```

A bare expression statement is only legal if it can have an effect —
a call, an assignment, `++`/`--`. A stray `x;` or `1 + 2;` is almost
always a bug, so the checker says so.

## 4. Types must be finite

```taka,ignore
struct Bad { Bad inner, i32 v, }
// sema error: struct contains itself by value (infinite size) 'Bad'
```

Use a pointer (`Bad* inner`) for recursive types. The checker computes
every struct's size; infinite ones cannot exist.

## 5. `defer` bodies cannot divert control flow

```taka,ignore
fn bad() {
    defer { return; } // sema error: return inside defer is not allowed
}
```

Same for `break` and `continue` inside defer. A defer observes the
function's exit; it cannot cause one (Chapter 5).

## Philosophy

These are not borrow-checker rules — there is no borrow checker. They
are *typo-and-thinko* rules: the mistakes a compiler can catch for
free, with messages a human can act on. Memory unsafety (dangling
pointers, data races) is still your responsibility, exactly as in C —
taka's promise is only that nothing is hidden and everything above is
caught before codegen.
