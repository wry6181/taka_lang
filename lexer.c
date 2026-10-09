#include "lexer.h"
#include <string.h>
#include <ctype.h>

// EBNF lexical section -> this table.
// Each keyword rule in the EBNF becomes one entry here.
static const struct { const char *text; TokenKind kind; } KEYWORDS[] = {
    {"fn",    TOKEN_FN},
    {"struct", TOKEN_STRUCT},
    {"enum", TOKEN_ENUM},
    {"effect", TOKEN_EFFECT},
    {"handler", TOKEN_HANDLER},
    {"trait", TOKEN_TRAIT},
    {"impl", TOKEN_IMPL},
    {"dyn", TOKEN_DYN},
    {"type", TOKEN_TYPE},
    {"extern", TOKEN_EXTERN},
    {"import", TOKEN_IMPORT},
    {"mod", TOKEN_MOD},
    {"pub", TOKEN_PUB},
    {"private", TOKEN_PRIVATE},
    {"const", TOKEN_CONST},
    {"mut",   TOKEN_MUT},
    {"auto",  TOKEN_AUTO},
    {"where", TOKEN_WHERE},
    {"for", TOKEN_FOR},
    {"in", TOKEN_IN},
    {"if", TOKEN_IF},
    {"else", TOKEN_ELSE},
    {"while", TOKEN_WHILE},
    {"match", TOKEN_MATCH},
    {"break", TOKEN_BREAK},
    {"continue", TOKEN_CONTINUE},
    {"return", TOKEN_RETURN},
    {"defer", TOKEN_DEFER},
    {"perform", TOKEN_PERFORM},
    {"handle", TOKEN_HANDLE},
    {"with", TOKEN_WITH},
    {"resume", TOKEN_RESUME},
    {"transact", TOKEN_TRANSACT},
    {"spawn", TOKEN_SPAWN},
    {"as", TOKEN_AS},
    {"sizeof", TOKEN_SIZEOF},
    {"offsetof", TOKEN_OFFSETOF},
    {"true", TOKEN_TRUE},
    {"false", TOKEN_FALSE},
    {"null", TOKEN_NULL},
    {"undef", TOKEN_UNDEF},
    {"Self", TOKEN_SELF},
    {"i8",    TOKEN_I8},
    {"i16", TOKEN_I16},
    {"i32",   TOKEN_I32},
    {"i64", TOKEN_I64},
    {"u8", TOKEN_U8},
    {"u16", TOKEN_U16},
    {"u32", TOKEN_U32},
    {"u64", TOKEN_U64},
    {"f32", TOKEN_F32},
    {"f64", TOKEN_F64},
    {"char", TOKEN_CHAR_TY},
    {"bool", TOKEN_BOOL},
    {"sview", TOKEN_SVIEW},
    {"never", TOKEN_NEVER},
    {"io", TOKEN_IO},
    {"alloc", TOKEN_ALLOC},
    {"reads", TOKEN_READS},
    {"writes", TOKEN_WRITES},
    {"decides", TOKEN_DECIDES},
    {"suspends", TOKEN_SUSPENDS},
    {"transacts", TOKEN_TRANSACTS},
    {"pure", TOKEN_PURE},
    {"ownership", TOKEN_OWNERSHIP},
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
        case TOKEN_WILDCARD: return "_";
        case TOKEN_INT:     return "INT";
        case TOKEN_FLOAT:   return "FLOAT";
        case TOKEN_STRING:  return "STRING";
        case TOKEN_CHAR:    return "CHAR";
        case TOKEN_FN:      return "fn";
        case TOKEN_STRUCT:  return "struct";
        case TOKEN_ENUM:    return "enum";
        case TOKEN_EFFECT:  return "effect";
        case TOKEN_HANDLER: return "handler";
        case TOKEN_TRAIT:   return "trait";
        case TOKEN_IMPL:    return "impl";
        case TOKEN_DYN:     return "dyn";
        case TOKEN_TYPE:    return "type";
        case TOKEN_EXTERN:  return "extern";
        case TOKEN_IMPORT:  return "import";
        case TOKEN_MOD:     return "mod";
        case TOKEN_PUB:     return "pub";
        case TOKEN_PRIVATE: return "private";
        case TOKEN_CONST:   return "const";
        case TOKEN_MUT:     return "mut";
        case TOKEN_AUTO:    return "auto";
        case TOKEN_WHERE:   return "where";
        case TOKEN_FOR:     return "for";
        case TOKEN_IN:      return "in";
        case TOKEN_IF:      return "if";
        case TOKEN_ELSE:    return "else";
        case TOKEN_WHILE:   return "while";
        case TOKEN_MATCH:   return "match";
        case TOKEN_BREAK:   return "break";
        case TOKEN_CONTINUE: return "continue";
        case TOKEN_RETURN:  return "return";
        case TOKEN_DEFER:   return "defer";
        case TOKEN_PERFORM: return "perform";
        case TOKEN_HANDLE:  return "handle";
        case TOKEN_WITH:    return "with";
        case TOKEN_RESUME:  return "resume";
        case TOKEN_TRANSACT: return "transact";
        case TOKEN_SPAWN:   return "spawn";
        case TOKEN_AS:      return "as";
        case TOKEN_SIZEOF:  return "sizeof";
        case TOKEN_OFFSETOF: return "offsetof";
        case TOKEN_TRUE:    return "true";
        case TOKEN_FALSE:   return "false";
        case TOKEN_NULL:    return "null";
        case TOKEN_UNDEF:   return "undef";
        case TOKEN_SELF:    return "Self";
        case TOKEN_I8:      return "i8";
        case TOKEN_I16:     return "i16";
        case TOKEN_I32:     return "i32";
        case TOKEN_I64:     return "i64";
        case TOKEN_U8:      return "u8";
        case TOKEN_U16:     return "u16";
        case TOKEN_U32:     return "u32";
        case TOKEN_U64:     return "u64";
        case TOKEN_F32:     return "f32";
        case TOKEN_F64:     return "f64";
        case TOKEN_CHAR_TY: return "char";
        case TOKEN_BOOL:    return "bool";
        case TOKEN_SVIEW:   return "sview";
        case TOKEN_NEVER:   return "never";
        case TOKEN_IO:      return "io";
        case TOKEN_ALLOC:   return "alloc";
        case TOKEN_READS:   return "reads";
        case TOKEN_WRITES:  return "writes";
        case TOKEN_DECIDES: return "decides";
        case TOKEN_SUSPENDS: return "suspends";
        case TOKEN_TRANSACTS: return "transacts";
        case TOKEN_PURE:    return "pure";
        case TOKEN_OWNERSHIP: return "ownership";
        case TOKEN_EFFECT_KW: return "effect";
        case TOKEN_LPAREN:  return "(";
        case TOKEN_RPAREN:  return ")";
        case TOKEN_LBRACE:  return "{";
        case TOKEN_RBRACE:  return "}";
        case TOKEN_LBRACK:  return "[";
        case TOKEN_RBRACK:  return "]";
        case TOKEN_SEMI:    return ";";
        case TOKEN_COMMA:   return ",";
        case TOKEN_COLON:   return ":";
        case TOKEN_COLON2:  return "::";
        case TOKEN_DOT:     return ".";
        case TOKEN_DOTDOT:  return "..";
        case TOKEN_ARROW:   return "->";
        case TOKEN_FATARROW: return "=>";
        case TOKEN_QMARK:   return "?";
        case TOKEN_EQ:      return "=";
        case TOKEN_EQEQ:    return "==";
        case TOKEN_BANGEQ:  return "!=";
        case TOKEN_BANG:    return "!";
        case TOKEN_TILDE:   return "~";
        case TOKEN_PLUS:    return "+";
        case TOKEN_MINUS:   return "-";
        case TOKEN_STAR:    return "*";
        case TOKEN_SLASH:   return "/";
        case TOKEN_PERCENT: return "%";
        case TOKEN_LT:      return "<";
        case TOKEN_GT:      return ">";
        case TOKEN_LTEQ:    return "<=";
        case TOKEN_GTEQ:    return ">=";
        case TOKEN_SHL:     return "<<";
        case TOKEN_SHR:     return ">>";
        case TOKEN_AMP:     return "&";
        case TOKEN_AMP2:    return "&&";
        case TOKEN_PIPE:    return "|";
        case TOKEN_PIPE2:   return "||";
        case TOKEN_CARET:   return "^";
        case TOKEN_PLUS_EQ: return "+=";
        case TOKEN_MINUS_EQ: return "-=";
        case TOKEN_STAR_EQ: return "*=";
        case TOKEN_SLASH_EQ: return "/=";
        case TOKEN_PERCENT_EQ: return "%=";
        case TOKEN_SHL_EQ:  return "<<=";
        case TOKEN_SHR_EQ:  return ">>=";
        case TOKEN_AMP_EQ:  return "&=";
        case TOKEN_PIPE_EQ: return "|=";
        case TOKEN_CARET_EQ: return "^=";
        case TOKEN_PLUSPLUS: return "++";
        case TOKEN_MINUSMINUS: return "--";
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
static char peek3(Lexer *lx) { return lx->src[lx->pos + 2]; }

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

static b32 is_hexdig(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static b32 match_suffix(Lexer *lx, const char *s) {
    u64 n = (u64)strlen(s);
    for (u64 i = 0; i < n; i++) {
        if (lx->src[lx->pos + i] != s[i]) return FALSE;
    }
    char after = lx->src[lx->pos + n];
    if (isalnum((unsigned char)after) || after == '_') return FALSE;
    for (u64 i = 0; i < n; i++) advance(lx);
    return TRUE;
}

Token lexer_next(Lexer *lx) {
    skip_space_and_comments(lx);
    u64 start = lx->pos;
    i32 line = lx->line, col = lx->col;
    char c = peek(lx);

    if (c == '\0') return make(lx, TOKEN_EOF, start, line, col);

    // identifier ::= (letter | '_') { letter | digit | '_' } minus keywords
    if (isalpha((unsigned char)c) || c == '_') {
        advance(lx);
        while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
        u64 len = lx->pos - start;
        // lone `_` is WILDCARD
        if (len == 1 && lx->src[start] == '_')
            return make(lx, TOKEN_WILDCARD, start, line, col);
        Token t = make(lx, TOKEN_IDENT, start, line, col);
        t.kind = lookup_keyword(t.start, t.len);
        // TOKEN_EFFECT_KW alias never produced; keep EFFECT
        return t;
    }

    // number: int / float with 0x / 0b / suffixes / underscores
    if (isdigit((unsigned char)c)) {
        if (c == '0' && (peek2(lx) == 'x' || peek2(lx) == 'X')) {
            advance(lx); advance(lx);
            while (is_hexdig(peek(lx)) || peek(lx) == '_') advance(lx);
        } else if (c == '0' && (peek2(lx) == 'b' || peek2(lx) == 'B')) {
            advance(lx); advance(lx);
            while (peek(lx) == '0' || peek(lx) == '1' || peek(lx) == '_') advance(lx);
        } else {
            while (isdigit((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
            // float? digits '.' digits
            if (peek(lx) == '.' && isdigit((unsigned char)peek2(lx))) {
                advance(lx); // '.'
                while (isdigit((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
                // exponent
                if (peek(lx) == 'e' || peek(lx) == 'E') {
                    u64 save = lx->pos;
                    i32 scol = lx->col;
                    advance(lx);
                    if (peek(lx) == '+' || peek(lx) == '-') advance(lx);
                    if (!isdigit((unsigned char)peek(lx))) {
                        lx->pos = save; lx->col = scol;
                    } else {
                        while (isdigit((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
                    }
                }
                // float suffix f32/f64?
                if (peek(lx) == 'f' && peek2(lx) == '3' && peek3(lx) == '2') {
                    char a = lx->src[lx->pos+3];
                    if (!isalnum((unsigned char)a) && a != '_') { advance(lx); advance(lx); advance(lx); }
                } else if (peek(lx) == 'f' && peek2(lx) == '6' && peek3(lx) == '4') {
                    char a = lx->src[lx->pos+3];
                    if (!isalnum((unsigned char)a) && a != '_') { advance(lx); advance(lx); advance(lx); }
                }
                return make(lx, TOKEN_FLOAT, start, line, col);
            }
            // exponent without dot? e.g. 1e10
            if (peek(lx) == 'e' || peek(lx) == 'E') {
                u64 save = lx->pos; i32 scol = lx->col;
                char n1 = peek2(lx);
                char n2 = lx->src[lx->pos+2];
                b32 ok = FALSE;
                if (isdigit((unsigned char)n1)) ok = TRUE;
                else if ((n1 == '+' || n1 == '-') && isdigit((unsigned char)n2)) ok = TRUE;
                if (ok) {
                    advance(lx);
                    if (peek(lx) == '+' || peek(lx) == '-') advance(lx);
                    while (isdigit((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
                    if (peek(lx) == 'f' && peek2(lx) == '3' && peek3(lx) == '2') {
                        char a = lx->src[lx->pos+3];
                        if (!isalnum((unsigned char)a) && a != '_') { advance(lx); advance(lx); advance(lx); }
                    } else if (peek(lx) == 'f' && peek2(lx) == '6' && peek3(lx) == '4') {
                        char a = lx->src[lx->pos+3];
                        if (!isalnum((unsigned char)a) && a != '_') { advance(lx); advance(lx); advance(lx); }
                    }
                    return make(lx, TOKEN_FLOAT, start, line, col);
                } else {
                    lx->pos = save; lx->col = scol;
                }
            }
        }
        // int suffix i8..u64 (part of token)
        const char *sufs[] = {"i8","i16","i32","i64","u8","u16","u32","u64"};
        for (int i = 0; i < 8; i++) match_suffix(lx, sufs[i]);
        return make(lx, TOKEN_INT, start, line, col);
    }

    // float starting with '.'? grammar requires digit before '.', so skip.

    // string_literal ::= '"' { char | escape } '"'
    if (c == '"') {
        advance(lx);
        while (peek(lx) && peek(lx) != '"' && peek(lx) != '\n') {
            if (peek(lx) == '\\') { advance(lx); if (peek(lx)) advance(lx); }
            else advance(lx);
        }
        if (peek(lx) == '"') { advance(lx); return make(lx, TOKEN_STRING, start, line, col); }
        return make(lx, TOKEN_ERROR, start, line, col);
    }

    // char_literal ::= '\'' ( char | escape ) '\''
    if (c == '\'') {
        advance(lx);
        if (peek(lx) == '\\') { advance(lx); if (peek(lx)) advance(lx); }
        else if (peek(lx) && peek(lx) != '\'' && peek(lx) != '\n') advance(lx);
        if (peek(lx) == '\'') { advance(lx); return make(lx, TOKEN_CHAR, start, line, col); }
        return make(lx, TOKEN_ERROR, start, line, col);
    }

    // multi-char symbols, maximal munch
    // 3-char: <<=, >>=
    if (c == '<' && peek2(lx) == '<' && lx->src[lx->pos+2] == '=') {
        advance(lx); advance(lx); advance(lx);
        return make(lx, TOKEN_SHL_EQ, start, line, col);
    }
    if (c == '>' && peek2(lx) == '>' && lx->src[lx->pos+2] == '=') {
        advance(lx); advance(lx); advance(lx);
        return make(lx, TOKEN_SHR_EQ, start, line, col);
    }
    // 2-char
    if (c == ':' && peek2(lx) == ':') { advance(lx); advance(lx); return make(lx, TOKEN_COLON2, start, line, col); }
    if (c == '-' && peek2(lx) == '>') { advance(lx); advance(lx); return make(lx, TOKEN_ARROW, start, line, col); }
    if (c == '=' && peek2(lx) == '>') { advance(lx); advance(lx); return make(lx, TOKEN_FATARROW, start, line, col); }
    if (c == '.' && peek2(lx) == '.') { advance(lx); advance(lx); return make(lx, TOKEN_DOTDOT, start, line, col); }
    if (c == '=' && peek2(lx) == '=') { advance(lx); advance(lx); return make(lx, TOKEN_EQEQ, start, line, col); }
    if (c == '!' && peek2(lx) == '=') { advance(lx); advance(lx); return make(lx, TOKEN_BANGEQ, start, line, col); }
    if (c == '<' && peek2(lx) == '=') { advance(lx); advance(lx); return make(lx, TOKEN_LTEQ, start, line, col); }
    if (c == '>' && peek2(lx) == '=') { advance(lx); advance(lx); return make(lx, TOKEN_GTEQ, start, line, col); }
    if (c == '<' && peek2(lx) == '<') { advance(lx); advance(lx); return make(lx, TOKEN_SHL, start, line, col); }
    if (c == '>' && peek2(lx) == '>') { advance(lx); advance(lx); return make(lx, TOKEN_SHR, start, line, col); }
    if (c == '&' && peek2(lx) == '&') { advance(lx); advance(lx); return make(lx, TOKEN_AMP2, start, line, col); }
    if (c == '|' && peek2(lx) == '|') { advance(lx); advance(lx); return make(lx, TOKEN_PIPE2, start, line, col); }
    if (c == '+' && peek2(lx) == '=') { advance(lx); advance(lx); return make(lx, TOKEN_PLUS_EQ, start, line, col); }
    if (c == '-' && peek2(lx) == '=') { advance(lx); advance(lx); return make(lx, TOKEN_MINUS_EQ, start, line, col); }
    if (c == '*' && peek2(lx) == '=') { advance(lx); advance(lx); return make(lx, TOKEN_STAR_EQ, start, line, col); }
    if (c == '/' && peek2(lx) == '=') { advance(lx); advance(lx); return make(lx, TOKEN_SLASH_EQ, start, line, col); }
    if (c == '%' && peek2(lx) == '=') { advance(lx); advance(lx); return make(lx, TOKEN_PERCENT_EQ, start, line, col); }
    if (c == '&' && peek2(lx) == '=') { advance(lx); advance(lx); return make(lx, TOKEN_AMP_EQ, start, line, col); }
    if (c == '|' && peek2(lx) == '=') { advance(lx); advance(lx); return make(lx, TOKEN_PIPE_EQ, start, line, col); }
    if (c == '^' && peek2(lx) == '=') { advance(lx); advance(lx); return make(lx, TOKEN_CARET_EQ, start, line, col); }
    if (c == '+' && peek2(lx) == '+') { advance(lx); advance(lx); return make(lx, TOKEN_PLUSPLUS, start, line, col); }
    if (c == '-' && peek2(lx) == '-') { advance(lx); advance(lx); return make(lx, TOKEN_MINUSMINUS, start, line, col); }

    // single char
    switch (c) {
        case '(': advance(lx); return make(lx, TOKEN_LPAREN, start, line, col);
        case ')': advance(lx); return make(lx, TOKEN_RPAREN, start, line, col);
        case '{': advance(lx); return make(lx, TOKEN_LBRACE, start, line, col);
        case '}': advance(lx); return make(lx, TOKEN_RBRACE, start, line, col);
        case '[': advance(lx); return make(lx, TOKEN_LBRACK, start, line, col);
        case ']': advance(lx); return make(lx, TOKEN_RBRACK, start, line, col);
        case ';': advance(lx); return make(lx, TOKEN_SEMI, start, line, col);
        case ',': advance(lx); return make(lx, TOKEN_COMMA, start, line, col);
        case ':': advance(lx); return make(lx, TOKEN_COLON, start, line, col);
        case '.': advance(lx); return make(lx, TOKEN_DOT, start, line, col);
        case '?': advance(lx); return make(lx, TOKEN_QMARK, start, line, col);
        case '=': advance(lx); return make(lx, TOKEN_EQ, start, line, col);
        case '!': advance(lx); return make(lx, TOKEN_BANG, start, line, col);
        case '~': advance(lx); return make(lx, TOKEN_TILDE, start, line, col);
        case '+': advance(lx); return make(lx, TOKEN_PLUS, start, line, col);
        case '-': advance(lx); return make(lx, TOKEN_MINUS, start, line, col);
        case '*': advance(lx); return make(lx, TOKEN_STAR, start, line, col);
        case '/': advance(lx); return make(lx, TOKEN_SLASH, start, line, col);
        case '%': advance(lx); return make(lx, TOKEN_PERCENT, start, line, col);
        case '<': advance(lx); return make(lx, TOKEN_LT, start, line, col);
        case '>': advance(lx); return make(lx, TOKEN_GT, start, line, col);
        case '&': advance(lx); return make(lx, TOKEN_AMP, start, line, col);
        case '|': advance(lx); return make(lx, TOKEN_PIPE, start, line, col);
        case '^': advance(lx); return make(lx, TOKEN_CARET, start, line, col);
    }

    advance(lx);
    return make(lx, TOKEN_ERROR, start, line, col);
}
