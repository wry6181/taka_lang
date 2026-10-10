// Reference token dump: same 5-lines-per-token format as self_lex.tk tk_dump.
#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 8);
    if (!buf) return 1;
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    for (long i = (long)got; i < (long)got + 8; i++) buf[i] = 0;
    Lexer lx;
    lexer_init(&lx, buf);
    for (;;) {
        Token t = lexer_next(&lx);
        printf("%d\n%d\n%llu\n%d\n%d\n", (int)t.kind, (int)(t.start - buf),
            (unsigned long long)t.len, (int)t.line, (int)t.col);
        if (t.kind == TOKEN_EOF) break;
    }
    free(buf);
    return 0;
}
