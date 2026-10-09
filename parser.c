// parser.c — hand-mapped from grammar.ebnf (mut.tk subset only).
//
// Rule: every EBNF rule becomes one parse_* function.
//   program        ::= { item }                    -> parse_program  (here: single fn_def)
//   fn_def         ::= fn_sig block                -> parse_fn_def
//   block          ::= '{' { statement } '}'       -> parse_block
//   statement      ::= declaration | simple_stmt   -> parse_statement (+ is_declaration lookahead)
//   declaration    ::= [mut] type IDENT '=' expr   -> parse_declaration
//   type           ::= ('i8'|'i32') {'*'} ['&']     -> parse_type (subset of EBNF `type`)
//   simple_stmt    ::= expr [assign_op expr]       -> parse_simple_stmt
//   expr/unary     ::= '&' unary | '*' unary | IDENT | INT
//
// All AST + scratch memory lives in `types.h` types + your `mem_arena`.
// No malloc/calloc/realloc/free here; arena_destroy() frees everything.
//
// Build: cc lexer.c parser.c arena.c -o taka && ./taka mut.tk

#include "lexer.h"
#include "types.h"
#include "arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------- AST (types.h scalars) ----------
typedef enum { EX_INT, EX_VAR, EX_ADDR, EX_DEREF } ExprKind;
typedef struct Expr {
    ExprKind kind;
    i64 ival;               // EX_INT
    char *name;             // EX_VAR (arena string)
    struct Expr *inner;     // EX_ADDR / EX_DEREF
} Expr;

typedef enum { ST_DECL, ST_ASSIGN } StmtKind;
typedef struct {
    StmtKind kind;
    // ST_DECL:
    b32 is_mut;
    TokenKind base;         // TOKEN_I8 / TOKEN_I32
    i32 stars;              // count of '*'
    b32 is_ref;             // trailing '&'
    char *name;
    Expr *init;
    // ST_ASSIGN:
    TokenKind op;           // TOKEN_EQ / TOKEN_PLUS_EQ
    Expr *lhs, *rhs;
} Stmt;

typedef struct {
    char *name;
    Stmt **stmts;
    i32 count, cap;
} FnDef;

// ---------- parser state ----------
typedef struct {
    Token *toks;
    i32 count;
    i32 pos;
    mem_arena *arena;
} Parser;

static Token *peek(Parser *p) { return &p->toks[p->pos]; }
static TokenKind peek_kind(Parser *p) { return p->toks[p->pos].kind; }
static b32 at_end(Parser *p) { return peek_kind(p) == TOKEN_EOF; }

static void parse_error(Token *t, const char *msg) {
    fprintf(stderr, "parse error at %d:%d near '%.*s' (%s): %s\n",
        t->line, t->col, (int)t->len, t->start, token_name(t->kind), msg);
    exit(1);
}

static Token *expect(Parser *p, TokenKind k, const char *what) {
    Token *t = peek(p);
    if (t->kind != k) {
        fprintf(stderr, "expected %s: ", what);
        parse_error(t, what);
    }
    p->pos++;
    return t;
}

static b32 match(Parser *p, TokenKind k) {
    if (peek_kind(p) == k) { p->pos++; return TRUE; }
    return FALSE;
}

// ---------- arena helpers (replace malloc/calloc/realloc) ----------
static void *xpush(mem_arena *arena, u64 size) {
    void *ptr = arena_push(arena, size, TRUE);
    if (!ptr) { fprintf(stderr, "out of memory\n"); exit(1); }
    return ptr;
}

static char *copy_str(mem_arena *arena, const char *s, u64 n) {
    char *out = (char *)arena_push(arena, n + 1, FALSE);
    if (!out) { fprintf(stderr, "out of memory\n"); exit(1); }
    memcpy(out, s, (size_t)n);
    out[n] = '\0';
    return out;
}

static Expr *new_expr(Parser *p) {
    Expr *e = (Expr *)arena_push(p->arena, sizeof(Expr), TRUE);
    if (!e) { fprintf(stderr, "out of memory\n"); exit(1); }
    return e;
}

static Stmt *new_stmt(Parser *p) {
    Stmt *s = (Stmt *)arena_push(p->arena, sizeof(Stmt), TRUE);
    if (!s) { fprintf(stderr, "out of memory\n"); exit(1); }
    return s;
}

// arena arrays can't realloc in place: allocate bigger + copy, abandon old.
static void *arena_grow(mem_arena *arena, void *old_ptr, u64 old_bytes, u64 new_bytes) {
    void *new_ptr = arena_push(arena, new_bytes, FALSE);
    if (!new_ptr) { fprintf(stderr, "out of memory\n"); exit(1); }
    memcpy(new_ptr, old_ptr, (size_t)old_bytes);
    return new_ptr;
}

// ---------- forward decls (one per EBNF rule) ----------
static FnDef *parse_fn_def(Parser *p);
static void parse_block(Parser *p, FnDef *fn);
static Stmt *parse_statement(Parser *p);
static Stmt *parse_declaration(Parser *p);
static Stmt *parse_simple_stmt(Parser *p);
static Expr *parse_expr(Parser *p);
static Expr *parse_unary(Parser *p);
static Expr *parse_primary(Parser *p);

// EBNF note (statements): try "[mut] type identifier" first,
// else fall back to simple_stmt. `a * b;` is a decl, not multiply.
static b32 is_declaration(Parser *p) {
    i32 i = p->pos;
    if (p->toks[i].kind == TOKEN_MUT) i++;
    if (p->toks[i].kind != TOKEN_I8 && p->toks[i].kind != TOKEN_I32 &&
        p->toks[i].kind != TOKEN_CONST && p->toks[i].kind != TOKEN_AUTO)
        return FALSE;
    // skip base type
    i++;
    // skip {'*'} ['&']  —  EBNF: type ::= ... {'*'} ['&']
    while (p->toks[i].kind == TOKEN_STAR) i++;
    if (p->toks[i].kind == TOKEN_AMP) i++;
    // then IDENT '=' ?
    if (p->toks[i].kind != TOKEN_IDENT) return FALSE;
    if (p->toks[i + 1].kind != TOKEN_EQ) return FALSE;
    return TRUE;
}

// program ::= fn_def  (mut.tk has exactly one item)
static FnDef *parse_program(Parser *p) {
    FnDef *fn = parse_fn_def(p);
    expect(p, TOKEN_EOF, "end of file");
    return fn;
}

// fn_def ::= 'fn' IDENT '(' ')' block
static FnDef *parse_fn_def(Parser *p) {
    expect(p, TOKEN_FN, "'fn'");
    Token *name = expect(p, TOKEN_IDENT, "function name");
    expect(p, TOKEN_LPAREN, "'('");
    expect(p, TOKEN_RPAREN, "')'");
    FnDef *fn = (FnDef *)xpush(p->arena, sizeof(FnDef));
    fn->name = copy_str(p->arena, name->start, name->len);
    fn->count = 0;
    fn->cap = 8;
    fn->stmts = (Stmt **)xpush(p->arena, sizeof(Stmt *) * (u64)fn->cap);
    parse_block(p, fn);
    return fn;
}

// block ::= '{' { statement } '}'
static void parse_block(Parser *p, FnDef *fn) {
    expect(p, TOKEN_LBRACE, "'{'");
    while (peek_kind(p) != TOKEN_RBRACE && !at_end(p)) {
        Stmt *s = parse_statement(p);
        if (fn->count == fn->cap) {
            i32 new_cap = fn->cap * 2;
            fn->stmts = (Stmt **)arena_grow(p->arena, fn->stmts,
                sizeof(Stmt *) * (u64)fn->cap, sizeof(Stmt *) * (u64)new_cap);
            fn->cap = new_cap;
        }
        fn->stmts[fn->count++] = s;
    }
    expect(p, TOKEN_RBRACE, "'}'");
}

static Stmt *parse_statement(Parser *p) {
    if (is_declaration(p)) return parse_declaration(p);
    Stmt *s = parse_simple_stmt(p);
    expect(p, TOKEN_SEMI, "';'");
    return s;
}

// variable_declaration ::= ['mut'] type IDENT '=' expr  (+ trailing ';' by caller)
static Stmt *parse_declaration(Parser *p) {
    Stmt *s = new_stmt(p);
    s->kind = ST_DECL;
    s->is_mut = match(p, TOKEN_MUT);
    // type ::= ('i8'|'i32') {'*'} ['&']  — subset of full EBNF `type`
    Token *base = peek(p);
    if (base->kind != TOKEN_I8 && base->kind != TOKEN_I32)
        parse_error(base, "expected type (i8/i32 in mut.tk subset)");
    s->base = base->kind;
    p->pos++;
    while (match(p, TOKEN_STAR)) s->stars++;
    if (match(p, TOKEN_AMP)) s->is_ref = TRUE;
    Token *name = expect(p, TOKEN_IDENT, "variable name");
    s->name = copy_str(p->arena, name->start, name->len);
    expect(p, TOKEN_EQ, "'=' (initializer is required)");
    s->init = parse_expr(p);
    expect(p, TOKEN_SEMI, "';'");
    return s;
}

// simple_stmt ::= expr [assign_op expr]  (assign_op subset: '=' | '+=')
static Stmt *parse_simple_stmt(Parser *p) {
    Stmt *s = new_stmt(p);
    s->kind = ST_ASSIGN;
    s->lhs = parse_expr(p);
    if (peek_kind(p) == TOKEN_EQ || peek_kind(p) == TOKEN_PLUS_EQ) {
        s->op = peek_kind(p);
        p->pos++;
        s->rhs = parse_expr(p);
    } else {
        parse_error(peek(p), "expected '=' or '+=' (mut.tk has only assignments)");
    }
    return s;
}

static Expr *parse_expr(Parser *p) { return parse_unary(p); }

// unary ::= '&' unary | '*' unary | primary
static Expr *parse_unary(Parser *p) {
    if (match(p, TOKEN_AMP)) {
        Expr *e = new_expr(p);
        e->kind = EX_ADDR;
        e->inner = parse_unary(p);
        return e;
    }
    if (match(p, TOKEN_STAR)) {
        Expr *e = new_expr(p);
        e->kind = EX_DEREF;
        e->inner = parse_unary(p);
        return e;
    }
    return parse_primary(p);
}

static Expr *parse_primary(Parser *p) {
    Token *t = peek(p);
    if (t->kind == TOKEN_INT) {
        p->pos++;
        Expr *e = new_expr(p);
        e->kind = EX_INT;
        // ints may contain '_' separators: strip them
        char buf[64];
        u64 n = 0;
        for (u64 i = 0; i < t->len && n < sizeof(buf) - 1; i++)
            if (t->start[i] != '_') buf[n++] = t->start[i];
        buf[n] = '\0';
        e->ival = (i64)atol(buf);
        return e;
    }
    if (t->kind == TOKEN_IDENT) {
        p->pos++;
        Expr *e = new_expr(p);
        e->kind = EX_VAR;
        e->name = copy_str(p->arena, t->start, t->len);
        return e;
    }
    parse_error(t, "expected expression (ident, int, or &x)");
    return NULL;
}

// ---------- pretty printer (shows the tree) ----------
static void print_expr(Expr *e) {
    switch (e->kind) {
        case EX_INT:  printf("%lld", (long long)e->ival); break;
        case EX_VAR:  printf("%s", e->name); break;
        case EX_ADDR: printf("&"); print_expr(e->inner); break;
        case EX_DEREF: printf("*"); print_expr(e->inner); break;
    }
}

static void print_fn(FnDef *fn) {
    printf("FnDef(%s)\n", fn->name);
    for (i32 i = 0; i < fn->count; i++) {
        Stmt *s = fn->stmts[i];
        printf("  ");
        if (s->kind == ST_DECL) {
            printf("Decl(%s%s%s%s %s = ",
                s->is_mut ? "mut " : "",
                token_name(s->base),
                s->stars ? "*" : "",
                s->is_ref ? "&" : "",
                s->name);
            print_expr(s->init);
            printf(")");
            // semantic note from EBNF 14d: `mut T&` is a later error, not a parse error
            if (s->is_mut && s->is_ref) printf("  <-- parses OK, rejected by checker (mut on &)");
            printf("\n");
        } else {
            print_expr(s->lhs);
            printf(" %s ", s->op == TOKEN_PLUS_EQ ? "+=" : "=");
            print_expr(s->rhs);
            printf("\n");
        }
    }
}

// ---------- C codegen (taka AST -> plain C) ----------
static const char *c_base_type(TokenKind k) {
    return k == TOKEN_I8 ? "int8_t" : "int32_t";
}

// References compile to C pointers with auto-deref at use sites.
static b32 is_ref_var(FnDef *fn, const char *name) {
    for (i32 i = 0; i < fn->count; i++) {
        Stmt *s = fn->stmts[i];
        if (s->kind == ST_DECL && s->is_ref && strcmp(s->name, name) == 0) return TRUE;
    }
    return FALSE;
}

static void emit_expr(FILE *out, FnDef *fn, Expr *e) {
    switch (e->kind) {
        case EX_INT:  fprintf(out, "%lld", (long long)e->ival); break;
        case EX_VAR:
            if (is_ref_var(fn, e->name)) fprintf(out, "(*%s)", e->name);
            else fprintf(out, "%s", e->name);
            break;
        case EX_ADDR: fprintf(out, "&"); emit_expr(out, fn, e->inner); break;
        case EX_DEREF: fprintf(out, "*"); emit_expr(out, fn, e->inner); break;
    }
}

// Decl mapping:
//   i8 a = 8        ->  int8_t a = 8;
//   i32* p = &a     ->  int32_t *p = (int32_t*)&a;   (cast: taka type error, C warning otherwise)
//   T& r = x        ->  T *r = &x;                   (rule 14b: implicit address-taking)
static void emit_decl(FILE *out, FnDef *fn, Stmt *s) {
    UNUSED(fn);
    const char *base = c_base_type(s->base);
    fprintf(out, "    %s ", base);
    for (i32 i = 0; i < s->stars; i++) fprintf(out, "*");
    if (s->is_ref) fprintf(out, "*");
    fprintf(out, "%s = ", s->name);
    if (s->is_ref) {
        // `T& r = x` binds without `&`; `T& r = &x` already has it.
        if (s->init->kind == EX_VAR) fprintf(out, "(%s*)&%s", base, s->init->name);
        else { fprintf(out, "(%s*)", base); emit_expr(out, fn, s->init); }
    } else if (s->stars > 0) {
        fprintf(out, "(%s", base);
        for (i32 i = 0; i < s->stars; i++) fprintf(out, "*");
        fprintf(out, ")");
        emit_expr(out, fn, s->init);
    } else {
        emit_expr(out, fn, s->init);
    }
    fprintf(out, ";");
    if (!s->is_mut) fprintf(out, "  // taka: non-mut, checker would reject later mutation");
    if (s->is_mut && s->is_ref) fprintf(out, "  // taka 14d: `mut` on `&` is a checker error");
    fprintf(out, "\n");
}

static void emit_assign(FILE *out, FnDef *fn, Stmt *s) {
    fprintf(out, "    ");
    emit_expr(out, fn, s->lhs);
    fprintf(out, " %s ", s->op == TOKEN_PLUS_EQ ? "+=" : "=");
    emit_expr(out, fn, s->rhs);
    fprintf(out, ";\n");
}

static void gen_c(FnDef *fn, const char *path) {
    FILE *out = fopen(path, "w");
    if (!out) { perror(path); exit(1); }
    fprintf(out, "// generated from mut.tk — do not edit\n");
    fprintf(out, "#include <stdint.h>\n#include <stdio.h>\n\n");
    fprintf(out, "int main(void) {\n");
    for (i32 i = 0; i < fn->count; i++) {
        Stmt *s = fn->stmts[i];
        if (s->kind == ST_DECL) emit_decl(out, fn, s);
        else emit_assign(out, fn, s);
    }
    // observable run: dump final state (demo only, not part of semantics)
    fprintf(out, "    printf(\"a=%%d x=%%d *p=%%d r=%%d\\n\", (int)a, (int)x, (int)*p, (int)(*r));\n");
    fprintf(out, "    return 0;\n}\n");
    fclose(out);
}

// ---------- driver ----------
static const char *read_file(mem_arena *arena, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)arena_push(arena, (u64)n + 1, FALSE);
    if (!buf) { fprintf(stderr, "out of memory\n"); exit(1); }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror("read"); exit(1); }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s file.tk\n", argv[0]); return 1; }

    mem_arena *arena = arena_create(MByte(1));
    if (!arena) { fprintf(stderr, "arena_create failed\n"); return 1; }

    const char *src = read_file(arena, argv[1]);

    // lex everything up front (simplest for learning; streaming also works)
    Lexer lx;
    lexer_init(&lx, src);
    i32 cap = 64, n = 0;
    Token *toks = (Token *)xpush(arena, sizeof(Token) * (u64)cap);
    for (;;) {
        if (n == cap) {
            i32 new_cap = cap * 2;
            toks = (Token *)arena_grow(arena, toks,
                sizeof(Token) * (u64)cap, sizeof(Token) * (u64)new_cap);
            cap = new_cap;
        }
        toks[n] = lexer_next(&lx);
        if (toks[n].kind == TOKEN_ERROR)
            parse_error(&toks[n], "bad token (lexer)");
        if (toks[n++].kind == TOKEN_EOF) break;
    }

    Parser p = {toks, n, 0, arena};
    FnDef *fn = parse_program(&p);
    print_fn(fn);
    printf("parse OK\n");

    gen_c(fn, "mut.gen.c");
    printf("wrote mut.gen.c\n");

    arena_destroy(arena);
    return 0;
}
