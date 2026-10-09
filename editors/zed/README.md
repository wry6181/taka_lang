# taka support for Zed (dev extension)

## Use it now (local, no publishing)

1. The grammar path is already set for this checkout. On another machine,
   point it at your checkout (absolute path required):

```sh
sed -i "s|file://.*editors/tree-sitter-taka|file://$PWD/../tree-sitter-taka|" extension.toml
```

   `rev` must stay pinned: after any grammar change, commit it inside
   `../tree-sitter-taka` and copy the new SHA into `extension.toml`:

```sh
cd ../tree-sitter-taka && git add -A && git commit -m "update grammar"
git rev-parse HEAD   # paste into extension.toml as rev
```

2. In Zed: command palette → `zed: install dev extension` → select this
   directory (`editors/zed`). Needs `wasi-sdk` once — Zed downloads it
   automatically (or set `WASI_SDK_PATH`).

3. Open any `*.tk` file: highlighting, outline, brackets, folding, indentation.

## Publish later

1. Push `editors/tree-sitter-taka` to GitHub (e.g. `tree-sitter-taka`).
2. In `extension.toml`, replace the `file://` URL with the https URL plus a
   commit `rev`, then submit the extension to
   [zed-extensions](https://github.com/zed-industries/extensions).

## Keep queries in sync

`languages/taka/*.scm` are copies of `../tree-sitter-taka/queries/*.scm`
(the grammar dir is the source of truth):

```sh
make zed-sync   # from the repo root
```

## Grammar notes

`>>` lexes as shift: write nested generics spaced (`List<List<i32> >`).
`&&` in prefix position is two `&` only via spacing. Effect names
(`io`, `alloc`, …) highlight as keywords everywhere (they are contextual
in `grammar.ebnf` and usable as identifiers).
