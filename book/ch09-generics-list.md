# 9. Generics and `List`

One honest paragraph first: today, `List<T>` is the *only* generic
that builds. `Map<K,V>`, `Box<T>`, `fn f<T>(T x) -> T` all *parse* —
the checker waves them through as "validation OK" — but neither
backend monomorphizes them. Everything below is about `List`, the one
generic with a real lowering.

## Creating lists

```taka
extern fn tk_print_i32(i32 x);

fn sum(List<i32> xs) -> i32 {
    mut i32 total = 0;
    mut u64 i = 0;
    while (i < xs.len) {
        total = total + xs.data[i];
        i = i + 1;
    }
    return total;
}

fn main() {
    List<i32> xs = [10, 20, 30];
    xs.push(40);
    tk_print_i32(sum(xs));
    tk_print_i32(xs.len as i32);
}
```

```text
100
4
```

A `List<T>` is three words — `{data, len, cap}` — with C-level layout.
`[10, 20, 30]` heap-allocates and copies; `[]` makes an empty list;
`push` grows 8→16→32…; `.len` is the count; `.data[i]` is unchecked
indexing into the buffer. Nesting works, including the `>>` closer
with no space needed:

```taka
extern fn tk_print_i32(i32 x);

fn main() {
    List<List<i32>> grid = [];
    List<i32> row = [1, 2];
    grid.push(row);
    tk_print_i32(grid.data[0].data[1]);
}
```

```text
2
```

## Lists of structs

Lists hold any element type, including your structs — the standard way
to build tables, buffers, and pools:

```taka
extern fn tk_print_i32(i32 x);

struct Entry { i32 key, i32 val, }

fn assoc_get(List<Entry> xs, i32 key) -> i32 {
    mut u64 i = 0;
    while (i < xs.len) {
        if (xs.data[i].key == key) { return xs.data[i].val; }
        i = i + 1;
    }
    return 0 - 1;
}

fn main() {
    List<Entry> m = [];
    Entry e = Entry { key: 7, val: 700, };
    m.push(e);
    tk_print_i32(assoc_get(m, 7));
    tk_print_i32(assoc_get(m, 1));
}
```

```text
700
-1
```

The repository's `tests/map_test.tk` scales this exact pattern into a
Robin-Hood hash map (`StrMap` over `List<StrEntry>`) — read it when
you outgrow linear scan. And when you need a map with non-string keys
or a set, a tree, a graph: this chapter is the whole toolkit. There is
no standard library yet — `List` plus structs plus functions *is* the
standard library, and it is enough to be going on with.
