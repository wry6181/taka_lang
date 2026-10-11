#!/usr/bin/env bash
# Re-verify every standalone ```taka block in book/*.md with `taka check`.
# Blocks tagged ```taka,ignore are fragments/intentional errors: skipped.
# Usage: book/check.sh [path-to-taka]  (default: ./taka, run from repo root)
set -u
TAKA="${1:-./taka}"
OUT=$(mktemp -d /tmp/bookcheck-XXXXXX)
trap 'rm -rf "$OUT"' EXIT
python3 - "$OUT" <<'EOF'
import glob, os, sys
outdir = sys.argv[1]
n = 0
for md in sorted(glob.glob('book/*.md')):
    if md.endswith('SUMMARY.md'):
        continue
    text = open(md).read().split('\n')
    i = 0
    while i < len(text):
        line = text[i]
        if line == '```taka':
            buf = []
            i += 1
            while i < len(text) and text[i] != '```':
                buf.append(text[i])
                i += 1
            if any(l.strip() for l in buf):
                n += 1
                open(os.path.join(outdir, 'b%03d.tk' % n), 'w').write('\n'.join(buf) + '\n')
        i += 1
print('extracted %d blocks' % n)
EOF
pass=0; fail=0; failed=""
for f in "$OUT"/b*.tk; do
    [ -e "$f" ] || continue
    if "$TAKA" check "$f" >/dev/null 2>&1; then
        pass=$((pass+1))
    else
        fail=$((fail+1)); failed="$failed $(basename "$f")"
    fi
done
echo "book blocks: $pass passed, $fail failed"
[ -n "$failed" ] && echo "FAILED:$failed"
[ "$fail" -eq 0 ]
