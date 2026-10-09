#include "lexer.h"
#include <string.h>
#include <ctype.h>

// EBNF lexical section -> this table.
// Each keyword rule in the EBNF becomes one entry here.
static const struct { const char *text; TokenKind kind; } KEYWORDS[] = {
    {"fn",    TOKEN_FN},
    {"mut",   TOKEN_MUT},
    {"const", TOKEN_CONST},
    {"auto",  TOKEN_AUTO},
    {"i8",    TOKEN_I8},
    {"i32",   TOKEN_I32},
};

#define N_KEYWORDS (sizeof KEYWORDS / sizeof KEYWORDS[0])

static TokenKind lookup_keyword(const char *s, u64 len) {
    for (u64 i = 0; i < N_KEYWORDS; i++) {
        if ((u64)strlen(KEYWORDS[i].text) == len && memcmp(KEYWORDS[i].text, s, (size_t)len) == 0)
            return KEYWORDS[i].kind;
    }
    return TOKEN_IDENT;
}

const char *token_name(TokenKind k) {
    switch (k) {
        case TOKEN_IDENT:   return "IDENT";
        case TOKEN_INT:     return "INT";
        case TOKEN_FN:      return "fn";
        case TOKEN_MUT:     return "mut";
        case TOKEN_CONST:   return "const";
        case TOKEN_AUTO:    return "auto";
        case TOKEN_I8:      return "i8";
        case TOKEN_I32:     return "i32";
        case TOKEN_LPAREN:  return "(";
        case TOKEN_RPAREN:  return ")";
        case TOKEN_LBRACE:  return "{";
        case TOKEN_RBRACE:  return "}";
        case TOKEN_SEMI:    return ";";
        case TOKEN_EQ:      return "=";
        case TOKEN_PLUS_EQ: return "+=";
        case TOKEN_STAR:    return "*";
        case TOKEN_AMP:     return "&";
        case TOKEN_EOF:     return "EOF";
        case TOKEN_ERROR:   return "ERROR";
    }
    return "?";
}

void lexer_init(Lexer *lx, const char *src) {
    lx->src = src;
    lx->pos = 0;
    lx->line = 1;
    lx->col = 1;
}

static char peek(Lexer *lx) { return lx->src[lx->pos]; }
static char peek2(Lexer *lx) { return lx->src[lx->pos + 1]; }

static char advance(Lexer *lx) {
    char c = lx->src[lx->pos++];
    if (c == '\n') { lx->line++; lx->col = 1; }
    else lx->col++;
    return c;
}

static Token make(Lexer *lx, TokenKind kind, u64 start, i32 line, i32 col) {
    Token t;
    t.kind = kind;
    t.start = lx->src + start;
    t.len = lx->pos - start;
    t.line = line;
    t.col = col;
    return t;
}

static void skip_space_and_comments(Lexer *lx) {
    for (;;) {
        char c = peek(lx);
        // whitespace
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(lx); continue; }
        // line comment //
        if (c == '/' && peek2(lx) == '/') {
            while (peek(lx) && peek(lx) != '\n') advance(lx);
            continue;
        }
        // block comment /* ... */ (nestable per EBNF)
        if (c == '/' && peek2(lx) == '*') {
            advance(lx); advance(lx);
            i32 depth = 1;
            while (depth > 0 && peek(lx)) {
                if (peek(lx) == '/' && peek2(lx) == '*') { advance(lx); advance(lx); depth++; }
                else if (peek(lx) == '*' && peek2(lx) == '/') { advance(lx); advance(lx); depth--; }
                else advance(lx);
            }
            continue;
        }
        break;
    }
}

Token lexer_next(Lexer *lx) {
    skip_space_and_comments(lx);
    u64 start = lx->pos;
    i32 line = lx->line, col = lx->col;
    char c = peek(lx);

    if (c == '\0') return make(lx, TOKEN_EOF, start, line, col);

    // identifier ::= (letter | '_') { letter | digit | '_' }
    if (isalpha((unsigned char)c) || c == '_') {
        advance(lx);
        while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
        Token t = make(lx, TOKEN_IDENT, start, line, col);
        t.kind = lookup_keyword(t.start, t.len);
        return t;
    }

    // int_literal ::= digit { digit | '_' }  (0x/0b omitted for mut.tk brevity)
    if (isdigit((unsigned char)c)) {
        advance(lx);
        while (isdigit((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
        return make(lx, TOKEN_INT, start, line, col);
    }

    // single / double char symbols
    switch (c) {
        case '(': advance(lx); return make(lx, TOKEN_LPAREN, start, line, col);
        case ')': advance(lx); return make(lx, TOKEN_RPAREN, start, line, col);
        case '{': advance(lx); return make(lx, TOKEN_LBRACE, start, line, col);
        case '}': advance(lx); return make(lx, TOKEN_RBRACE, start, line, col);
        case ';': advance(lx); return make(lx, TOKEN_SEMI, start, line, col);
        case '*': advance(lx); return make(lx, TOKEN_STAR, start, line, col);
        case '&': advance(lx); return make(lx, TOKEN_AMP, start, line, col);
        case '=': advance(lx); return make(lx, TOKEN_EQ, start, line, col);
        case '+':
            advance(lx);
            if (peek(lx) == '=') { advance(lx); return make(lx, TOKEN_PLUS_EQ, start, line, col); }
            // mut.tk never uses lone '+', report it clearly
            return make(lx, TOKEN_ERROR, start, line, col);
    }

    advance(lx);
    return make(lx, TOKEN_ERROR, start, line, col);
}
