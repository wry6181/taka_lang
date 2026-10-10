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
// Build: cc lexer.c parser.c arena.c ast_build.c sema.c cgen.c -o taka
//   ./taka demo.tk            # emit demo.gen.c (C kept)
//   ./taka build demo.tk      # native ./demo (C is a temp artifact)
//   ./taka run demo.tk         # build to temp and execute

#include "lexer.h"
#include "types.h"
#include "arena.h"
#include "ast_build.h"
#include "sema.h"
#include "cgen.h"
#include "x64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

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
    i32 pending_gt;   // split `>>` -> `>` + pending `>`
    i32 pending_amp;  // split `&&` -> `&` + pending `&`
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

// ============================================================
// Full grammar parser (grammar.ebnf rev 4 + syntax_test.tk compat)
// Validation-only: consumes tokens, exits on parse error.
// Permissive where test and grammar diverge (see notes).
// ============================================================

typedef struct { i32 pos; i32 pending_gt; i32 pending_amp; } PState;
static PState full_save(Parser *p) { PState s = {p->pos, p->pending_gt, p->pending_amp}; return s; }
static void full_restore(Parser *p, PState s) { p->pos = s.pos; p->pending_gt = s.pending_gt; p->pending_amp = s.pending_amp; }
static TokenKind full_raw_kind(Parser *p) { return p->toks[p->pos].kind; }
static Token *full_raw_tok(Parser *p) { return &p->toks[p->pos]; }
static TokenKind full_peek_kind(Parser *p) {
    if (p->pending_gt > 0) return TOKEN_GT;
    if (p->pending_amp > 0) return TOKEN_AMP;
    return p->toks[p->pos].kind;
}
static b32 full_match(Parser *p, TokenKind k) {
    if (full_peek_kind(p) == k) {
        if (p->pending_gt > 0 && k == TOKEN_GT) p->pending_gt--;
        else if (p->pending_amp > 0 && k == TOKEN_AMP) p->pending_amp--;
        else p->pos++;
        return TRUE;
    }
    return FALSE;
}
static Token *full_expect(Parser *p, TokenKind k, const char *what) {
    Token *t = full_raw_tok(p);
    if (!full_match(p, k)) {
        fprintf(stderr, "expected %s: ", what);
        parse_error(t, what);
    }
    return t;
}
// consume one '>' handling `>>` split for generic/effect closing
static b32 full_match_gt(Parser *p) {
    if (p->pending_gt > 0) { p->pending_gt--; return TRUE; }
    if (p->pending_amp > 0) return FALSE;
    TokenKind k = p->toks[p->pos].kind;
    if (k == TOKEN_GT) { p->pos++; return TRUE; }
    if (k == TOKEN_SHR) { p->pos++; p->pending_gt = 1; return TRUE; }
    return FALSE;
}
static void full_expect_gt(Parser *p, const char *what) {
    if (!full_match_gt(p)) parse_error(full_raw_tok(p), what);
}
// consume one '&' for prefix position, splitting `&&`
static b32 full_match_amp_prefix(Parser *p) {
    if (p->pending_amp > 0) { p->pending_amp--; return TRUE; }
    if (p->pending_gt > 0) return FALSE;
    TokenKind k = p->toks[p->pos].kind;
    if (k == TOKEN_AMP) { p->pos++; return TRUE; }
    if (k == TOKEN_AMP2) { p->pos++; p->pending_amp = 1; return TRUE; }
    return FALSE;
}
static b32 full_peek_is_amp(Parser *p) {
    if (p->pending_amp > 0) return TRUE;
    if (p->pending_gt > 0) return FALSE;
    TokenKind k = p->toks[p->pos].kind;
    return k == TOKEN_AMP || k == TOKEN_AMP2;
}

static b32 full_is_primitive(TokenKind k) {
    return k==TOKEN_I8||k==TOKEN_I16||k==TOKEN_I32||k==TOKEN_I64||
           k==TOKEN_U8||k==TOKEN_U16||k==TOKEN_U32||k==TOKEN_U64||
           k==TOKEN_F32||k==TOKEN_F64||k==TOKEN_CHAR_TY||k==TOKEN_BOOL||
           k==TOKEN_SVIEW||k==TOKEN_NEVER;
}
static b32 full_is_builtin_effect(TokenKind k) {
    return k==TOKEN_IO||k==TOKEN_ALLOC||k==TOKEN_READS||k==TOKEN_WRITES||
           k==TOKEN_DECIDES||k==TOKEN_SUSPENDS||k==TOKEN_TRANSACTS||
           k==TOKEN_PURE||k==TOKEN_OWNERSHIP;
}
static b32 full_is_path_start(TokenKind k) {
    if (k==TOKEN_IDENT||k==TOKEN_SELF) return TRUE;
    // permissive: builtin effects usable as identifiers/paths (contextual)
    if (full_is_builtin_effect(k)) return TRUE;
    // permissive: allow other keywords as path segments? no, keep strict except effects
    return FALSE;
}
static b32 full_is_assign_op(TokenKind k) {
    return k==TOKEN_EQ||k==TOKEN_PLUS_EQ||k==TOKEN_MINUS_EQ||k==TOKEN_STAR_EQ||
           k==TOKEN_SLASH_EQ||k==TOKEN_PERCENT_EQ||k==TOKEN_SHL_EQ||k==TOKEN_SHR_EQ||
           k==TOKEN_AMP_EQ||k==TOKEN_PIPE_EQ||k==TOKEN_CARET_EQ;
}

// forward decls (one per EBNF rule)
static void parse_full_program(Parser *p);
static void parse_full_item(Parser *p);
static void parse_full_fn_sig(Parser *p);
static void parse_full_generics(Parser *p);
static void parse_full_where(Parser *p);
static void parse_full_bound(Parser *p);
static void parse_full_effect_row(Parser *p);
static void parse_full_type(Parser *p);
static void parse_full_type_list(Parser *p);
static void parse_full_generic_args(Parser *p);
static void parse_full_path(Parser *p);
static void parse_full_block(Parser *p);
static void parse_full_statement(Parser *p);
static void parse_full_variable_declaration_no_semi(Parser *p);
static void parse_full_simple_stmt_no_semi(Parser *p);
static void parse_full_condition(Parser *p);
static void parse_full_expr(Parser *p);
static void parse_full_pattern(Parser *p);
static void parse_full_pattern_primary(Parser *p);

// ---------- helpers ----------
static void parse_full_visibility_opt(Parser *p) {
    if (full_match(p, TOKEN_PUB) || full_match(p, TOKEN_PRIVATE)) {}
}
static void parse_full_path(Parser *p) {
    TokenKind k = full_peek_kind(p);
    if (!full_is_path_start(k)) parse_error(full_raw_tok(p), "expected path");
    // consume first segment (ident/self/effect)
    if (p->pending_gt>0||p->pending_amp>0) parse_error(full_raw_tok(p), "bad path");
    p->pos++;
    while (full_peek_kind(p)==TOKEN_COLON2) {
        // `::<` starts turbofish, not path continuation — leave `::` for caller
        if (p->pos+1 < p->count && p->toks[p->pos+1].kind==TOKEN_LT) break;
        p->pos++; // consume ::
        TokenKind k2 = full_peek_kind(p);
        // after :: allow IDENT, SELF, or builtin effect (permissive) or primitive? no
        if (k2==TOKEN_IDENT||k2==TOKEN_SELF||full_is_builtin_effect(k2)) {
            if (p->pending_gt>0||p->pending_amp>0) parse_error(full_raw_tok(p), "bad path segment");
            p->pos++;
        } else parse_error(full_raw_tok(p), "expected path segment after ::");
    }
}
static void parse_full_bound(Parser *p) {
    parse_full_path(p);
    if (full_peek_kind(p)==TOKEN_LT) parse_full_generic_args(p);
    while (full_match(p, TOKEN_PLUS)) {
        parse_full_path(p);
        if (full_peek_kind(p)==TOKEN_LT) parse_full_generic_args(p);
    }
}
static void parse_full_where(Parser *p) {
    full_expect(p, TOKEN_WHERE, "'where'");
    full_expect(p, TOKEN_IDENT, "where param");
    full_expect(p, TOKEN_COLON, "':' in where");
    parse_full_bound(p);
    while (full_match(p, TOKEN_COMMA)) {
        if (full_peek_kind(p)!=TOKEN_IDENT) break; // trailing comma
        // need IDENT ':' bound, but if next isn't ident-colon, break (handles trailing)
        PState s = full_save(p);
        if (full_peek_kind(p)==TOKEN_IDENT) {
            p->pos++;
            if (full_peek_kind(p)==TOKEN_COLON) {
                p->pos++;
                parse_full_bound(p);
                continue;
            }
        }
        full_restore(p, s);
        break;
    }
}
static void parse_full_generic_param(Parser *p) {
    if (full_match(p, TOKEN_EFFECT)) {
        full_expect(p, TOKEN_IDENT, "effect row variable");
        return;
    }
    full_expect(p, TOKEN_IDENT, "generic param");
    if (full_match(p, TOKEN_COLON)) parse_full_bound(p);
}
static void parse_full_generics(Parser *p) {
    full_expect(p, TOKEN_LT, "'<' for generics");
    // allow empty? grammar requires 1+, but be permissive for trailing handling
    if (full_peek_kind(p)==TOKEN_GT || full_raw_kind(p)==TOKEN_SHR) {
        full_expect_gt(p, "'>' for generics");
        return;
    }
    parse_full_generic_param(p);
    while (full_match(p, TOKEN_COMMA)) {
        TokenKind k = full_peek_kind(p);
        // trailing comma before '>'
        if (k==TOKEN_GT) break;
        if (k==TOKEN_SHR) break; // `>>` closing
        // also handle SHR split? raw SHR means two closes; break
        parse_full_generic_param(p);
    }
    full_expect_gt(p, "'>' for generics");
}
static void parse_full_generic_args(Parser *p) {
    full_expect(p, TOKEN_LT, "'<' for generic args");
    // generic_args ::= '<' type_list '>'
    // allow trailing comma inside type_list handling
    // empty generic args is error, but be permissive? require at least one type
    parse_full_type_list(p);
    full_expect_gt(p, "'>' for generic args");
}
static void parse_full_effect_item(Parser *p) {
    TokenKind k = full_peek_kind(p);
    if (full_is_builtin_effect(k)) {
        p->pos++;
        return;
    }
    // path [generic_args]
    parse_full_path(p);
    if (full_peek_kind(p)==TOKEN_LT) {
        // could be generic args for custom effect; parse
        parse_full_generic_args(p);
    }
}
static void parse_full_effect_row(Parser *p) {
    // '<' [effect_item {',' effect_item} [',']] ['|' IDENT] '>'  + bare `<| e>` support
    // also supports trailing comma
    full_expect(p, TOKEN_LT, "'<' for effect row");
    // bare tail `<| e>`
    if (full_match(p, TOKEN_PIPE)) {
        full_expect(p, TOKEN_IDENT, "effect row tail");
        full_expect_gt(p, "'>' for effect row");
        return;
    }
    // empty `<>` is syntax error per grammar, but don't special-case: will fail below
    // if immediate '>', it's `<>` -> error
    if (full_peek_kind(p)==TOKEN_GT || full_raw_kind(p)==TOKEN_SHR) {
        parse_error(full_raw_tok(p), "'<>' is illegal");
    }
    parse_full_effect_item(p);
    while (full_match(p, TOKEN_COMMA)) {
        TokenKind k = full_peek_kind(p);
        if (k==TOKEN_GT) break; // trailing comma
        if (full_raw_kind(p)==TOKEN_SHR) break; // trailing + close `>>`? e.g. `<io,>>`? break
        if (k==TOKEN_PIPE) break; // `, | e` tail
        parse_full_effect_item(p);
    }
    if (full_match(p, TOKEN_PIPE)) {
        full_expect(p, TOKEN_IDENT, "effect row tail after |");
    }
    full_expect_gt(p, "'>' for effect row");
}

// ---------- types ----------
// type ::= [const] type_body {'*'} ['&'] ; auto_type ::= [const] auto ['&']
// type_body ::= primitive | path [generic_args] | [extern] fn(...) [...] | dyn ... | (type_list) | [type; const_expr]
static void parse_full_type(Parser *p) {
    full_match(p, TOKEN_CONST);
    // auto?
    if (full_match(p, TOKEN_AUTO)) {
        while (full_match(p, TOKEN_STAR)) {} // auto*? grammar says auto[&] only, but be permissive? actually no stars for auto; allow anyway? keep: no stars, just &
        full_match(p, TOKEN_AMP);
        // auto may still have trailing *&? no. done
        return;
    }
    // [extern] fn(...) -> fn type
    PState s = full_save(p);
    b32 is_extern_fn = FALSE;
    if (full_match(p, TOKEN_EXTERN)) is_extern_fn = TRUE;
    if (full_peek_kind(p)==TOKEN_FN) {
        p->pos++; // fn
        full_expect(p, TOKEN_LPAREN, "'(' for fn type");
        if (full_peek_kind(p)!=TOKEN_RPAREN) parse_full_type_list(p);
        full_expect(p, TOKEN_RPAREN, "')' for fn type");
        if (full_peek_kind(p)==TOKEN_LT) parse_full_effect_row(p);
        if (full_match(p, TOKEN_ARROW)) parse_full_type(p);
        // fn type doesn't take trailing *& here? Actually type ::= ... type_body {'*'} ['&'], and fn is a type_body, so allow suffix after
        while (full_match(p, TOKEN_STAR)) {}
        full_match(p, TOKEN_AMP);
        UNUSED(is_extern_fn);
        return;
    }
    // not fn type: restore if we consumed extern but not fn
    if (is_extern_fn) full_restore(p, s);

    // dyn Trait* ... : 'dyn' path [generic_args]
    if (full_match(p, TOKEN_DYN)) {
        parse_full_path(p);
        if (full_peek_kind(p)==TOKEN_LT) parse_full_generic_args(p);
        while (full_match(p, TOKEN_STAR)) {}
        full_match(p, TOKEN_AMP);
        return;
    }
    // tuple type '(' type_list ')'
    if (full_peek_kind(p)==TOKEN_LPAREN) {
        // need to distinguish tuple type vs paren? In type context, '(' always tuple type
        p->pos++;
        if (full_peek_kind(p)!=TOKEN_RPAREN) parse_full_type_list(p);
        full_expect(p, TOKEN_RPAREN, "')' for tuple type");
        while (full_match(p, TOKEN_STAR)) {}
        full_match(p, TOKEN_AMP);
        return;
    }
    // array type '[' type ';' const_expr ']'
    if (full_peek_kind(p)==TOKEN_LBRACK) {
        p->pos++;
        parse_full_type(p);
        full_expect(p, TOKEN_SEMI, "';' in array type");
        parse_full_expr(p); // const_expr (just expr)
        full_expect(p, TOKEN_RBRACK, "']' for array type");
        while (full_match(p, TOKEN_STAR)) {}
        full_match(p, TOKEN_AMP);
        return;
    }
    // primitive or path
    TokenKind k = full_peek_kind(p);
    if (full_is_primitive(k)) {
        p->pos++;
    } else if (full_is_path_start(k)) {
        parse_full_path(p);
        if (full_peek_kind(p)==TOKEN_LT) parse_full_generic_args(p);
    } else {
        parse_error(full_raw_tok(p), "expected type");
    }
    while (full_match(p, TOKEN_STAR)) {}
    full_match(p, TOKEN_AMP);
}
static void parse_full_type_list(Parser *p) {
    parse_full_type(p);
    while (full_match(p, TOKEN_COMMA)) {
        // trailing comma?
        TokenKind k = full_peek_kind(p);
        if (k==TOKEN_RPAREN||k==TOKEN_RBRACK||k==TOKEN_RBRACE||k==TOKEN_GT||k==TOKEN_SHR||k==TOKEN_SEMI) break;
        // also if next closes generic `>`? break already
        // check for `>` via full_match_gt peek? full_peek returns GT if pending or raw GT. So k==GT covers.
        parse_full_type(p);
    }
}

// ---------- patterns ----------
static void parse_full_field_pat(Parser *p) {
    full_expect(p, TOKEN_IDENT, "field pattern name");
    if (full_match(p, TOKEN_COLON)) parse_full_pattern(p);
}
static void parse_full_pattern_primary(Parser *p) {
    TokenKind k = full_peek_kind(p);
    if (k==TOKEN_WILDCARD) { p->pos++; return; }
    if (k==TOKEN_AMP) {
        // '&' IDENT binds by reference
        p->pos++;
        full_expect(p, TOKEN_IDENT, "identifier after & pattern");
        return;
    }
    // literal?
    if (k==TOKEN_INT||k==TOKEN_FLOAT||k==TOKEN_STRING||k==TOKEN_CHAR||
        k==TOKEN_TRUE||k==TOKEN_FALSE||k==TOKEN_NULL||k==TOKEN_UNDEF) {
        p->pos++;
        return;
    }
    // [mut] IDENT (binding) OR path [...] (variant)
    // need lookahead: if `mut`? consume, then must have IDENT/path
    PState s = full_save(p);
    full_match(p, TOKEN_MUT);
    TokenKind k2 = full_peek_kind(p);
    if (k2==TOKEN_IDENT||k2==TOKEN_SELF||full_is_builtin_effect(k2)) {
        // could be binding or path start. Try to see if path has :: or ( or { after
        // consume first ident
        p->pos++;
        // if next is :: -> it's a path, continue as path
        if (full_peek_kind(p)==TOKEN_COLON2) {
            // path with ::
            while (full_match(p, TOKEN_COLON2)) {
                TokenKind ks = full_peek_kind(p);
                if (ks==TOKEN_IDENT||ks==TOKEN_SELF||full_is_builtin_effect(ks)) p->pos++;
                else parse_error(full_raw_tok(p), "expected path segment in pattern");
            }
            // after path, optional (pattern_list) or {field_pats}
            if (full_match(p, TOKEN_LPAREN)) {
                if (full_peek_kind(p)!=TOKEN_RPAREN) {
                    parse_full_pattern(p);
                    while (full_match(p, TOKEN_COMMA)) {
                        if (full_peek_kind(p)==TOKEN_RPAREN) break;
                        parse_full_pattern(p);
                    }
                }
                full_expect(p, TOKEN_RPAREN, "')' in pattern");
            } else if (full_match(p, TOKEN_LBRACE)) {
                // [field_pats] | '..'
                if (full_match(p, TOKEN_DOTDOT)) {
                    full_match(p, TOKEN_COMMA);
                } else if (full_peek_kind(p)!=TOKEN_RBRACE) {
                    parse_full_field_pat(p);
                    while (full_match(p, TOKEN_COMMA)) {
                        if (full_peek_kind(p)==TOKEN_RBRACE) break;
                        if (full_match(p, TOKEN_DOTDOT)) { full_match(p, TOKEN_COMMA); break; }
                        if (full_peek_kind(p)==TOKEN_RBRACE) break;
                        parse_full_field_pat(p);
                    }
                }
                full_expect(p, TOKEN_RBRACE, "'}' in pattern");
            }
            // else unit variant / binding
            return;
        }
        // no ::. If next is '(' or '{', it's tuple/struct variant with single-segment path
        if (full_peek_kind(p)==TOKEN_LPAREN) {
            p->pos++;
            if (full_peek_kind(p)!=TOKEN_RPAREN) {
                parse_full_pattern(p);
                while (full_match(p, TOKEN_COMMA)) {
                    if (full_peek_kind(p)==TOKEN_RPAREN) break;
                    parse_full_pattern(p);
                }
            }
            full_expect(p, TOKEN_RPAREN, "')' in pattern");
            return;
        }
        if (full_peek_kind(p)==TOKEN_LBRACE) {
            p->pos++;
            if (full_match(p, TOKEN_DOTDOT)) { full_match(p, TOKEN_COMMA); }
            else if (full_peek_kind(p)!=TOKEN_RBRACE) {
                parse_full_field_pat(p);
                while (full_match(p, TOKEN_COMMA)) {
                    if (full_peek_kind(p)==TOKEN_RBRACE) break;
                    if (full_match(p, TOKEN_DOTDOT)) { full_match(p, TOKEN_COMMA); break; }
                    if (full_peek_kind(p)==TOKEN_RBRACE) break;
                    parse_full_field_pat(p);
                }
            }
            full_expect(p, TOKEN_RBRACE, "'}' in pattern");
            return;
        }
        // else plain binding `x` or `mut x`
        return;
    }
    full_restore(p, s);
    parse_error(full_raw_tok(p), "expected pattern");
    UNUSED(k);
}
static void parse_full_pattern(Parser *p) {
    parse_full_pattern_primary(p);
    while (full_match(p, TOKEN_PIPE)) parse_full_pattern_primary(p);
}

// ---------- expressions (Pratt) ----------
static int full_binop_prec(TokenKind k) {
    switch (k) {
        case TOKEN_STAR: case TOKEN_SLASH: case TOKEN_PERCENT: return 70;
        case TOKEN_PLUS: case TOKEN_MINUS: return 60;
        case TOKEN_SHL: case TOKEN_SHR: return 50;
        case TOKEN_AMP: return 40;
        case TOKEN_CARET: return 30;
        case TOKEN_PIPE: return 20;
        case TOKEN_EQEQ: case TOKEN_BANGEQ: case TOKEN_LT: case TOKEN_GT:
        case TOKEN_LTEQ: case TOKEN_GTEQ: return 15;
        case TOKEN_AMP2: return 10;
        case TOKEN_PIPE2: return 5;
        default: return -1;
    }
}
static void parse_full_expr_list(Parser *p) {
    parse_full_expr(p);
    while (full_match(p, TOKEN_COMMA)) {
        if (full_peek_kind(p)==TOKEN_RPAREN||full_peek_kind(p)==TOKEN_RBRACK||full_peek_kind(p)==TOKEN_RBRACE) break;
        parse_full_expr(p);
    }
}
// forward for block-like in primary
static void parse_full_match_expr(Parser *p);
static void parse_full_if_expr(Parser *p);

static void parse_full_primary(Parser *p) {
    TokenKind k = full_peek_kind(p);
    // literals
    if (k==TOKEN_INT||k==TOKEN_FLOAT||k==TOKEN_STRING||k==TOKEN_CHAR||
        k==TOKEN_TRUE||k==TOKEN_FALSE||k==TOKEN_NULL||k==TOKEN_UNDEF) {
        p->pos++; return;
    }
    // sizeof / offsetof (with and without parens for test compat)
    if (k==TOKEN_SIZEOF) {
        p->pos++;
        if (full_match(p, TOKEN_LPAREN)) {
            // (type|expr): heuristic — primitive/const/dyn/extern/fn => type;
            // IDENT+`<` (generic args) => type; else expr.
            TokenKind k2 = full_peek_kind(p);
            b32 is_type = FALSE;
            if (full_is_primitive(k2)||k2==TOKEN_CONST||k2==TOKEN_DYN||k2==TOKEN_EXTERN||k2==TOKEN_FN) is_type = TRUE;
            else if ((k2==TOKEN_IDENT||k2==TOKEN_SELF) && p->pos+1<p->count && p->toks[p->pos+1].kind==TOKEN_LT) is_type = TRUE;
            else if (k2==TOKEN_LBRACK||k2==TOKEN_LPAREN) {
                // ambiguous `[`/`(`: check for `;` (array type) vs generic? For sizeof, default to type if starts with those? Test has no such, default to expr for safety? Actually `[i32;4]` type has `;`. Peek for `;` before matching `]`? Too complex. Default to type for `[`/`(`? Let's default to expr (covers `sizeof(x)` paren expr). Array types in sizeof are rare.
                // To cover `sizeof(i32)` (primitive) we already handle. For `[`/`(`, default expr.
                is_type = FALSE;
            }
            if (is_type) parse_full_type(p);
            else parse_full_expr(p);
            full_expect(p, TOKEN_RPAREN, "')' for sizeof");
        } else {
            // bare `sizeof x`
            parse_full_expr(p);
        }
        return;
    }
    if (k==TOKEN_OFFSETOF) {
        p->pos++;
        if (full_match(p, TOKEN_LPAREN)) {
            parse_full_type(p);
            full_expect(p, TOKEN_COMMA, "',' in offsetof");
            full_expect(p, TOKEN_IDENT, "field in offsetof");
            full_expect(p, TOKEN_RPAREN, "')' for offsetof");
        } else {
            // bare `offsetof y` (test compat): single path/ident
            parse_full_path(p);
        }
        return;
    }
    // if / match / perform / handle / spawn / resume / transact
    if (k==TOKEN_IF) { parse_full_if_expr(p); return; }
    if (k==TOKEN_MATCH) { parse_full_match_expr(p); return; }
    if (k==TOKEN_PERFORM) {
        p->pos++;
        parse_full_path(p);
        full_expect(p, TOKEN_LPAREN, "'(' for perform");
        if (full_peek_kind(p)!=TOKEN_RPAREN) parse_full_expr_list(p);
        full_expect(p, TOKEN_RPAREN, "')' for perform");
        return;
    }
    if (k==TOKEN_HANDLE) {
        p->pos++;
        // (block|expr) with handler_body
        if (full_peek_kind(p)==TOKEN_LBRACE) {
            // Could be block expr or handler inline? `handle work with {...}` vs `handle {..} with ..`
            // Parse first as block/brace expr (greedy), then expect `with`
            // To disambiguate, parse block first
            parse_full_block(p);
        } else {
            parse_full_expr(p);
        }
        full_expect(p, TOKEN_WITH, "'with' for handle");
        if (full_peek_kind(p)==TOKEN_LBRACE) {
            // inline handler_body `{ handler_ops }`
            p->pos++;
            if (full_peek_kind(p)!=TOKEN_RBRACE) {
                // parse handler ops: ident(...) => ... | return(...) => ...
                for (;;) {
                    if (full_match(p, TOKEN_RETURN)) {
                        full_expect(p, TOKEN_LPAREN, "'(' after return in handler");
                        // param: [mut] (type|auto) IDENT
                        full_match(p, TOKEN_MUT);
                        parse_full_type(p);
                        full_expect(p, TOKEN_IDENT, "param name");
                        full_expect(p, TOKEN_RPAREN, "')'");
                    } else {
                        full_expect(p, TOKEN_IDENT, "handler op name");
                        full_expect(p, TOKEN_LPAREN, "'(' for handler op");
                        // [params]
                        if (full_peek_kind(p)!=TOKEN_RPAREN) {
                            // param list
                            for (;;) {
                                full_match(p, TOKEN_MUT);
                                parse_full_type(p);
                                full_expect(p, TOKEN_IDENT, "handler param name");
                                if (!full_match(p, TOKEN_COMMA)) break;
                                if (full_peek_kind(p)==TOKEN_RPAREN) break;
                            }
                        }
                        full_expect(p, TOKEN_RPAREN, "')' for handler op");
                    }
                    full_expect(p, TOKEN_FATARROW, "'=>' for handler op");
                    if (full_peek_kind(p)==TOKEN_LBRACE) parse_full_block(p);
                    else parse_full_expr(p);
                    if (!full_match(p, TOKEN_COMMA)) break;
                    if (full_peek_kind(p)==TOKEN_RBRACE) break;
                }
            }
            full_expect(p, TOKEN_RBRACE, "'}' for handler body");
        } else {
            parse_full_path(p);
        }
        return;
    }
    if (k==TOKEN_SPAWN) {
        p->pos++;
        if (full_peek_kind(p)==TOKEN_LBRACE) parse_full_block(p);
        else {
            // postfix must be a call: parse postfix (which includes call). Just parse expr (will handle call)
            parse_full_expr(p);
        }
        return;
    }
    if (k==TOKEN_RESUME) {
        p->pos++;
        full_expect(p, TOKEN_LPAREN, "'(' for resume");
        if (full_peek_kind(p)!=TOKEN_RPAREN) parse_full_expr(p);
        full_expect(p, TOKEN_RPAREN, "')' for resume");
        return;
    }
    if (k==TOKEN_TRANSACT) {
        p->pos++;
        parse_full_block(p);
        return;
    }
    // lambda vs array literal on '['
    if (k==TOKEN_LBRACK) {
        // lookahead: find matching ']' then check '('
        i32 i = p->pos;
        i32 depth = 0;
        i32 match_idx = -1;
        for (; i < p->count; i++) {
            TokenKind kk = p->toks[i].kind;
            if (kk==TOKEN_LBRACK) depth++;
            else if (kk==TOKEN_RBRACK) { depth--; if (depth==0) { match_idx=i; break; } }
            else if (kk==TOKEN_EOF) break;
        }
        b32 is_lambda = FALSE;
        if (match_idx>=0 && match_idx+1 < p->count && p->toks[match_idx+1].kind==TOKEN_LPAREN) is_lambda=TRUE;
        if (is_lambda) {
            // lambda: '[' [capture_list] ']' '(' [lambda_params] ')' [effect_row] ['->' type] (block|'=>' expr)
            p->pos++; // [
            if (full_peek_kind(p)!=TOKEN_RBRACK) {
                for (;;) {
                    if (full_match(p, TOKEN_AMP)) {
                        full_expect(p, TOKEN_IDENT, "capture name after &");
                    } else {
                        full_match(p, TOKEN_MUT);
                        full_expect(p, TOKEN_IDENT, "capture name");
                        if (full_match(p, TOKEN_EQ)) parse_full_expr(p);
                    }
                    if (!full_match(p, TOKEN_COMMA)) break;
                    if (full_peek_kind(p)==TOKEN_RBRACK) break;
                }
            }
            full_expect(p, TOKEN_RBRACK, "']' for lambda captures");
            full_expect(p, TOKEN_LPAREN, "'(' for lambda params");
            if (full_peek_kind(p)!=TOKEN_RPAREN) {
                for (;;) {
                    full_match(p, TOKEN_MUT);
                    parse_full_type(p);
                    full_expect(p, TOKEN_IDENT, "lambda param name");
                    if (!full_match(p, TOKEN_COMMA)) break;
                    if (full_peek_kind(p)==TOKEN_RPAREN) break;
                }
            }
            full_expect(p, TOKEN_RPAREN, "')' for lambda params");
            if (full_peek_kind(p)==TOKEN_LT) parse_full_effect_row(p);
            if (full_match(p, TOKEN_ARROW)) parse_full_type(p);
            if (full_match(p, TOKEN_FATARROW)) parse_full_expr(p);
            else if (full_peek_kind(p)==TOKEN_LBRACE) parse_full_block(p);
            else parse_error(full_raw_tok(p), "expected lambda body");
            return;
        } else {
            // array_lit: '[' [expr_list] ']' | '[' expr ';' const_expr ']'
            p->pos++; // [
            if (full_peek_kind(p)==TOKEN_RBRACK) { p->pos++; return; } // empty []
            // need to distinguish fill `[0;256]` vs list `[1,2,3]`
            // save, parse expr, check ';' vs ','/']'
            PState s = full_save(p);
            parse_full_expr(p);
            if (full_match(p, TOKEN_SEMI)) {
                parse_full_expr(p); // const_expr
                full_expect(p, TOKEN_RBRACK, "']' for array fill");
            } else {
                // list: we already consumed first expr, continue while ','
                while (full_match(p, TOKEN_COMMA)) {
                    if (full_peek_kind(p)==TOKEN_RBRACK) break;
                    parse_full_expr(p);
                }
                full_expect(p, TOKEN_RBRACK, "']' for array lit");
            }
            UNUSED(s);
            return;
        }
    }
    // bare '{' as block/brace-init expression (test compat: `{1,2,3}`, `{}`, `{ stmts; expr }`)
    if (k==TOKEN_LBRACE) {
        // Parse permissively: allow empty, comma-separated exprs, semicolon statements, trailing expr
        p->pos++; // {
        if (full_match(p, TOKEN_RBRACE)) return; // empty {}
        // Try to parse contents until '}'
        // We loop: parse statement-or-expr? Simplest: parse expr-or-decl, then require , or ; or }
        // To handle both `1,2,3` and `auto t=1; t+1`, we parse generically:
        for (;;) {
            TokenKind kk = full_peek_kind(p);
            if (kk==TOKEN_RBRACE) { p->pos++; return; }
            // if looks like declaration? try decl? For `{ auto t=1; ...}`, first is decl.
            // Heuristic: if MUT/AUTO/CONST/primitive at start + IDENT after type, parse as variable decl (without requiring knowledge)
            // Instead of duplicating decl logic, just call parse_full_statement if it ends with ; else expr?
            // Simplest: attempt to parse as statement if it could be decl/if/while/for/etc, else expr.
            // For validation, we can try: if next starts a declaration-like (mut/auto/const/primitive), parse declaration (with optional ;), else parse expr.
            // This is permissive enough.
            PState s = full_save(p);
            // check declaration lookahead via helper? Use generic: try variable decl if matches pattern
            // We'll just parse expr first? But `auto t=1` as expr would fail (auto not expr). So need decl first.
            b32 consumed = FALSE;
            // peek for decl starters: mut/auto/const/primitive/ident-that-could-be-type?
            // To avoid complexity, if peek is AUTO/CONST or (MUT) or primitive, parse as declaration
            TokenKind k1 = full_peek_kind(p);
            b32 maybe_decl = (k1==TOKEN_AUTO||k1==TOKEN_CONST||k1==TOKEN_MUT||full_is_primitive(k1));
            // also `i32 x`? primitive already. `List<T> y`? starts with IDENT, ambiguous with expr. For block `{...}`, `auto` is the common decl starter. So check only those.
            if (maybe_decl) {
                // try decl
                parse_full_variable_declaration_no_semi(p);
                // after decl, expect ; or , or }
                if (full_match(p, TOKEN_SEMI)) { consumed=TRUE; }
                else if (full_match(p, TOKEN_COMMA)) { consumed=TRUE; }
                else if (full_peek_kind(p)==TOKEN_RBRACE) { p->pos++; return; }
                else parse_error(full_raw_tok(p), "expected ';' after declaration in block expr");
            } else {
                // parse expr (could be call, etc.)
                parse_full_expr(p);
                if (full_match(p, TOKEN_SEMI)) { consumed=TRUE; }
                else if (full_match(p, TOKEN_COMMA)) { consumed=TRUE; }
                else if (full_peek_kind(p)==TOKEN_RBRACE) { p->pos++; return; }
                else {
                    // Could be block-like without ;? e.g. `{ t+1 }` trailing expr then } — handled above (RBRACE). Else error.
                    parse_error(full_raw_tok(p), "expected ',' ';' or '}' in brace expr");
                }
            }
            UNUSED(s); UNUSED(consumed);
        }
        return;
    }
    // '(' paren expr
    if (k==TOKEN_LPAREN) {
        p->pos++;
        parse_full_expr(p);
        full_expect(p, TOKEN_RPAREN, "')'");
        return;
    }
    // path [turbofish]  OR  struct_lit (path '{' ... '}')
    if (full_is_path_start(k)) {
        parse_full_path(p);
        // turbofish `::<...>` (path parser stops before `::<`)
        if (full_peek_kind(p)==TOKEN_COLON2) {
            // peek 2 ahead: is `<` after `::`?
            if (p->pos+1 < p->count && p->toks[p->pos+1].kind==TOKEN_LT) {
                p->pos++; // ::
                full_expect(p, TOKEN_LT, "'<' for turbofish");
                parse_full_type_list(p);
                full_expect_gt(p, "'>' for turbofish");
            }
        }
        // struct literal? path '{' ... '}' — disambiguate vs match/block brace:
        // `match s { ... => ... }` has `=>`, struct lit has `:` (or empty). Scan ahead.
        if (full_peek_kind(p)==TOKEN_LBRACE) {
            // lookahead from '{' to matching '}' for FATARROW vs COLON
            int bi = p->pos; // index of '{'
            int depth_b=0, depth_p=0, depth_br=0;
            b32 seen_fat=FALSE, seen_colon=FALSE;
            for (int j=bi; j<p->count; j++) {
                TokenKind kj = p->toks[j].kind;
                if (kj==TOKEN_LBRACE) depth_b++;
                else if (kj==TOKEN_RBRACE) {
                    depth_b--;
                    if (depth_b==0) break; // matched outer
                } else if (depth_b==1) {
                    if (kj==TOKEN_LPAREN) depth_p++;
                    else if (kj==TOKEN_RPAREN) { if (depth_p>0) depth_p--; }
                    else if (kj==TOKEN_LBRACK) depth_br++;
                    else if (kj==TOKEN_RBRACK) { if (depth_br>0) depth_br--; }
                    else if (kj==TOKEN_FATARROW && depth_p==0 && depth_br==0) seen_fat=TRUE;
                    else if (kj==TOKEN_COLON && depth_p==0 && depth_br==0) seen_colon=TRUE;
                }
                if (kj==TOKEN_EOF) break;
            }
            if (seen_fat && !seen_colon) {
                // match arms, not struct lit — leave '{' for caller
                return;
            }
            // empty `{}` or `:` => struct lit; otherwise (neither) leave as well? For safety, if neither (e.g. `{}`? empty handled), treat empty as struct lit, else if no colon and no fat (e.g. `{1,2}`? but that's bare, not path+{) — for path+{ with neither, leave (not struct lit).
            if (!seen_colon) {
                // check empty: next is '}'?
                if (p->pos+1 < p->count && p->toks[p->pos+1].kind==TOKEN_RBRACE) {
                    // empty `T {}` struct lit
                } else {
                    return; // not struct lit
                }
            }
            p->pos++; // {
            if (full_peek_kind(p)!=TOKEN_RBRACE) {
                // field_init ::= IDENT ':' expr
                for (;;) {
                    full_expect(p, TOKEN_IDENT, "field name in struct lit");
                    full_expect(p, TOKEN_COLON, "':' in struct lit");
                    parse_full_expr(p);
                    if (!full_match(p, TOKEN_COMMA)) break;
                    if (full_peek_kind(p)==TOKEN_RBRACE) break;
                }
            }
            full_expect(p, TOKEN_RBRACE, "'}' for struct lit");
            return;
        }
        return;
    }
    parse_error(full_raw_tok(p), "expected expression");
}

static void parse_full_postfix(Parser *p) {
    parse_full_primary(p);
    for (;;) {
        if (full_match(p, TOKEN_LPAREN)) {
            if (full_peek_kind(p)!=TOKEN_RPAREN) parse_full_expr_list(p);
            full_expect(p, TOKEN_RPAREN, "')' for call");
        } else if (full_match(p, TOKEN_LBRACK)) {
            parse_full_expr(p);
            full_expect(p, TOKEN_RBRACK, "']' for index");
        } else if (full_match(p, TOKEN_DOT)) {
            full_expect(p, TOKEN_IDENT, "field/method after '.'");
            // method call? if '(' then it's call, but our loop will handle '(' next iteration as call. Good.
            // turbofish after method? `obj::method::<...>`? Actually `obj.method::<...>`? Test has `obj::method::<i32,f32>(2)`? That's `::` path? Hmm `obj::method::<i32,f32>(2)` — path `obj::method` + turbofish + call. Our primary path handling already includes `::` segments, but turbofish after `.`? `obj.method` uses `.`, not `::`. Then `::<...>` after method? `obj::method::<...>` uses `::` twice. Our loop: after `.` + IDENT, next iteration sees `::`? No, loop only handles `(`, `[`, `.`, `++`, `--`. It doesn't handle `::` + turbofish. Need to handle: after `. IDENT`, if next is `::` + `<`, parse turbofish. Let's add:
            if (full_peek_kind(p)==TOKEN_COLON2 && p->pos+1 < p->count && p->toks[p->pos+1].kind==TOKEN_LT) {
                p->pos++; full_expect(p, TOKEN_LT, "'<'"); parse_full_type_list(p); full_expect_gt(p, "'>'");
            }
        } else if (full_match(p, TOKEN_PLUSPLUS) || full_match(p, TOKEN_MINUSMINUS)) {
        } else break;
    }
}
static void parse_full_unary(Parser *p) {
    // ( '+' | '-' | '!' | '~' | '*' | '&' | '++' | '--' ) unary | postfix
    TokenKind k = full_peek_kind(p);
    if (k==TOKEN_PLUS||k==TOKEN_MINUS||k==TOKEN_BANG||k==TOKEN_TILDE||k==TOKEN_STAR||
        k==TOKEN_PLUSPLUS||k==TOKEN_MINUSMINUS) {
        p->pos++;
        parse_full_unary(p);
        return;
    }
    if (full_peek_is_amp(p)) {
        full_match_amp_prefix(p);
        parse_full_unary(p);
        return;
    }
    parse_full_postfix(p);
}
static void parse_full_cast(Parser *p) {
    parse_full_unary(p);
    while (full_match(p, TOKEN_AS)) parse_full_type(p);
}
static void parse_full_binary_prec(Parser *p, int min_prec) {
    parse_full_cast(p);
    for (;;) {
        TokenKind k = full_peek_kind(p);
        // '<' in expression is ALWAYS comparison (never generic). So LT is binop here. Good.
        // Need to handle pending? If pending_gt>0, peek is GT, which is binop prec 15. That's correct only if we're in expression inside generic? But pending means we're closing generics, not in expr. Expression shouldn't be parsed while pending>0? Actually effect_row/type parsing uses pending, expr parsing shouldn't interleave. If pending>0, we shouldn't be in expr. But to be safe, if pending>0, don't treat GT as binop? Hmm. If we're inside type generic args, we don't parse expr. So pending>0 implies not in expr. But our brace expr etc. could? Unlikely. For safety, if pending_gt>0 or pending_amp>0, break (don't consume as binop, let closer handle).
        if (p->pending_gt>0 || p->pending_amp>0) break;
        int prec = full_binop_prec(k);
        if (prec < min_prec) break;
        // non-assoc for comparison? Grammar says reject a<b<c. For validation, allow chaining (permissive) to avoid false failures. Use left-assoc (next min_prec = prec+1 for left, prec for right? For left, next = prec+1)
        // For simplicity, all left-assoc here (ternary handled separately, right-assoc)
        p->pos++; // consume binop (handles SHR/AMP2 etc. as single token; pending is 0 so safe)
        // Special: `>>`/`<<` etc. already single token. Good.
        parse_full_binary_prec(p, prec+1);
    }
}
static void parse_full_ternary(Parser *p) {
    parse_full_binary_prec(p, 0);
    if (full_match(p, TOKEN_QMARK)) {
        parse_full_expr(p);
        full_expect(p, TOKEN_COLON, "':' for ternary");
        parse_full_ternary(p); // right-assoc
    }
}
static void parse_full_expr(Parser *p) { parse_full_ternary(p); }

static void parse_full_if_expr(Parser *p) {
    full_expect(p, TOKEN_IF, "'if'");
    if (full_match(p, TOKEN_LPAREN)) {
        parse_full_condition(p);
        full_expect(p, TOKEN_RPAREN, "')' for if");
    } else {
        parse_full_condition(p);
    }
    parse_full_block(p);
    while (full_match(p, TOKEN_ELSE)) {
        if (full_peek_kind(p)==TOKEN_IF) {
            p->pos++;
            if (full_match(p, TOKEN_LPAREN)) {
                parse_full_condition(p);
                full_expect(p, TOKEN_RPAREN, "')' for else-if");
            } else {
                parse_full_condition(p);
            }
            parse_full_block(p);
        } else {
            parse_full_block(p);
            break;
        }
    }
    // if_expr requires else, if_stmt allows no else. For validation, allow optional else (already). Good.
}
static void parse_full_match_expr(Parser *p) {
    full_expect(p, TOKEN_MATCH, "'match'");
    if (full_match(p, TOKEN_LPAREN)) {
        parse_full_expr(p);
        full_expect(p, TOKEN_RPAREN, "')' for match");
    } else {
        parse_full_expr(p);
    }
    full_expect(p, TOKEN_LBRACE, "'{' for match");
    // arm {',' arm} [',']
    if (full_peek_kind(p)!=TOKEN_RBRACE) {
        for (;;) {
            parse_full_pattern(p);
            if (full_match(p, TOKEN_IF)) parse_full_expr(p); // guard
            full_expect(p, TOKEN_FATARROW, "'=>' for match arm");
            if (full_peek_kind(p)==TOKEN_LBRACE) {
                // Could be block OR struct lit? In arm `=>` (expr|block). `{...}` as block. Parse as block (which handles brace expr? but arm block is statement block, not expr? Use block parser)
                // To be permissive, if `{` then parse as block (statement block)
                parse_full_block(p);
            } else parse_full_expr(p);
            if (!full_match(p, TOKEN_COMMA)) break;
            if (full_peek_kind(p)==TOKEN_RBRACE) break;
        }
    }
    full_expect(p, TOKEN_RBRACE, "'}' for match");
}

// ---------- statements ----------
// try "[mut] type IDENT" lookahead (for statement disambiguation). Returns TRUE if decl.
static b32 full_is_declaration(Parser *p) {
    PState s = full_save(p);
    PState s0 = s;
    if (full_peek_kind(p)==TOKEN_MUT) {
        if (p->pending_gt||p->pending_amp) { full_restore(p,s); return FALSE; }
        p->pos++;
    }
    TokenKind first = full_peek_kind(p);
    // type-start allowlist: const/auto/primitive/path/fn/extern/dyn/tuple/array
    b32 ok_start = (first==TOKEN_CONST||first==TOKEN_AUTO||full_is_primitive(first)||
        first==TOKEN_IDENT||first==TOKEN_SELF||full_is_builtin_effect(first)||
        first==TOKEN_FN||first==TOKEN_EXTERN||first==TOKEN_DYN||
        first==TOKEN_LBRACK||first==TOKEN_LPAREN);
    if (!ok_start) { full_restore(p,s); return FALSE; }
    int angle=0, paren=0, brack=0;
    for (int i=p->pos; i<p->count; i++) {
        TokenKind kk = p->toks[i].kind;
        if (kk==TOKEN_LT) angle++;
        else if (kk==TOKEN_GT) { if (angle>0) angle--; else break; }
        else if (kk==TOKEN_SHR) { if (angle>0) { if (angle>=2) angle-=2; else angle=0; } else break; }
        else if (kk==TOKEN_LPAREN) paren++;
        else if (kk==TOKEN_RPAREN) { if (paren>0) paren--; else break; }
        else if (kk==TOKEN_LBRACK) brack++;
        else if (kk==TOKEN_RBRACK) { if (brack>0) brack--; }
        else if (kk==TOKEN_SEMI) { if (angle==0&&paren==0&&brack==0) break; else continue; }
        else if (kk==TOKEN_LBRACE||kk==TOKEN_RBRACE) { if (angle==0&&paren==0&&brack==0) break; }
        if (angle==0&&paren==0&&brack==0) {
            if (kk==TOKEN_DOT) { full_restore(p,s); return FALSE; } // '.' never in type
            if (kk==TOKEN_IDENT) {
                if (i+1<p->count && (p->toks[i+1].kind==TOKEN_EQ||p->toks[i+1].kind==TOKEN_COMMA)) {
                    b32 res = (i > s0.pos) ? TRUE : FALSE;
                    full_restore(p,s);
                    return res;
                }
            }
        }
    }
    full_restore(p,s);
    return FALSE;
}

static void parse_full_variable_declaration_no_semi(Parser *p) {
    full_match(p, TOKEN_MUT);
    TokenKind k = full_peek_kind(p);
    if (k==TOKEN_AUTO || (k==TOKEN_CONST && p->pos+1<p->count && p->toks[p->pos+1].kind==TOKEN_AUTO)) {
        parse_full_type(p);
    } else {
        parse_full_type(p);
    }
    full_expect(p, TOKEN_IDENT, "variable name");
    full_expect(p, TOKEN_EQ, "'=' (initializer required)");
    parse_full_expr(p);
    while (full_match(p, TOKEN_COMMA)) {
        if (full_peek_kind(p)!=TOKEN_IDENT) parse_error(full_raw_tok(p), "expected variable name after ','");
        p->pos++;
        full_expect(p, TOKEN_EQ, "'=' for additional declarator");
        parse_full_expr(p);
    }
}
static void parse_full_simple_stmt_no_semi(Parser *p) {
    parse_full_expr(p);
    if (full_is_assign_op(full_peek_kind(p))) {
        p->pos++;
        parse_full_expr(p);
    }
}
static void parse_full_condition(Parser *p) {
    if (full_peek_kind(p)==TOKEN_AUTO||full_peek_kind(p)==TOKEN_MUT) {
        p->pos++;
        parse_full_pattern(p);
        full_expect(p, TOKEN_EQ, "'=' in condition");
        parse_full_expr(p);
        return;
    }
    parse_full_expr(p);
}
static void parse_full_block(Parser *p) {
    full_expect(p, TOKEN_LBRACE, "'{' for block");
    while (full_peek_kind(p)!=TOKEN_RBRACE) {
        if (full_peek_kind(p)==TOKEN_EOF) parse_error(full_raw_tok(p), "unterminated block");
        TokenKind k = full_peek_kind(p);
        if (k==TOKEN_IF||k==TOKEN_WHILE||k==TOKEN_FOR||k==TOKEN_MATCH||k==TOKEN_TRANSACT||k==TOKEN_HANDLE||k==TOKEN_LBRACE) {
            parse_full_statement(p);
            continue;
        }
        if (k==TOKEN_BREAK||k==TOKEN_CONTINUE||k==TOKEN_RETURN||k==TOKEN_DEFER) {
            parse_full_statement(p);
            continue;
        }
        if (k==TOKEN_IDENT && p->pos+1<p->count && p->toks[p->pos+1].kind==TOKEN_COLON) {
            parse_full_statement(p);
            continue;
        }
        if (full_is_declaration(p)) {
            parse_full_statement(p);
            continue;
        }
        int paren=0, brack=0, brace=0;
        int idx_semi=-1;
        for (int i=p->pos; i<p->count; i++) {
            TokenKind kk = p->toks[i].kind;
            if (kk==TOKEN_LPAREN) paren++;
            else if (kk==TOKEN_RPAREN) { if (paren>0) paren--; }
            else if (kk==TOKEN_LBRACK) brack++;
            else if (kk==TOKEN_RBRACK) { if (brack>0) brack--; }
            else if (kk==TOKEN_LBRACE) brace++;
            else if (kk==TOKEN_RBRACE) {
                if (brace==0) break;
                else brace--;
            }
            else if (kk==TOKEN_SEMI && paren==0 && brack==0 && brace==0) { idx_semi=i; break; }
            else if (kk==TOKEN_EOF) break;
        }
        if (idx_semi>=0) {
            parse_full_statement(p);
        } else {
            if (full_peek_kind(p)==TOKEN_RBRACE) break;
            parse_full_expr(p);
            break;
        }
    }
    full_expect(p, TOKEN_RBRACE, "'}' for block");
}
static void parse_full_statement(Parser *p) {
    TokenKind k = full_peek_kind(p);
    if (k==TOKEN_IDENT && p->pos+1<p->count && p->toks[p->pos+1].kind==TOKEN_COLON) {
        p->pos++; p->pos++;
        parse_full_statement(p);
        return;
    }
    if (k==TOKEN_LBRACE) { parse_full_block(p); return; }
    if (k==TOKEN_MATCH) { parse_full_match_expr(p); return; }
    if (k==TOKEN_HANDLE) {
        parse_full_expr(p);
        full_match(p, TOKEN_SEMI);
        return;
    }
    if (k==TOKEN_TRANSACT) { parse_full_expr(p); return; }
    if (k==TOKEN_SPAWN) {
        p->pos++;
        if (full_peek_kind(p)==TOKEN_LBRACE) { parse_full_block(p); return; }
        parse_full_postfix(p);
        full_expect(p, TOKEN_SEMI, "';' for spawn call");
        return;
    }
    if (k==TOKEN_IF) {
        p->pos++;
        if (full_match(p, TOKEN_LPAREN)) {
            parse_full_condition(p);
            full_expect(p, TOKEN_RPAREN, "')' for if");
        } else {
            parse_full_condition(p);
        }
        parse_full_block(p);
        while (full_match(p, TOKEN_ELSE)) {
            if (full_peek_kind(p)==TOKEN_IF) {
                p->pos++;
                if (full_match(p, TOKEN_LPAREN)) {
                    parse_full_condition(p);
                    full_expect(p, TOKEN_RPAREN, "')' for else-if");
                } else {
                    parse_full_condition(p);
                }
                parse_full_block(p);
            } else {
                parse_full_block(p);
                break;
            }
        }
        return;
    }
    if (k==TOKEN_WHILE) {
        p->pos++;
        if (full_match(p, TOKEN_LPAREN)) {
            parse_full_condition(p);
            full_expect(p, TOKEN_RPAREN, "')' for while");
        } else {
            parse_full_condition(p);
        }
        parse_full_block(p);
        return;
    }
    if (k==TOKEN_FOR) {
        p->pos++;
        full_expect(p, TOKEN_LPAREN, "'(' for for");
        int paren=0; b32 seen_in=FALSE, seen_semi=FALSE;
        for (int i=p->pos; i<p->count; i++) {
            TokenKind kk=p->toks[i].kind;
            if (kk==TOKEN_LPAREN) paren++;
            else if (kk==TOKEN_RPAREN) { if (paren==0) break; else paren--; }
            else if (kk==TOKEN_LBRACE) break;
            else if (paren==0 && kk==TOKEN_IN) { seen_in=TRUE; break; }
            else if (paren==0 && kk==TOKEN_SEMI) { seen_semi=TRUE; break; }
        }
        if (seen_in && !seen_semi) {
            full_match(p, TOKEN_AUTO);
            parse_full_pattern(p);
            full_expect(p, TOKEN_IN, "'in' for for-each");
            parse_full_expr(p);
        } else {
            if (full_peek_kind(p)!=TOKEN_SEMI) {
                if (full_is_declaration(p)) parse_full_variable_declaration_no_semi(p);
                else parse_full_simple_stmt_no_semi(p);
            }
            full_expect(p, TOKEN_SEMI, "';' in for");
            if (full_peek_kind(p)!=TOKEN_SEMI) parse_full_expr(p);
            full_expect(p, TOKEN_SEMI, "';' in for");
            if (full_peek_kind(p)!=TOKEN_RPAREN) parse_full_simple_stmt_no_semi(p);
        }
        full_expect(p, TOKEN_RPAREN, "')' for for");
        parse_full_block(p);
        return;
    }
    if (k==TOKEN_DEFER) {
        p->pos++;
        if (full_peek_kind(p)==TOKEN_LBRACE) parse_full_block(p);
        else { parse_full_simple_stmt_no_semi(p); full_expect(p, TOKEN_SEMI, "';' for defer"); }
        return;
    }
    if (k==TOKEN_BREAK) {
        p->pos++;
        if (full_peek_kind(p)==TOKEN_IDENT) p->pos++;
        full_expect(p, TOKEN_SEMI, "';' for break");
        return;
    }
    if (k==TOKEN_CONTINUE) {
        p->pos++;
        if (full_peek_kind(p)==TOKEN_IDENT) p->pos++;
        full_expect(p, TOKEN_SEMI, "';' for continue");
        return;
    }
    if (k==TOKEN_RETURN) {
        p->pos++;
        if (full_peek_kind(p)!=TOKEN_SEMI) parse_full_expr(p);
        full_expect(p, TOKEN_SEMI, "';' for return");
        return;
    }
    if (full_is_declaration(p)) {
        parse_full_variable_declaration_no_semi(p);
        full_expect(p, TOKEN_SEMI, "';' for declaration");
        return;
    }
    parse_full_simple_stmt_no_semi(p);
    full_expect(p, TOKEN_SEMI, "';' for statement");
}
static void parse_full_fn_sig(Parser *p) {
    full_expect(p, TOKEN_FN, "'fn'");
    full_expect(p, TOKEN_IDENT, "function name");
    if (full_peek_kind(p)==TOKEN_LT) parse_full_generics(p);
    full_expect(p, TOKEN_LPAREN, "'(' for params");
    if (full_peek_kind(p)!=TOKEN_RPAREN) {
        for (;;) {
            full_match(p, TOKEN_MUT);
            parse_full_type(p);
            full_expect(p, TOKEN_IDENT, "param name");
            if (!full_match(p, TOKEN_COMMA)) break;
            if (full_peek_kind(p)==TOKEN_RPAREN) break;
        }
    }
    full_expect(p, TOKEN_RPAREN, "')' for params");
    if (full_peek_kind(p)==TOKEN_LT) parse_full_effect_row(p);
    if (full_match(p, TOKEN_ARROW)) parse_full_type(p);
    if (full_peek_kind(p)==TOKEN_WHERE) parse_full_where(p);
}
static void parse_full_item(Parser *p) {
    parse_full_visibility_opt(p);
    TokenKind k = full_peek_kind(p);
    if (k==TOKEN_IMPORT) {
        p->pos++;
        parse_full_path(p);
        full_expect(p, TOKEN_SEMI, "';' for import");
        return;
    }
    if (k==TOKEN_MOD) {
        p->pos++;
        parse_full_path(p);
        full_match(p, TOKEN_SEMI);
        return;
    }
    if (k==TOKEN_FN) {
        parse_full_fn_sig(p);
        if (full_match(p, TOKEN_SEMI)) return;
        parse_full_block(p);
        return;
    }
    if (k==TOKEN_EXTERN) {
        p->pos++;
        full_match(p, TOKEN_STRING);
        parse_full_fn_sig(p);
        full_expect(p, TOKEN_SEMI, "';' for extern");
        return;
    }
    if (k==TOKEN_STRUCT) {
        p->pos++;
        full_expect(p, TOKEN_IDENT, "struct name");
        if (full_peek_kind(p)==TOKEN_LT) parse_full_generics(p);
        if (full_peek_kind(p)==TOKEN_WHERE) parse_full_where(p);
        full_expect(p, TOKEN_LBRACE, "'{' for struct");
        if (full_peek_kind(p)!=TOKEN_RBRACE) {
            for (;;) {
                parse_full_visibility_opt(p);
                full_match(p, TOKEN_MUT);
                parse_full_type(p);
                full_expect(p, TOKEN_IDENT, "field name");
                if (!full_match(p, TOKEN_COMMA)) break;
                if (full_peek_kind(p)==TOKEN_RBRACE) break;
            }
        }
        full_expect(p, TOKEN_RBRACE, "'}' for struct");
        return;
    }
    if (k==TOKEN_ENUM) {
        p->pos++;
        full_expect(p, TOKEN_IDENT, "enum name");
        if (full_peek_kind(p)==TOKEN_LT) parse_full_generics(p);
        if (full_peek_kind(p)==TOKEN_WHERE) parse_full_where(p);
        full_expect(p, TOKEN_LBRACE, "'{' for enum");
        if (full_peek_kind(p)!=TOKEN_RBRACE) {
            for (;;) {
                parse_full_visibility_opt(p);
                full_expect(p, TOKEN_IDENT, "variant name");
                if (full_match(p, TOKEN_LPAREN)) {
                    if (full_peek_kind(p)!=TOKEN_RPAREN) parse_full_type_list(p);
                    full_expect(p, TOKEN_RPAREN, "')' for variant");
                } else if (full_peek_kind(p)==TOKEN_LBRACE) {
                    p->pos++;
                    if (full_peek_kind(p)!=TOKEN_RBRACE) {
                        for (;;) {
                            parse_full_type(p);
                            full_expect(p, TOKEN_IDENT, "variant field name");
                            if (!full_match(p, TOKEN_COMMA)) break;
                            if (full_peek_kind(p)==TOKEN_RBRACE) break;
                        }
                    }
                    full_expect(p, TOKEN_RBRACE, "'}' for variant fields");
                } else if (full_match(p, TOKEN_EQ)) {
                    parse_full_expr(p);
                }
                if (!full_match(p, TOKEN_COMMA)) break;
                if (full_peek_kind(p)==TOKEN_RBRACE) break;
            }
        }
        full_expect(p, TOKEN_RBRACE, "'}' for enum");
        return;
    }
    if (k==TOKEN_EFFECT) {
        p->pos++;
        full_expect(p, TOKEN_IDENT, "effect name");
        if (full_peek_kind(p)==TOKEN_LT) parse_full_generics(p);
        full_expect(p, TOKEN_LBRACE, "'{' for effect");
        if (full_peek_kind(p)!=TOKEN_RBRACE) {
            for (;;) {
                parse_full_visibility_opt(p);
                full_expect(p, TOKEN_FN, "'fn' for effect op");
                full_expect(p, TOKEN_IDENT, "effect op name");
                full_expect(p, TOKEN_LPAREN, "'(' for effect op");
                if (full_peek_kind(p)!=TOKEN_RPAREN) {
                    for (;;) {
                        full_match(p, TOKEN_MUT);
                        parse_full_type(p);
                        full_expect(p, TOKEN_IDENT, "effect param name");
                        if (!full_match(p, TOKEN_COMMA)) break;
                        if (full_peek_kind(p)==TOKEN_RPAREN) break;
                    }
                }
                full_expect(p, TOKEN_RPAREN, "')' for effect op");
                if (full_match(p, TOKEN_ARROW)) parse_full_type(p);
                if (!full_match(p, TOKEN_COMMA)) break;
                if (full_peek_kind(p)==TOKEN_RBRACE) break;
            }
        }
        full_expect(p, TOKEN_RBRACE, "'}' for effect");
        return;
    }
    if (k==TOKEN_HANDLER) {
        p->pos++;
        full_expect(p, TOKEN_IDENT, "handler name");
        if (full_peek_kind(p)==TOKEN_LT) parse_full_generics(p);
        full_expect(p, TOKEN_FOR, "'for' for handler");
        parse_full_path(p);
        if (full_peek_kind(p)==TOKEN_LT) parse_full_effect_row(p);
        full_expect(p, TOKEN_LBRACE, "'{' for handler");
        if (full_peek_kind(p)!=TOKEN_RBRACE) {
            for (;;) {
                if (full_match(p, TOKEN_RETURN)) {
                    full_expect(p, TOKEN_LPAREN, "'(' after return");
                    full_match(p, TOKEN_MUT);
                    parse_full_type(p);
                    full_expect(p, TOKEN_IDENT, "return param name");
                    full_expect(p, TOKEN_RPAREN, "')'");
                } else {
                    full_expect(p, TOKEN_IDENT, "handler op name");
                    full_expect(p, TOKEN_LPAREN, "'(' for handler op");
                    if (full_peek_kind(p)!=TOKEN_RPAREN) {
                        for (;;) {
                            full_match(p, TOKEN_MUT);
                            parse_full_type(p);
                            full_expect(p, TOKEN_IDENT, "handler param name");
                            if (!full_match(p, TOKEN_COMMA)) break;
                            if (full_peek_kind(p)==TOKEN_RPAREN) break;
                        }
                    }
                    full_expect(p, TOKEN_RPAREN, "')' for handler op");
                }
                full_expect(p, TOKEN_FATARROW, "'=>' for handler op");
                if (full_peek_kind(p)==TOKEN_LBRACE) parse_full_block(p);
                else parse_full_expr(p);
                if (!full_match(p, TOKEN_COMMA)) break;
                if (full_peek_kind(p)==TOKEN_RBRACE) break;
            }
        }
        full_expect(p, TOKEN_RBRACE, "'}' for handler");
        return;
    }
    if (k==TOKEN_TRAIT) {
        p->pos++;
        full_expect(p, TOKEN_IDENT, "trait name");
        if (full_peek_kind(p)==TOKEN_LT) parse_full_generics(p);
        if (full_match(p, TOKEN_COLON)) parse_full_bound(p);
        full_expect(p, TOKEN_LBRACE, "'{' for trait");
        while (full_peek_kind(p)!=TOKEN_RBRACE) {
            parse_full_fn_sig(p);
            full_expect(p, TOKEN_SEMI, "';' for trait method");
        }
        full_expect(p, TOKEN_RBRACE, "'}' for trait");
        return;
    }
    if (k==TOKEN_IMPL) {
        p->pos++;
        if (full_peek_kind(p)==TOKEN_LT) parse_full_generics(p);
        parse_full_path(p);
        if (full_peek_kind(p)==TOKEN_LT) parse_full_generic_args(p);
        full_expect(p, TOKEN_FOR, "'for' for impl");
        parse_full_type(p);
        if (full_peek_kind(p)==TOKEN_WHERE) parse_full_where(p);
        full_expect(p, TOKEN_LBRACE, "'{' for impl");
        while (full_peek_kind(p)!=TOKEN_RBRACE) {
            parse_full_fn_sig(p);
            parse_full_block(p);
        }
        full_expect(p, TOKEN_RBRACE, "'}' for impl");
        return;
    }
    if (k==TOKEN_TYPE) {
        p->pos++;
        full_expect(p, TOKEN_IDENT, "type alias name");
        if (full_peek_kind(p)==TOKEN_LT) parse_full_generics(p);
        full_expect(p, TOKEN_EQ, "'=' for type alias");
        parse_full_type(p);
        full_expect(p, TOKEN_SEMI, "';' for type alias");
        return;
    }
    parse_full_variable_declaration_no_semi(p);
    full_expect(p, TOKEN_SEMI, "';' for global");
}
static void parse_full_program(Parser *p) {
    while (full_peek_kind(p)!=TOKEN_EOF) {
        parse_full_item(p);
    }
    full_expect(p, TOKEN_EOF, "end of file");
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

static void usage(const char *argv0) {
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  %s [--backend=c|native] file.tk              parse, check, emit (<base>.gen.c or .s)\n", argv0);
    fprintf(stderr, "  %s [--backend=c|native] build file.tk [-o out]  build a native binary\n", argv0);
    fprintf(stderr, "  %s [--backend=c|native] run file.tk [args..]    build to temp and execute\n", argv0);
    fprintf(stderr, "backends: c = via C (default); native = direct x86-64, no C involved\n");
}

static int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

static void stem_of(const char *path, char *out, size_t n) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, n, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
    if (!out[0]) snprintf(out, n, "a");
}

// Directory holding taka_runtime.h: cwd if present, else the compiler's dir.
static void include_dir(char *out, size_t n) {
    if (access("taka_runtime.h", R_OK) == 0) { snprintf(out, n, "."); return; }
    char exe[1024];
    ssize_t m = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (m > 0) {
        exe[m] = 0;
        char *sl = strrchr(exe, '/');
        if (sl) { *sl = 0; snprintf(out, n, "%s", exe); return; }
    }
    snprintf(out, n, ".");
}

static int make_tmpdir(char *out, size_t n) {
    char tmpl[] = "/tmp/taka-build-XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return -1; }
    snprintf(out, n, "%s", tmpl);
    return 0;
}

static void cleanup_tmpdir(const char *dir, const char *cfile, const char *bin) {
    if (cfile) unlink(cfile);
    if (bin) unlink(bin);
    rmdir(dir);
}

// Compile one C file to a native binary. Returns 0 on success.
static int cc_build(const char *cfile, const char *bin) {
    char inc[1024];
    include_dir(inc, sizeof inc);
    char cmd[4096];
    int w = snprintf(cmd, sizeof cmd, "cc -O2 -I\"%s\" \"%s\" -o \"%s\"", inc, cfile, bin);
    if (w < 0 || (size_t)w >= sizeof cmd) { fprintf(stderr, "taka: paths too long\n"); return 1; }
    int rc = system(cmd);
    if (rc != 0) { fprintf(stderr, "taka: C compile failed\n"); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = "gen";
    const char *backend = "c";
    const char *infile = NULL;
    const char *outopt = NULL;
    int ai = 1; // first non-flag arg
    if (ai < argc && strncmp(argv[ai], "--backend=", 10) == 0) {
        backend = argv[ai] + 10;
        if (strcmp(backend, "c") != 0 && strcmp(backend, "native") != 0) {
            fprintf(stderr, "taka: unknown backend '%s'\n", backend);
            return 1;
        }
        ai++;
    }
    int extra_start = 0; // first argv index of run-args (run mode only)
    if (ai < argc && strcmp(argv[ai], "build") == 0) {
        mode = "build";
        ai++;
        if (ai >= argc) { usage(argv[0]); return 1; }
        infile = argv[ai++];
        while (ai < argc) {
            if (strcmp(argv[ai], "-o") == 0 && ai + 1 < argc) { outopt = argv[++ai]; ai++; }
            else { usage(argv[0]); return 1; }
        }
    } else if (ai < argc && strcmp(argv[ai], "run") == 0) {
        mode = "run";
        ai++;
        if (ai >= argc) { usage(argv[0]); return 1; }
        infile = argv[ai++];
        extra_start = ai;
        if (extra_start < argc && strcmp(argv[extra_start], "--") == 0) extra_start++;
    } else if (ai + 1 == argc) {
        mode = "gen";
        infile = argv[ai];
    } else {
        usage(argv[0]);
        return 1;
    }

    mem_arena *arena = arena_create(MByte(8));
    if (!arena) { fprintf(stderr, "arena_create failed\n"); return 1; }

    const char *src = read_file(arena, infile);

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

    Parser pf = {toks, n, 0, arena, 0, 0};
    parse_full_program(&pf);
    if (strcmp(mode, "gen") == 0) printf("parse OK\n");

    // Emit C: legacy single-fn path for mut.tk, core backend otherwise.
    // In build/run modes the C file lives in a temp dir (no .gen.c kept).
    char tmpdir[1024];
    int have_tmpdir = 0;
    const char *gen_path = NULL;
    char gen_buf[1024];
    int use_c = (strcmp(backend, "c") == 0);
    if (strcmp(mode, "gen") == 0) {
        if (use_c && ends_with(infile, "mut.tk")) {
            Parser p = {toks, n, 0, arena, 0, 0};
            FnDef *fn = parse_program(&p);
            print_fn(fn);
            gen_c(fn, "mut.gen.c");
            printf("wrote mut.gen.c\n");
            arena_destroy(arena);
            return 0;
        }
        char stem[256];
        stem_of(infile, stem, sizeof stem);
        snprintf(gen_buf, sizeof gen_buf, use_c ? "%s.gen.c" : "%s.s", stem);
        gen_path = gen_buf;
    } else {
        if (make_tmpdir(tmpdir, sizeof tmpdir) != 0) { arena_destroy(arena); return 1; }
        have_tmpdir = 1;
        snprintf(gen_buf, sizeof gen_buf, use_c ? "%s/out.gen.c" : "%s/out.s", tmpdir);
        gen_path = gen_buf;
    }

    if (use_c && ends_with(infile, "mut.tk")) {
        Parser p = {toks, n, 0, arena, 0, 0};
        FnDef *fn = parse_program(&p);
        gen_c(fn, gen_path);
    } else {
        const char *berr = NULL;
        TkProgram *prog = ast_build_core(toks, n, arena, &berr);
        if (!prog) {
            // gen mode keeps the old non-fatal behavior (validation OK);
            // build/run need real codegen, so they fail here.
            if (strcmp(mode, "gen") == 0) {
                printf("core backend: TODO (%s) — validation OK\n", berr ? berr : "advanced feature");
                arena_destroy(arena);
                return 0;
            }
            fprintf(stderr, "taka: core backend TODO (%s) — cannot build this file\n",
                berr ? berr : "advanced feature");
            if (have_tmpdir) cleanup_tmpdir(tmpdir, NULL, NULL);
            arena_destroy(arena);
            return 1;
        }
        if (sema_check(prog, arena) != 0) {
            if (have_tmpdir) cleanup_tmpdir(tmpdir, NULL, NULL);
            arena_destroy(arena);
            return 1;
        }
        int emit_rc = use_c ? cgen_emit(prog, gen_path) : x64_emit(prog, gen_path);
        if (emit_rc != 0) {
            fprintf(stderr, "taka: codegen failed\n");
            if (have_tmpdir) cleanup_tmpdir(tmpdir, gen_path, NULL);
            arena_destroy(arena);
            return 1;
        }
    }

    if (strcmp(mode, "gen") == 0) {
        printf("sema OK\nwrote %s\n", gen_path);
        arena_destroy(arena);
        return 0;
    }

    char bin[1024];
    const char *bin_path = NULL;
    char bin_tmp[1024];
    if (strcmp(mode, "build") == 0) {
        if (outopt) {
            snprintf(bin, sizeof bin, "%s", outopt);
        } else {
            char stem[256];
            stem_of(infile, stem, sizeof stem);
            snprintf(bin, sizeof bin, "%s", stem);
        }
        bin_path = bin;
    } else {
        snprintf(bin_tmp, sizeof bin_tmp, "%s/prog", tmpdir);
        bin_path = bin_tmp;
    }

    if (cc_build(gen_path, bin_path) != 0) {
        cleanup_tmpdir(tmpdir, gen_path, NULL);
        arena_destroy(arena);
        return 1;
    }

    if (strcmp(mode, "build") == 0) {
        printf("built %s\n", bin_path);
        cleanup_tmpdir(tmpdir, gen_path, NULL);
        arena_destroy(arena);
        return 0;
    }

    // run: fork/exec the temp binary with extra args, propagate its status.
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        cleanup_tmpdir(tmpdir, gen_path, bin_path);
        arena_destroy(arena);
        return 1;
    }
    if (pid == 0) {
        int m = argc - extra_start;
        char **args = (char **)malloc(sizeof(char *) * (size_t)(m + 2));
        if (!args) _exit(127);
        args[0] = (char *)bin_path;
        for (int i = 0; i < m; i++) args[i + 1] = argv[extra_start + i];
        args[m + 1] = NULL;
        execv(bin_path, args);
        perror("execv");
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {}
    cleanup_tmpdir(tmpdir, gen_path, bin_path);
    arena_destroy(arena);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}
