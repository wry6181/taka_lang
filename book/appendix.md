# Appendix: keywords, operators, CLI

## Keywords

```text
fn struct enum effect handler trait impl dyn type extern import mod
pub private const mut auto where for in if else while match
break continue return defer perform handle with resume transact spawn
as sizeof offsetof
true false null undef Self
i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 char bool sview never
```

Effect-row names (`io alloc reads writes decides suspends transacts
pure ownership`) are contextual — usable as identifiers outside effect
position. A lone `_` is a wildcard pattern, not an identifier.

## Operators (high to low precedence)

```text
( ) [ ] .                 call, index, field
! ~ - * & (unary)  as     not, bit-not, negate, deref, addr-of, cast
* / %                     multiplicative
+ -                       additive
<< >>                     shifts
&                         bit-and
^                         bit-xor
|                         bit-or
== != < <= > >=           comparison (do NOT chain)
&&                        logical and
||                        logical or
? :                       ternary (right-assoc)
= += -= *= /= %= <<= >>= &= |= ^=
```

Postfix `++`/`--` bind tightest with their operand. `=`-family
assignment is a statement-level form (also allowed in `for` headers).

## CLI reference (`taka help`)

```text
taka — cargo-like driver for the taka language
usage:
  taka [flags] build file.tk [-o out]   compile to a native binary
  taka [flags] run file.tk [args..]     build to temp and execute
  taka [flags] test file.tk [more..]    build+run each file, report ok/FAILED
  taka [flags] check file.tk            typecheck only (like cargo check)
  taka [flags] emit-c file.tk [-o f.c]  write the generated C (inspect/optimize by hand)
  taka [flags] sexp file.tk             dump the canonical AST sexp
  taka [flags] file.tk                  parse, check, emit (<base>.gen.c or .s)
  taka help                             show this help
flags (before the subcommand):
  --backend=c|native   c = via C + cc optimizer (default); native = direct x86-64
  --release            optimize the C build (-O2; default is -O0 for fast builds)
  --dev                fast unoptimized C build (-O0; the default)
  -h, --help           show this help
```

(`taka file.tk` on a file ending in `mut.tk` takes a legacy path —
prefer `check`/`build`/`run` for those.)
