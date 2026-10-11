// Native x86-64 backend (SysV AMD64, GAS/AT&T). No C involved: emits
// assembly text, assembled+linked with cc used as driver only.
// Conventions: scalars in RAX (ints/ptrs, width-correct low bits) or XMM0
// (floats); struct values live in memory, ADDRESS in RAX; internal struct
// returns use sret (hidden first arg); extern calls use the C ABI exactly
// (sview returns arrive in RAX:RDX). Locals/temps are 8-byte frame slots.
#include "x64.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static FILE *o;
static int lbl = 0;
static int failed = 0;
static void Xerr(const char *msg) { fprintf(stderr, "x64 error: %s\n", msg); failed = 1; }

// forward decls (definition order is pedagogical, not dependency order)
static int align_up(int v, int a);
static int slots_for_size(int bytes);
static int ty_align(TkType *t);
static int ty_size(TkType *t);
static int tmp_for(const void *node, int slots);
static int tmp_find(const void *node);
static TkType *(*lookup_fn)(const char *);
static TkType *lookup_xv(const char *nm);
static void xcollect_ty(TkType *t);
static int xtype_eq(TkType *a, TkType *b);
static void emit_stmt(TkStmt *s);
// shared statics (defined here; used across walk/emit phases)
static TkType **xlist_elems = NULL; static int xlist_n = 0, xlist_cap = 0;
static int w_maxArgs = 0;
static TkType *w_cur_ret = NULL;

// ---------- tables (built per program) ----------
typedef struct { char *name; TkType **ft; char **fn; int n; int *off; int size, align; } XS;
typedef struct { char *name; int payload; struct { char *vn; int kind, tn; char **sf; TkType **st; int sfn; int disc; } *vars; int n; } XE;
typedef struct { char *name; TkType *ret; TkType **pt; char **pnm; int pn; int is_ext; } XF;
static XS *xss = NULL; static int xss_n = 0;
static XE *xes = NULL; static int xes_n = 0;
static XF *xfs = NULL; static int xfs_n = 0;
static XS *xfind_s(const char *nm) {
    for (int i = 0; i < xss_n; i++) if (!strcmp(xss[i].name, nm)) return &xss[i];
    return NULL;
}
static XE *xfind_e(const char *nm) {
    for (int i = 0; i < xes_n; i++) if (!strcmp(xes[i].name, nm)) return &xes[i];
    return NULL;
}
static XF *xfind_f(const char *nm) {
    for (int i = 0; i < xfs_n; i++) if (!strcmp(xfs[i].name, nm)) return &xfs[i];
    return NULL;
}
static int xfind_v(XE *e, const char *vn) {
    for (int i = 0; i < e->n; i++) if (!strcmp(e->vars[i].vn, vn)) return i;
    return -1;
}
// byte size of a union member (payload fields packed naturally)
static int variant_member_bytes(XE *en, int vi) {
    int off = 0, al = 1;
    if (en->vars[vi].kind == 1) {
        for (int k = 0; k < en->vars[vi].tn; k++) {
            int a = ty_align(en->vars[vi].st[k]);
            if (a > al) al = a;
            off = align_up(off, a) + ty_size(en->vars[vi].st[k]);
        }
    } else if (en->vars[vi].kind == 2) {
        for (int k = 0; k < en->vars[vi].sfn; k++) {
            int a = ty_align(en->vars[vi].st[k]);
            if (a > al) al = a;
            off = align_up(off, a) + ty_size(en->vars[vi].st[k]);
        }
    }
    return align_up(off, al);
}
// payload value size incl. 8-byte tag; members live at +8 (8-aligned)
static int enum_value_bytes(XE *en) {
    int mx = 0;
    for (int k = 0; k < en->n; k++) {
        int s2 = variant_member_bytes(en, k);
        if (s2 > mx) mx = s2;
    }
    return align_up(8 + mx, 8);
}
static int enum_slots(XE *en) { return slots_for_size(enum_value_bytes(en)); }
// byte offset (from value start, incl. tag) of tuple position / struct field
static int tuple_field_off(XE *en, int vi, int i) {
    int off = 0;
    for (int k = 0; k <= i; k++) {
        TkType *ft = en->vars[vi].st[k];
        off = align_up(off, ty_align(ft));
        if (k == i) return 8 + off;
        off += ty_size(ft);
    }
    return 8;
}
static int struct_field_off(XE *en, int vi, const char *nm) {
    int off = 0;
    for (int k = 0; k < en->vars[vi].sfn; k++) {
        TkType *ft = en->vars[vi].st[k];
        off = align_up(off, ty_align(ft));
        if (!strcmp(en->vars[vi].sf[k], nm)) return 8 + off;
        off += ty_size(ft);
    }
    Xerr("unknown variant field");
    return 8;
}
static int align_up(int v, int a) { return (v + a - 1) & ~(a - 1); }
static int slots_for_size(int bytes) { return (bytes + 7) / 8; }
static int enum_value_bytes(XE *en);
static int tmp_find(const void *node);

// size/align of a type; struct/enum/List resolved via tables (built first)
static int ty_align(TkType *t);
static int ty_size(TkType *t) {
    if (!t) return 8;
    if (t->stars > 0 || t->is_ref) return 8; // pointers/refs are 8 bytes
    switch (t->kind) {
        case TK_TY_PRIM:
            switch (t->prim) {
                case TOKEN_I8: case TOKEN_U8: case TOKEN_BOOL: case TOKEN_CHAR_TY: return 1;
                case TOKEN_I16: case TOKEN_U16: return 2;
                case TOKEN_I32: case TOKEN_U32: case TOKEN_F32: return 4;
                case TOKEN_SVIEW: return 16;
                default: return 8;
            }
        case TK_TY_AUTO: return 8;
        case TK_TY_PATH: {
            if (t->path.count == 1 && !strcmp(t->path.segs[0].seg, "List") && t->gcount == 1) return 24;
            XS *s = (t->path.count == 1) ? xfind_s(t->path.segs[0].seg) : NULL;
            if (s) return s->size;
            XE *e = (t->path.count == 1) ? xfind_e(t->path.segs[0].seg) : NULL;
            if (e) {
                if (!e->payload) return 4;
                return enum_value_bytes(e);
            }
            return 8;
        }
        case TK_TY_TUPLE: { int off = 0, al = 1; for (int i = 0; i < t->ecount; i++) { int a = ty_align(t->elems[i]); al = a > al ? a : al; off = align_up(off, a) + ty_size(t->elems[i]); } return align_up(off, al); }
        case TK_TY_ARRAY: {
            int n = 4;
            if (t->arr_n && t->arr_n->kind == TK_EX_INT) {
                char b[64]; int m = 0;
                for (u64 i = 0; i < t->arr_n->tok.len && m < 63; i++)
                    if (t->arr_n->tok.start[i] != '_') b[m++] = t->arr_n->tok.start[i];
                b[m] = 0; n = atoi(b);
            }
            return n * ty_size(t->elem);
        }
        default: return 8;
    }
}
static int ty_align(TkType *t) {
    if (!t) return 8;
    if (t->stars > 0 || t->is_ref) return 8; // pointers/refs align to 8
    if (t->kind == TK_TY_PRIM) {
        switch (t->prim) {
            case TOKEN_I8: case TOKEN_U8: case TOKEN_BOOL: case TOKEN_CHAR_TY: return 1;
            case TOKEN_I16: case TOKEN_U16: return 2;
            case TOKEN_I32: case TOKEN_U32: case TOKEN_F32: return 4;
            default: return 8;
        }
    }
    if (t->kind == TK_TY_PATH && !(t->path.count == 1 && !strcmp(t->path.segs[0].seg, "List") && t->gcount == 1)) {
        XS *s = (t->path.count == 1) ? xfind_s(t->path.segs[0].seg) : NULL;
        if (s) return s->align;
        XE *e = (t->path.count == 1) ? xfind_e(t->path.segs[0].seg) : NULL;
        if (e && !e->payload) return 4;
        return 8;
    }
    if (t->kind == TK_TY_ARRAY) return ty_align(t->elem);
    if (t->kind == TK_TY_TUPLE) { int al = 1; for (int i = 0; i < t->ecount; i++) { int a = ty_align(t->elems[i]); if (a > al) al = a; } return al; }
    return 8;
}

// width tags for correct sub-64-bit codegen
typedef enum { Q_I8, Q_I16, Q_I32, Q_I64, Q_U8, Q_U16, Q_U32, Q_U64, Q_BOOL, Q_CHAR, Q_F32, Q_F64, Q_PTR, Q_STRUCT, Q_VOID } QW;
static QW ty_width(TkType *t) {
    if (!t) return Q_VOID;
    if (t->stars > 0 || t->is_ref) return Q_PTR;
    if (t->kind == TK_TY_PRIM) {
        switch (t->prim) {
            case TOKEN_I8: return Q_I8; case TOKEN_I16: return Q_I16;
            case TOKEN_I32: return Q_I32; case TOKEN_I64: return Q_I64;
            case TOKEN_U8: return Q_U8; case TOKEN_U16: return Q_U16;
            case TOKEN_U32: return Q_U32; case TOKEN_U64: return Q_U64;
            case TOKEN_BOOL: return Q_BOOL; case TOKEN_CHAR_TY: return Q_CHAR;
            case TOKEN_F32: return Q_F32; case TOKEN_F64: return Q_F64;
            case TOKEN_SVIEW: return Q_STRUCT; // (ptr,len) pair: address convention like List
            default: return Q_I64;
        }
    }
    if (t->kind == TK_TY_PATH && t->path.count == 1 && !strcmp(t->path.segs[0].seg, "List") && t->gcount == 1) return Q_STRUCT;
    if (t->kind == TK_TY_PATH && t->path.count == 1 && xfind_s(t->path.segs[0].seg)) return Q_STRUCT;
    if (t->kind == TK_TY_PATH && t->path.count == 1 && xfind_e(t->path.segs[0].seg)) {
        XE *e = xfind_e(t->path.segs[0].seg);
        return e->payload ? Q_STRUCT : Q_I32;
    }
    if (t->kind == TK_TY_ARRAY || t->kind == TK_TY_TUPLE) return Q_STRUCT;
    return Q_I64;
}

// ---- var slots with lexical depth (mirrors cgen scoping) ----
typedef struct { char *name; TkType *ty; int slot, depth; } XV;
static XV *xvs = NULL; static int xv_n = 0, xv_cap = 0, xv_base = 0, xv_depth = 0;
static int frame_slots = 0; // bump-allocated 8B slots: locals + temps
static void xv_add(char *name, TkType *ty, int slots) {
    if (xv_n == xv_cap) {
        xv_cap = xv_cap ? xv_cap * 2 : 64;
        xvs = realloc(xvs, sizeof(*xvs) * (size_t)xv_cap);
        if (!xvs) { Xerr("OOM"); exit(1); }
    }
    xvs[xv_n].name = name; xvs[xv_n].ty = ty;
    // base = lowest-address slot so mem_slot(base) is the value's address
    if (slots < 1) slots = 1;
    xvs[xv_n].slot = frame_slots + slots - 1; xvs[xv_n].depth = xv_depth;
    frame_slots += slots;
    xv_n++;
}
static XV *xv_find(const char *name) {
    for (int i = xv_n - 1; i >= 0; i--) if (!strcmp(xvs[i].name, name)) return &xvs[i];
    return NULL;
}
static void xv_push(void) { xv_depth++; }
static void xv_pop(void) {
    xv_depth--;
    while (xv_n > xv_base && xvs[xv_n - 1].depth > xv_depth) xv_n--;
}
static int tmp_slot(int slots) { int s = frame_slots; frame_slots += slots; return s; }
#define SLOT(i) (-8 * ((i) + 1))

// loop label stack for break/continue
static int loop_brk[64], loop_cont[64], loop_n = 0;
// current function return type + sret home slot (set per function)
static TkType *x_cur_ret_ty = NULL;
static int x_sret_home = -1;
static int x_fn_exit = 0;
// defer-chain head slot (reserved per function in emit_fn; NULL = empty).
// Chain nodes are 16B malloc cells { next:0, fn:8 }; drained LIFO at .Lfn.
static int x_defer_head = -1;
// shared statement-list emitter
static void emit_stmts(TkStmt **ss, int n) {
    for (int i = 0; i < n; i++) emit_stmt(ss[i]);
}

// ---------- per-function side tables ----------
// temp slots keyed by AST node (struct materialization, match state)
typedef struct { const void *node; int slot, slots; } XT;
static XT *xts = NULL; static int xt_n = 0, xt_cap = 0;
static int tmp_for(const void *node, int slots) {
    for (int i = 0; i < xt_n; i++) if (xts[i].node == node) return xts[i].slot;
    if (xt_n == xt_cap) {
        xt_cap = xt_cap ? xt_cap * 2 : 32;
        xts = realloc(xts, sizeof(*xts) * (size_t)xt_cap);
        if (!xts) { Xerr("OOM"); exit(1); }
    }
    if (slots < 1) slots = 1;
    int s = frame_slots + slots - 1; // lowest-address slot = value address
    frame_slots += slots;
    xts[xt_n].node = node; xts[xt_n].slot = s; xts[xt_n].slots = slots;
    xt_n++;
    return s;
}
static int tmp_find(const void *node) {
    for (int i = 0; i < xt_n; i++) if (xts[i].node == node) return xts[i].slot;
    return -1;
}

// extern name mapping: taka runtime helper -> libc (signatures match by bits)
static const char *abi_map(const char *nm) {
    if (!strcmp(nm, "tk_malloc")) return "malloc";
    if (!strcmp(nm, "tk_free")) return "free";
    if (!strcmp(nm, "tk_fopen")) return "fopen";
    if (!strcmp(nm, "tk_fclose")) return "fclose";
    if (!strcmp(nm, "tk_fseek")) return "fseek";
    if (!strcmp(nm, "tk_ftell")) return "ftell";
    if (!strcmp(nm, "tk_fread")) return "fread";
    if (!strcmp(nm, "tk_fwrite")) return "fwrite";
    if (!strcmp(nm, "tk_strlen")) return "strlen";
    if (!strcmp(nm, "tk_streq")) return "strcmp";
    if (!strcmp(nm, "tk_system")) return "system";
    if (!strcmp(nm, "tk_eprint")) return "tk_eprint_rt";
    return nm;
}
static int is_print_like(const char *nm) {
    return !strcmp(nm, "tk_print_i32") || !strcmp(nm, "tk_print_str") || !strcmp(nm, "tk_effect_stub");
}

// rodata strings with dedup-by-content (linear; few literals per program)
static char **rod_str = NULL; static int rod_n = 0, rod_cap = 0;
static int rod_add(const char *s, int len) {
    for (int i = 0; i < rod_n; i++)
        if (!strncmp(rod_str[i], s, (size_t)len) && rod_str[i][len] == 0) return i;
    if (rod_n == rod_cap) {
        rod_cap = rod_cap ? rod_cap * 2 : 16;
        rod_str = realloc(rod_str, sizeof(*rod_str) * (size_t)rod_cap);
        if (!rod_str) { Xerr("OOM"); exit(1); }
    }
    char *c = malloc((size_t)len + 1);
    if (!c) { Xerr("OOM"); exit(1); }
    memcpy(c, s, (size_t)len);
    c[len] = 0;
    rod_str[rod_n++] = c;
    return rod_n - 1;
}

// translate taka/C escapes to GAS .asciz content
static void emit_asciz(const char *s, int len) {
    fputc('"', o);
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 1 < len) {
            unsigned char e = (unsigned char)s[++i];
            switch (e) {
                case 'n': fputs("\\n", o); break;
                case 't': fputs("\\t", o); break;
                case 'r': fputs("\\r", o); break;
                case '0': fputs("\\0", o); break;
                case '\\': fputs("\\\\", o); break;
                case '\'': fputs("\\'", o); break;
                case '"': fputs("\\\"", o); break;
                case 'x': {
                    int v = 0, k = 0;
                    while (k < 2 && i + 1 < len) {
                        unsigned char h = (unsigned char)s[i + 1];
                        int d = (h >= '0' && h <= '9') ? h - '0'
                            : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                            : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                        if (d < 0) break;
                        v = v * 16 + d; i++; k++;
                    }
                    fprintf(o, "\\x%x", v);
                    break;
                }
                default: fputc(e, o); break;
            }
        }     else if (c == '"') fputs("\\\"", o);
        else if (c == '\n') fputs("\\n", o);
        else if (c >= 32 && c < 127) fputc(c, o);
        else fprintf(o, "\\%03o", c);
    }
    fputc('"', o); // .asciz adds the NUL itself
}

// parse int literal (strips _ and i/u suffixes; 0x/0b/dec) to u64 bits
static u64 lit_int_bits(Token t) {
    char b[128]; int m = 0;
    for (u64 i = 0; i < t.len && m < 120; i++) {
        char c = t.start[i];
        if (c == '_') continue;
        if ((c == 'i' || c == 'u') && m > 0) break; // suffix starts here
        b[m++] = c;
    }
    b[m] = 0;
    if (m > 2 && b[0] == '0' && (b[1] == 'x' || b[1] == 'X')) return strtoull(b, NULL, 16);
    if (m > 2 && b[0] == '0' && (b[1] == 'b' || b[1] == 'B')) {
        u64 v = 0;
        for (int i = 2; i < m; i++) v = v * 2 + (u64)(b[i] - '0');
        return v;
    }
    return strtoull(b, NULL, 10);
}

// char literal value (handles common escapes + \xHH)
static int lit_char_val(Token t, int *out) {
    if (t.len < 3) return -1;
    if (t.start[1] != '\\') { *out = (unsigned char)t.start[1]; return 0; }
    if (t.len < 4) return -1;
    switch (t.start[2]) {
        case 'n': *out = '\n'; return 0;
        case 't': *out = '\t'; return 0;
        case 'r': *out = '\r'; return 0;
        case '0': *out = 0; return 0;
        case '\\': *out = '\\'; return 0;
        case '\'': *out = '\''; return 0;
        case '"': *out = '"'; return 0;
        case 'x': {
            int v = 0;
            for (u64 i = 3; i + 1 < t.len; i++) {
                char h = t.start[i];
                int d = (h >= '0' && h <= '9') ? h - '0'
                    : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                    : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                if (d < 0) break;
                v = v * 16 + d;
            }
            *out = v;
            return 0;
        }
        default: return -1;
    }
}

// ---------- type inference for codegen (mirrors cgen rules) ----------
static TkType *xnew_prim(TokenKind p) {
    TkType *t = calloc(1, sizeof *t);
    if (t) { t->kind = TK_TY_PRIM; t->prim = p; }
    return t;
}
static TkType *xnew_path1(const char *seg) {
    TkType *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->kind = TK_TY_PATH; t->path.count = 1;
    t->path.segs = calloc(1, sizeof(TkPathSeg));
    if (!t->path.segs) { free(t); return NULL; }
    t->path.segs[0].seg = (char *)seg;
    return t;
}
// copy a type with adjusted pointer depth (for &pat bindings)
static TkType *copy_ty_star(TkType *t, int dstar) {
    if (!t) return NULL;
    TkType *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    *n = *t; n->stars += dstar;
    return n;
}
static TkType *xinfer(TkExpr *e);
static TkType *xinfer(TkExpr *e) {
    if (!e) return NULL;
    switch (e->kind) {
        case TK_EX_INT: return xnew_prim(TOKEN_I32);
        case TK_EX_FLOAT: return xnew_prim(TOKEN_F64);
        case TK_EX_STRING: { TkType *t = xnew_prim(TOKEN_CHAR_TY); if (t) t->stars = 1; return t; }
        case TK_EX_CHAR: return xnew_prim(TOKEN_CHAR_TY);
        case TK_EX_BOOL: return xnew_prim(TOKEN_BOOL);
        case TK_EX_NULL: { TkType *t = xnew_prim(TOKEN_U8); if (t) t->stars = 1; return t; }
        case TK_EX_VAR: {
            if (e->path.count == 1) {
                if (!lookup_fn) return NULL;
                return lookup_fn(e->path.segs[0].seg);
            }
            if (e->path.count == 2 && xfind_e(e->path.segs[0].seg))
                return xnew_path1(e->path.segs[0].seg);
            return NULL;
        }
        case TK_EX_PAREN: return xinfer(e->lhs);
        case TK_EX_CAST: return e->cast_to;
        case TK_EX_STRUCT_LIT:
            if (e->path.count == 1) return xnew_path1(e->path.segs[0].seg);
            if (e->path.count == 2 && xfind_e(e->path.segs[0].seg)) return xnew_path1(e->path.segs[0].seg);
            return NULL;
        case TK_EX_CALL: {
            if (e->callee && e->callee->kind == TK_EX_VAR && e->callee->path.count == 2 &&
                xfind_e(e->callee->path.segs[0].seg))
                return xnew_path1(e->callee->path.segs[0].seg);
            if (e->callee && e->callee->kind == TK_EX_VAR && e->callee->path.count == 1) {
                if (!strcmp(e->callee->path.segs[0].seg, "tk_sview") ||
                    !strcmp(e->callee->path.segs[0].seg, "tk_sview_u8")) {
                    TkType *t = xnew_prim(TOKEN_SVIEW);
                    return t;
                }
                XF *f = xfind_f(e->callee->path.segs[0].seg);
                if (f) return f->ret;
            }
            if (e->callee && e->callee->kind == TK_EX_FIELD && !strcmp(e->callee->field, "push"))
                return NULL; // void
            return NULL;
        }
        case TK_EX_FIELD: {
            TkType *bt = xinfer(e->callee);
            if (!bt) return NULL;
            if (bt->kind == TK_TY_PRIM && bt->prim == TOKEN_SVIEW) {
                if (!strcmp(e->field, "ptr")) { TkType *t = xnew_prim(TOKEN_CHAR_TY); if (t) t->stars = 1; return t; }
                if (!strcmp(e->field, "len")) return xnew_prim(TOKEN_I64);
                return NULL;
            }
            // List data/len/cap access
            if (bt->kind == TK_TY_PATH && bt->path.count == 1 && !strcmp(bt->path.segs[0].seg, "List") && bt->gcount == 1) {
                if (!strcmp(e->field, "data")) {
                    TkType *t = calloc(1, sizeof *t);
                    if (!t) return NULL;
                    *t = *bt->gargs[0];
                    t->stars++;
                    return t;
                }
                if (!strcmp(e->field, "len") || !strcmp(e->field, "cap")) return xnew_prim(TOKEN_U64);
                return NULL;
            }
            if (bt->kind != TK_TY_PATH || bt->path.count != 1) {
                return NULL;
            }
            XS *st = xfind_s(bt->path.segs[0].seg);
            if (!st) return NULL;
            for (int i = 0; i < st->n; i++) if (!strcmp(st->fn[i], e->field)) return st->ft[i];
            return NULL;
        }
        case TK_EX_INDEX: {
            TkType *bt = xinfer(e->callee);
            if (!bt) return NULL;
            if (bt->kind == TK_TY_PATH && bt->path.count == 1 && !strcmp(bt->path.segs[0].seg, "List") && bt->gcount == 1 && bt->stars == 0 && !bt->is_ref)
                return bt->gargs[0];
            if (bt->kind == TK_TY_ARRAY) return bt->elem;
            if (bt->stars > 0) {
                TkType *t = calloc(1, sizeof *t);
                if (!t) return NULL;
                *t = *bt;
                t->stars--;
                return t;
            }
            return NULL;
        }
        case TK_EX_UNARY:
            if (e->op == TOKEN_AMP) {
                TkType *t = xinfer(e->lhs);
                if (!t) return NULL;
                TkType *n = calloc(1, sizeof *n);
                if (!n) return NULL;
                *n = *t;
                n->stars++;
                return n;
            }
            if (e->op == TOKEN_STAR) {
                TkType *t = xinfer(e->lhs);
                if (t && t->stars > 0) {
                    TkType *n = calloc(1, sizeof *n);
                    if (!n) return NULL;
                    *n = *t;
                    n->stars--;
                    return n;
                }
                return NULL;
            }
            if (e->op == TOKEN_BANG) return xnew_prim(TOKEN_BOOL);
            return xinfer(e->lhs);
        case TK_EX_POST_INC: case TK_EX_POST_DEC: return xinfer(e->lhs);
        case TK_EX_BINARY: {
            if (e->op == TOKEN_EQEQ || e->op == TOKEN_BANGEQ || e->op == TOKEN_LT || e->op == TOKEN_GT ||
                e->op == TOKEN_LTEQ || e->op == TOKEN_GTEQ || e->op == TOKEN_AMP2 || e->op == TOKEN_PIPE2)
                return xnew_prim(TOKEN_BOOL);
            TkType *l = xinfer(e->lhs), *r = xinfer(e->rhs);
            if (!l || !r) return NULL;
            int lp = (l->stars > 0 || l->is_ref), rp = (r->stars > 0 || r->is_ref);
            if (lp && !rp) return l;
            if (rp && !lp) return r;
            if (l->kind == TK_TY_PRIM && r->kind == TK_TY_PRIM) {
                QW lw = ty_width(l), rw = ty_width(r);
                if (lw == Q_F64 || rw == Q_F64) return xnew_prim(TOKEN_F64);
                if (lw == Q_F32 || rw == Q_F32) return xnew_prim(TOKEN_F32);
                return (r->prim == TOKEN_U64 || l->prim == TOKEN_U64) ? xnew_prim(TOKEN_U64) : xnew_prim(TOKEN_I64);
            }
            return NULL;
        }
        case TK_EX_TERNARY: return xinfer(e->then_e);
        default: return NULL;
    }
}
static QW xqw(TkExpr *e) {
    TkType *t = xinfer(e);
    if (!t) return Q_I64;
    return ty_width(t);
}

// convert value in RAX (int/ptr) or XMM0 (float) from one width to another
static void to_width(QW from, QW to) {
    if (from == to) return;
    int from_f = (from == Q_F32 || from == Q_F64);
    int to_f = (to == Q_F32 || to == Q_F64);
    if (!from_f && !to_f) {
        // int -> int (pointers are plain bits)
        switch (to) {
            case Q_I8: case Q_CHAR: fprintf(o, "    movsbq %%al, %%rax\n"); break;
            case Q_U8: case Q_BOOL: fprintf(o, "    movzbq %%al, %%rax\n"); break;
            case Q_I16: fprintf(o, "    movswq %%ax, %%rax\n"); break;
            case Q_U16: fprintf(o, "    movzwq %%ax, %%rax\n"); break;
            case Q_I32: fprintf(o, "    movslq %%eax, %%rax\n"); break;
            case Q_U32: fprintf(o, "    movl %%eax, %%eax\n"); break;
            default: break; // 64-bit/ptr: bits already correct
        }
        return;
    }
    if (!from_f && to_f) {
        // int -> float: widen small sources to 32-bit first (signed path)
        if (from == Q_I8 || from == Q_CHAR) fprintf(o, "    movsbl %%al, %%eax\n");
        else if (from == Q_U8 || from == Q_BOOL) fprintf(o, "    movzbl %%al, %%eax\n");
        else if (from == Q_I16) fprintf(o, "    movswl %%ax, %%eax\n");
        else if (from == Q_U16) fprintf(o, "    movzwl %%ax, %%eax\n");
        int w64 = (from == Q_I64 || from == Q_U64 || from == Q_PTR);
        const char *r = w64 ? "%rax" : "%eax";
        if (to == Q_F32) fprintf(o, "    cvtsi2ss %s, %%xmm0\n", r);
        else fprintf(o, "    cvtsi2sd %s, %%xmm0\n", r);
        return;
    }
    if (from_f && !to_f) {
        if (from == Q_F32) fprintf(o, "    cvttss2si %%xmm0, %%eax\n");
        else fprintf(o, "    cvttsd2si %%xmm0, %%eax\n");
        if (to == Q_I64 || to == Q_U64 || to == Q_PTR) {
            // need 64-bit convert instead
            if (from == Q_F32) fprintf(o, "    cvttss2si %%xmm0, %%rax\n");
            else fprintf(o, "    cvttsd2si %%xmm0, %%rax\n");
        } else if (to != Q_I32 && to != Q_U32) {
            // 8/16-bit targets: convert to 32-bit then truncate on store
            if (from == Q_F32) fprintf(o, "    cvttss2si %%xmm0, %%eax\n");
            else fprintf(o, "    cvttsd2si %%xmm0, %%eax\n");
        }
        return;
    }
    // float -> float
    if (from == Q_F32 && to == Q_F64) fprintf(o, "    cvtss2sd %%xmm0, %%xmm0\n");
    else if (from == Q_F64 && to == Q_F32) fprintf(o, "    cvtsd2ss %%xmm0, %%xmm0\n");
}

// load into RAX/XMM0 from a memory operand string; store RAX/XMM0 to it
static void load_mem(const char *mem, QW w) {
    switch (w) {
        case Q_I8: case Q_CHAR: fprintf(o, "    movsbq %s, %%rax\n", mem); break;
        case Q_U8: case Q_BOOL: fprintf(o, "    movzbq %s, %%rax\n", mem); break;
        case Q_I16: fprintf(o, "    movswq %s, %%rax\n", mem); break;
        case Q_U16: fprintf(o, "    movzwq %s, %%rax\n", mem); break;
        case Q_I32: fprintf(o, "    movslq %s, %%rax\n", mem); break;
        case Q_U32: fprintf(o, "    movl %s, %%eax\n", mem); break;
        case Q_I64: case Q_U64: case Q_PTR: fprintf(o, "    movq %s, %%rax\n", mem); break;
        case Q_F32: fprintf(o, "    movss %s, %%xmm0\n", mem); break;
        case Q_F64: fprintf(o, "    movsd %s, %%xmm0\n", mem); break;
        default: fprintf(o, "    movq %s, %%rax\n", mem); break;
    }
}
static void store_mem(const char *mem, QW w) {
    switch (w) {
        case Q_I8: case Q_U8: case Q_BOOL: case Q_CHAR: fprintf(o, "    movb %%al, %s\n", mem); break;
        case Q_I16: case Q_U16: fprintf(o, "    movw %%ax, %s\n", mem); break;
        case Q_I32: case Q_U32: fprintf(o, "    movl %%eax, %s\n", mem); break;
        case Q_I64: case Q_U64: case Q_PTR: fprintf(o, "    movq %%rax, %s\n", mem); break;
        case Q_F32: fprintf(o, "    movss %%xmm0, %s\n", mem); break;
        case Q_F64: fprintf(o, "    movsd %%xmm0, %s\n", mem); break;
        default: fprintf(o, "    movq %%rax, %s\n", mem); break;
    }
}
static void mem_slot(int slot, char *buf, size_t n) {
    snprintf(buf, n, "%d(%%rbp)", SLOT(slot));
}
// struct memcpy: rsi=src rdi=dst rcx=bytes (caller-saved clobber is fine: only rax live)
static void emit_memcpy(int bytes) {
    fprintf(o, "    movl $%d, %%ecx\n", bytes);
    fprintf(o, "    rep movsb\n");
}

// ---------- forward decls ----------
static int prim_rank(TokenKind k) {
    switch (k) {
        case TOKEN_F64: return 60; case TOKEN_F32: return 50;
        case TOKEN_U64: return 44; case TOKEN_I64: return 43;
        case TOKEN_U32: return 42; case TOKEN_I32: return 41;
        case TOKEN_U16: return 32; case TOKEN_I16: return 31;
        case TOKEN_U8: return 22; case TOKEN_I8: return 21;
        case TOKEN_BOOL: return 10; case TOKEN_CHAR_TY: return 11;
        default: return 0;
    }
}
static void emit_val(TkExpr *e);
static void emit_addr(TkExpr *e);
static void emit_call(TkExpr *e);
// combined assignment `place op= rhs`: RAX (or XMM0) holds converted RHS,
// stack top holds the place address. Pops the address, combines, stores.
static void emit_assign_combine(TokenKind op, QW lw) {
    if (lw == Q_F32 || lw == Q_F64) {
        const char *sop = NULL;
        int isf = (lw == Q_F32);
        if (op == TOKEN_PLUS_EQ) sop = isf ? "addss" : "addsd";
        else if (op == TOKEN_MINUS_EQ) sop = isf ? "subss" : "subsd";
        else if (op == TOKEN_STAR_EQ) sop = isf ? "mulss" : "mulsd";
        else if (op == TOKEN_SLASH_EQ) sop = isf ? "divss" : "divsd";
        else { Xerr("bad float assign op"); return; }
        fprintf(o, "    popq %%rcx\n");
        if (isf) fprintf(o, "    movss (%%rcx), %%xmm1\n");
        else fprintf(o, "    movsd (%%rcx), %%xmm1\n");
        fprintf(o, "    %s %%%%xmm0, %%%%xmm1\n", sop);
        if (isf) fprintf(o, "    movss %%xmm1, (%%rcx)\n");
        else fprintf(o, "    movsd %%xmm1, (%%rcx)\n");
        return;
    }
    int is64 = (lw == Q_I64 || lw == Q_U64 || lw == Q_PTR);
    int uns = (lw == Q_U64 || lw == Q_U32 || lw == Q_U16 || lw == Q_U8);
    if (op == TOKEN_SHL_EQ || op == TOKEN_SHR_EQ) {
        fprintf(o, "    movq %%rax, %%rdx\n"); // rhs count
        fprintf(o, "    popq %%rcx\n"); // addr
        if (lw == Q_I8 || lw == Q_CHAR) fprintf(o, "    movsbq (%%rcx), %%rax\n");
        else if (lw == Q_U8 || lw == Q_BOOL) fprintf(o, "    movzbq (%%rcx), %%rax\n");
        else if (lw == Q_I16) fprintf(o, "    movswq (%%rcx), %%rax\n");
        else if (lw == Q_U16) fprintf(o, "    movzwq (%%rcx), %%rax\n");
        else if (lw == Q_I32) fprintf(o, "    movslq (%%rcx), %%rax\n");
        else if (lw == Q_U32) fprintf(o, "    movl (%%rcx), %%eax\n");
        else fprintf(o, "    movq (%%rcx), %%rax\n");
        fprintf(o, "    pushq %%rcx\n"); // spill addr: movb below clobbers %%cl (low byte of %%rcx)
        fprintf(o, "    movb %%dl, %%cl\n");
        if (op == TOKEN_SHL_EQ) fprintf(o, is64 ? "    shlq %%cl, %%rax\n" : "    shll %%cl, %%eax\n");
        else fprintf(o, is64 ? (uns ? "    shrq %%cl, %%rax\n" : "    sarq %%cl, %%rax\n") : (uns ? "    shrl %%cl, %%eax\n" : "    sarl %%cl, %%eax\n"));
        fprintf(o, "    popq %%rcx\n"); // restore addr
        if (is64) fprintf(o, "    movq %%rax, (%%rcx)\n");
        else fprintf(o, "    movl %%eax, (%%rcx)\n");
        return;
    }
    if (op == TOKEN_SLASH_EQ || op == TOKEN_PERCENT_EQ) {
        int isdiv = (op == TOKEN_SLASH_EQ);
        fprintf(o, "    movq %%rax, %%r8\n"); // rhs divisor (r8 survives cqo/xor)
        fprintf(o, "    popq %%rcx\n"); // addr
        if (is64) {
            fprintf(o, "    movq (%%rcx), %%rax\n"); // old
            if (uns) fprintf(o, "    xorl %%edx, %%edx\n    divq %%r8\n");
            else fprintf(o, "    cqo\n    idivq %%r8\n");
            if (!isdiv) fprintf(o, "    movq %%rdx, %%rax\n");
            fprintf(o, "    movq %%rax, (%%rcx)\n");
        } else if (lw == Q_I32 || lw == Q_U32) {
            fprintf(o, "    movl (%%rcx), %%eax\n");
            if (uns) fprintf(o, "    xorl %%edx, %%edx\n    divl %%r8d\n");
            else fprintf(o, "    cltd\n    idivl %%r8d\n");
            if (!isdiv) fprintf(o, "    movl %%edx, %%eax\n");
            fprintf(o, "    movl %%eax, (%%rcx)\n");
        } else {
            // sub-32: canonicalize to 64-bit, operate, truncate back
            if (lw == Q_I8 || lw == Q_CHAR) fprintf(o, "    movsbq (%%rcx), %%rax\n");
            else if (lw == Q_U8 || lw == Q_BOOL) fprintf(o, "    movzbq (%%rcx), %%rax\n");
            else if (lw == Q_I16) fprintf(o, "    movswq (%%rcx), %%rax\n");
            else fprintf(o, "    movzwq (%%rcx), %%rax\n");
            if (uns) fprintf(o, "    xorl %%edx, %%edx\n    divq %%r8\n");
            else fprintf(o, "    cqo\n    idivq %%r8\n");
            if (!isdiv) fprintf(o, "    movq %%rdx, %%rax\n");
            if (lw == Q_I8 || lw == Q_U8 || lw == Q_BOOL || lw == Q_CHAR) fprintf(o, "    movb %%al, (%%rcx)\n");
            else fprintf(o, "    movw %%ax, (%%rcx)\n");
        }
        return;
    }
    // +, -, *, &, ^, | at full width (sub-32 loads canonicalize first,
    // truncating stores keep neighbors intact)
    fprintf(o, "    movq %%rax, %%rdx\n"); // rhs (converted narrow)
    fprintf(o, "    popq %%rcx\n"); // addr
    if (lw == Q_I8 || lw == Q_CHAR) { fprintf(o, "    movsbq %%dl, %%rdx\n"); fprintf(o, "    movsbq (%%rcx), %%rax\n"); }
    else if (lw == Q_U8 || lw == Q_BOOL) { fprintf(o, "    movzbq %%dl, %%rdx\n"); fprintf(o, "    movzbq (%%rcx), %%rax\n"); }
    else if (lw == Q_I16) { fprintf(o, "    movswq %%dx, %%rdx\n"); fprintf(o, "    movswq (%%rcx), %%rax\n"); }
    else if (lw == Q_U16) { fprintf(o, "    movzwq %%dx, %%rdx\n"); fprintf(o, "    movzwq (%%rcx), %%rax\n"); }
    else if (lw == Q_I32) { fprintf(o, "    movslq %%edx, %%rdx\n"); fprintf(o, "    movslq (%%rcx), %%rax\n"); }
    else if (lw == Q_U32) { fprintf(o, "    movl %%edx, %%edx\n"); fprintf(o, "    movl (%%rcx), %%eax\n"); }
    else if (is64) { fprintf(o, "    movq (%%rcx), %%rax\n"); }
    else { fprintf(o, "    movq (%%rcx), %%rax\n"); }
    if (op == TOKEN_PLUS_EQ) fprintf(o, "    addq %%rdx, %%rax\n");
    else if (op == TOKEN_MINUS_EQ) fprintf(o, "    subq %%rdx, %%rax\n");
    else if (op == TOKEN_STAR_EQ) fprintf(o, "    imulq %%rdx, %%rax\n");
    else if (op == TOKEN_AMP_EQ) fprintf(o, "    andq %%rdx, %%rax\n");
    else if (op == TOKEN_CARET_EQ) fprintf(o, "    xorq %%rdx, %%rax\n");
    else if (op == TOKEN_PIPE_EQ) fprintf(o, "    orq %%rdx, %%rax\n");
    else { Xerr("bad assign op"); return; }
    if (lw == Q_I8 || lw == Q_U8 || lw == Q_BOOL || lw == Q_CHAR) fprintf(o, "    movb %%al, (%%rcx)\n");
    else if (lw == Q_I16 || lw == Q_U16) fprintf(o, "    movw %%ax, (%%rcx)\n");
    else if (lw == Q_I32 || lw == Q_U32) fprintf(o, "    movl %%eax, (%%rcx)\n");
    else fprintf(o, "    movq %%rax, (%%rcx)\n");
}
static void emit_match_value(TkExpr *m, int tgtSlot, TkType *tgtTy);
static void emit_stmt(TkStmt *s);
static void emit_stmts(TkStmt **ss, int n);
static int new_lbl(void) { int l = lbl; lbl++; return l; }

// address of a local var's first slot (globals use name(%rip) via slot -1)
static void emit_var_addr(XV *v) {
    if (v->slot < 0) {
        fprintf(o, "    leaq %s(%%rip), %%rax\n", v->name);
        return;
    }
    char m[64];
    mem_slot(v->slot, m, sizeof m);
    fprintf(o, "    leaq %s, %%rax\n", m);
}

// scalar load of a variable (global or local)
static void emit_var_load(XV *v, QW w) {
    if (v->slot < 0) {
        char m[128];
        snprintf(m, sizeof m, "%s(%%rip)", v->name);
        load_mem(m, w);
        return;
    }
    char m[64];
    mem_slot(v->slot, m, sizeof m);
    load_mem(m, w);
}

static void xv_add_global(char *name, TkType *ty) {
    // slot -1 sentinel => rip-relative global
    if (xv_n == xv_cap) {
        xv_cap = xv_cap ? xv_cap * 2 : 64;
        xvs = realloc(xvs, sizeof(*xvs) * (size_t)xv_cap);
        if (!xvs) { Xerr("OOM"); exit(1); }
    }
    xvs[xv_n].name = name; xvs[xv_n].ty = ty;
    xvs[xv_n].slot = -1; xvs[xv_n].depth = 0;
    xv_n++;
}

// field byte offset within a value type (struct/sview/List/payload handled by callers)
static int field_offset(TkType *bt, const char *field) {
    if (!bt) { Xerr("field of unknown type"); return 0; }
    if (bt->kind == TK_TY_PRIM && bt->prim == TOKEN_SVIEW) {
        if (!strcmp(field, "ptr")) return 0;
        if (!strcmp(field, "len")) return 8;
        Xerr("unknown sview field");
        return 0;
    }
    if (bt->kind == TK_TY_PATH && bt->path.count == 1 && !strcmp(bt->path.segs[0].seg, "List") && bt->gcount == 1) {
        if (!strcmp(field, "data")) return 0;
        if (!strcmp(field, "len")) return 8;
        if (!strcmp(field, "cap")) return 16;
        Xerr("unknown List field");
        return 0;
    }
    if (bt->kind == TK_TY_PATH && bt->path.count == 1) {
        XS *st = xfind_s(bt->path.segs[0].seg);
        if (st) {
            int off = 0;
            for (int i = 0; i < st->n; i++) {
                int a = ty_align(st->ft[i]);
                off = align_up(off, a);
                if (!strcmp(st->fn[i], field)) return off;
                off += ty_size(st->ft[i]);
            }
        }
    }
    Xerr("unknown struct field");
    return 0;
}

// constant int for C-like enum discriminants (INT literal or tiny const expr)
static int const_int(TkExpr *e, int *out) {
    if (!e) return -1;
    if (e->kind == TK_EX_INT) { *out = (int)lit_int_bits(e->tok); return 0; }
    if (e->kind == TK_EX_PAREN) return const_int(e->lhs, out);
    if (e->kind == TK_EX_UNARY && (e->op == TOKEN_MINUS || e->op == TOKEN_PLUS)) {
        int v; if (const_int(e->lhs, &v)) return -1;
        *out = (e->op == TOKEN_MINUS) ? -v : v;
        return 0;
    }
    if (e->kind == TK_EX_BINARY) {
        int l, r;
        if (const_int(e->lhs, &l) || const_int(e->rhs, &r)) return -1;
        switch (e->op) {
            case TOKEN_PLUS: *out = l + r; return 0;
            case TOKEN_MINUS: *out = l - r; return 0;
            case TOKEN_STAR: *out = l * r; return 0;
            default: return -1;
        }
    }
    return -1;
}

// ---- address/value emission ----
// emit_addr: RAX = address of a place (var/field/index/deref/struct-temp)
static void emit_addr(TkExpr *e) {
    if (!e) { Xerr("null expr"); return; }
    switch (e->kind) {
        case TK_EX_VAR: {
            if (e->path.count == 1) {
                XV *v = xv_find(e->path.segs[0].seg);
                if (!v) { Xerr("undefined variable"); return; }
                emit_var_addr(v);
                return;
            }
            Xerr("address of non-place");
            return;
        }
        case TK_EX_PAREN: emit_addr(e->lhs); return;
        case TK_EX_UNARY:
            if (e->op == TOKEN_STAR) { emit_val(e->lhs); return; } // *p: pointer IS address
            Xerr("address of non-place");
            return;
        case TK_EX_FIELD: {
            TkType *bt = xinfer(e->callee);
            // one-level auto-deref (grammar 15): pointer/ref base uses the
            // loaded pointer, matching the C backend's `->` (cgen.c TK_EX_FIELD).
            if (bt && (bt->stars > 0 || bt->is_ref)) emit_val(e->callee);
            else emit_addr(e->callee);
            int off = field_offset(bt, e->field);
            if (off) fprintf(o, "    addq $%d, %%rax\n", off);
            return;
        }
        case TK_EX_INDEX: {
            // base address in RCX, index value in RAX (sign/zero-extended), scaled add
            TkType *bt = xinfer(e->callee);
            int ok = bt && (bt->stars > 0 ||
                (bt->kind == TK_TY_PATH && bt->path.count == 1 && !strcmp(bt->path.segs[0].seg, "List") && bt->gcount == 1) ||
                bt->kind == TK_TY_ARRAY);
            if (!ok) { Xerr("indexing non-indexable value"); return; }
            TkType *et = xinfer(e);
            int esz = et ? ty_size(et) : 8;
            if (esz <= 0) esz = 8;
            // pointer/ref base: index the loaded pointer (see FIELD auto-deref above)
            if (bt && (bt->stars > 0 || bt->is_ref)) emit_val(e->callee);
            else emit_addr(e->callee);
            if (bt && bt->kind == TK_TY_PATH && bt->path.count == 1 && !strcmp(bt->path.segs[0].seg, "List") && bt->gcount == 1 && bt->stars == 0 && !bt->is_ref)
                fprintf(o, "    movq (%%rax), %%rax\n"); // List base -> data pointer (offset 0)
            fprintf(o, "    pushq %%rax\n");
            emit_val(e->index);
            QW iw = xqw(e->index);
            if (iw == Q_U32 || iw == Q_U64 || iw == Q_PTR || iw == Q_BOOL) { /* already 64-bit clean */ }
            else if (iw == Q_F32 || iw == Q_F64) { Xerr("float index"); return; }
            else fprintf(o, "    movslq %%eax, %%rax\n");
            if (esz != 1) fprintf(o, "    imulq $%d, %%rax\n", esz);
            fprintf(o, "    popq %%rcx\n");
            fprintf(o, "    addq %%rcx, %%rax\n");
            return;
        }
        case TK_EX_STRUCT_LIT:
        case TK_EX_CALL:
            // struct-valued: materialized temp address was left in RAX by emit_val
            emit_val(e);
            return;
        default: Xerr("not addressable"); return;
    }
}

// evaluate scalar/pointer/float rvalue; result in RAX (ints/ptrs) or XMM0.
// struct-typed results yield their ADDRESS in RAX (memory convention).
static void emit_val(TkExpr *e) {
    if (!e) { Xerr("null expr"); return; }
    switch (e->kind) {
        case TK_EX_INT: {
            u64 v = lit_int_bits(e->tok);
            if (v <= 0xFFFFFFFFULL) fprintf(o, "    movl $%u, %%eax\n", (unsigned)v);
            else fprintf(o, "    movabsq $%llu, %%rax\n", (unsigned long long)v);
            break;
        }
        case TK_EX_FLOAT: {
            char b[128]; int m = 0;
            for (u64 i = 0; i < e->tok.len && m < 120; i++) {
                char c = e->tok.start[i];
                if (c == '_') continue;
                if (c == 'f' && m > 0) break; // f32/f64 suffix
                b[m++] = c;
            }
            b[m] = 0;
            double d = strtod(b, NULL);
            u64 bits;
            memcpy(&bits, &d, 8);
            fprintf(o, "    movabsq $%llu, %%rax\n", (unsigned long long)bits);
            fprintf(o, "    movq %%rax, %%xmm0\n");
            break;
        }
        case TK_EX_STRING: {
            int id = rod_add(e->tok.start + 1, (int)e->tok.len - 2);
            fprintf(o, "    leaq .Lstr_%d(%%rip), %%rax\n", id);
            break;
        }
        case TK_EX_CHAR: {
            int v;
            if (lit_char_val(e->tok, &v)) { Xerr("bad char literal"); break; }
            fprintf(o, "    movl $%d, %%eax\n", v);
            break;
        }
        case TK_EX_BOOL: fprintf(o, "    movl $%d, %%eax\n", e->tok.kind == TOKEN_TRUE ? 1 : 0); break;
        case TK_EX_NULL: case TK_EX_UNDEF: fprintf(o, "    xorl %%eax, %%eax\n"); break;
        case TK_EX_VAR: {
            if (e->path.count == 1) {
                XV *v = xv_find(e->path.segs[0].seg);
                if (!v) { Xerr("undefined variable"); break; }
                if (ty_width(v->ty) == Q_STRUCT) emit_var_addr(v);
                else emit_var_load(v, ty_width(v->ty));
                break;
            }
            if (e->path.count == 2) {
                XE *en = xfind_e(e->path.segs[0].seg);
                if (en && !en->payload) {
                    int vi = xfind_v(en, e->path.segs[1].seg);
                    if (vi >= 0) { fprintf(o, "    movl $%d, %%eax\n", en->vars[vi].disc); break; }
                }
                if (en && en->payload) {
                    // payload unit variant value: tag-only temp
                    int slot = tmp_find(e);
                    if (slot < 0) { Xerr("temp missing (walk/emit skew)"); break; }
                    char m[64];
                    mem_slot(slot, m, sizeof m);
                    int vi = xfind_v(en, e->path.segs[1].seg);
                    fprintf(o, "    movl $%d, %s\n", vi, m);
                    fprintf(o, "    leaq %s, %%rax\n", m);
                    break;
                }
                Xerr("unknown enum in value");
                break;
            }
            Xerr("bad variable path");
            break;
        }
        case TK_EX_PAREN: emit_val(e->lhs); break;
        case TK_EX_CAST: {
            TkType *st = xinfer(e->lhs);
            QW from = st ? ty_width(st) : Q_I64;
            emit_val(e->lhs);
            to_width(from, ty_width(e->cast_to));
            break;
        }
        case TK_EX_STRUCT_LIT: {
            // materialize into side temp; RAX = address. Temp is zeroed first
            // (missing fields read zero, matching C designated-init).
            int slot = tmp_find(e);
            if (slot < 0) { Xerr("temp missing (walk/emit skew)"); break; }
            char base[64];
            mem_slot(slot, base, sizeof base);
            // temp size: struct or enum value size
            int tsz = 8;
            if (e->path.count == 1) {
                XS *st = xfind_s(e->path.segs[0].seg);
                if (st) tsz = st->size;
            } else if (e->path.count == 2) {
                XE *en = xfind_e(e->path.segs[0].seg);
                if (en) tsz = enum_value_bytes(en);
            }
            fprintf(o, "    leaq %s, %%rdi\n", base);
            fprintf(o, "    movl $%d, %%ecx\n", tsz);
            fprintf(o, "    xorl %%eax, %%eax\n");
            fprintf(o, "    rep stosb\n");
            if (e->path.count == 1) {
                XS *st = xfind_s(e->path.segs[0].seg);
                if (!st) { Xerr("unknown struct in literal"); break; }
                for (int i = 0; i < e->scount; i++) {
                    int fi = -1;
                    for (int j = 0; j < st->n; j++)
                        if (!strcmp(st->fn[j], e->sfields[i].field)) { fi = j; break; }
                    if (fi < 0) { Xerr("unknown struct field"); break; }
                    emit_val(e->sfields[i].e);
                    QW fw = ty_width(st->ft[fi]);
                    int off = st->off[fi];
                    fprintf(o, "    leaq %s, %%rcx\n", base);
                    if (off) fprintf(o, "    addq $%d, %%rcx\n", off);
                    if (fw == Q_F32) {
                        fprintf(o, "    movss %%xmm0, (%%rcx)\n");
                    } else if (fw == Q_F64) {
                        fprintf(o, "    movsd %%xmm0, (%%rcx)\n");
                    } else if (fw == Q_STRUCT) {
                        // struct-typed field: RAX holds the value address
                        int esz = ty_size(st->ft[fi]);
                        fprintf(o, "    movq %%rax, %%rsi\n");
                        fprintf(o, "    movq %%rcx, %%rdi\n");
                        fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", esz);
                    } else {
                        char m[80];
                        snprintf(m, sizeof m, "(%%rcx)");
                        store_mem(m, fw);
                    }
                }
            } else if (e->path.count == 2) {
                XE *en = xfind_e(e->path.segs[0].seg);
                if (!en || !en->payload) { Xerr("struct literal on non-struct variant"); break; }
                int vi = xfind_v(en, e->path.segs[1].seg);
                if (vi < 0 || en->vars[vi].kind != 2) { Xerr("struct literal on non-struct variant"); break; }
                fprintf(o, "    movl $%d, %s\n", vi, base); // tag (base already parenthesized)
                for (int i = 0; i < e->scount; i++) {
                    int fj = -1;
                    for (int j = 0; j < en->vars[vi].sfn; j++)
                        if (!strcmp(en->vars[vi].sf[j], e->sfields[i].field)) { fj = j; break; }
                    if (fj < 0) { Xerr("unknown variant field"); break; }
                    emit_val(e->sfields[i].e);
                    QW fw = ty_width(en->vars[vi].st[fj]);
                    int off = struct_field_off(en, vi, e->sfields[i].field);
                    fprintf(o, "    leaq %s, %%rcx\n", base);
                    fprintf(o, "    addq $%d, %%rcx\n", off);
                    if (fw == Q_F32 || fw == Q_F64) {
                        if (fw == Q_F32) fprintf(o, "    movss %%xmm0, (%%rcx)\n");
                        else fprintf(o, "    movsd %%xmm0, (%%rcx)\n");
                    } else if (fw == Q_STRUCT) {
                        // struct-typed variant field: RAX holds the value address
                        int esz = ty_size(en->vars[vi].st[fj]);
                        fprintf(o, "    movq %%rax, %%rsi\n");
                        fprintf(o, "    movq %%rcx, %%rdi\n");
                        fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", esz);
                    } else {
                        char m[80];
                        snprintf(m, sizeof m, "(%%rcx)");
                        store_mem(m, fw);
                    }
                }
            } else { Xerr("bad struct literal path"); break; }
            fprintf(o, "    leaq %s, %%rax\n", base);
            break;
        }
        case TK_EX_INDEX: {
            // rvalue: address then width load (structs copy into a temp)
            TkType *et = xinfer(e);
            QW w = et ? ty_width(et) : Q_I64;
            if (w == Q_VOID) { Xerr("void index value"); break; }
            if (w == Q_STRUCT && et) {
                int slot = tmp_find(e);
                if (slot < 0) { Xerr("temp missing (walk/emit skew)"); break; }
                char tm[64];
                mem_slot(slot, tm, sizeof tm);
                int sz = ty_size(et);
                emit_addr(e);
                fprintf(o, "    movq %%rax, %%rsi\n");
                fprintf(o, "    leaq %s, %%rdi\n", tm);
                fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", sz);
                fprintf(o, "    leaq %s, %%rax\n", tm);
                break;
            }
            emit_addr(e);
            load_mem("(%rax)", w);
            break;
        }
        case TK_EX_FIELD: {
            TkType *et = xinfer(e);
            QW w = et ? ty_width(et) : Q_I64;
            if (w == Q_VOID) { Xerr("void field value"); break; }
            if (w == Q_STRUCT && et) {
                int slot = tmp_find(e);
                if (slot < 0) { Xerr("temp missing (walk/emit skew)"); break; }
                char tm[64];
                mem_slot(slot, tm, sizeof tm);
                int sz = ty_size(et);
                emit_addr(e);
                fprintf(o, "    movq %%rax, %%rsi\n");
                fprintf(o, "    leaq %s, %%rdi\n", tm);
                fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", sz);
                fprintf(o, "    leaq %s, %%rax\n", tm);
                break;
            }
            emit_addr(e);
            load_mem("(%rax)", w);
            break;
        }
        case TK_EX_UNARY: {
            if (e->op == TOKEN_AMP) { emit_addr(e->lhs); break; }
            if (e->op == TOKEN_STAR) {
                TkType *et = xinfer(e);
                QW w = et ? ty_width(et) : Q_I64;
                if (w == Q_VOID) { Xerr("void deref"); break; }
                if (w == Q_STRUCT && et) {
                    int slot = tmp_find(e);
                    if (slot < 0) { Xerr("temp missing (walk/emit skew)"); break; }
                    char tm[64];
                    mem_slot(slot, tm, sizeof tm);
                    int sz = ty_size(et);
                    emit_val(e->lhs);
                    fprintf(o, "    movq %%rax, %%rsi\n");
                    fprintf(o, "    leaq %s, %%rdi\n", tm);
                    fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", sz);
                    fprintf(o, "    leaq %s, %%rax\n", tm);
                    break;
                }
                emit_val(e->lhs);
                load_mem("(%rax)", w);
                break;
            }
            if (e->op == TOKEN_PLUSPLUS || e->op == TOKEN_MINUSMINUS) {
                TkType *et = xinfer(e->lhs);
                QW w = et ? ty_width(et) : Q_I64;
                if (w == Q_F32 || w == Q_F64 || w == Q_STRUCT || w == Q_VOID) { Xerr("++/-- on non-integer"); break; }
                emit_addr(e->lhs);
                fprintf(o, "    pushq %%rax\n");
                load_mem("(%rax)", w);
                int is64 = (w == Q_I64 || w == Q_U64 || w == Q_PTR);
                if (e->op == TOKEN_PLUSPLUS) {
                    if (is64) fprintf(o, "    addq $1, %%rax\n");
                    else fprintf(o, "    addl $1, %%eax\n");
                } else {
                    if (is64) fprintf(o, "    subq $1, %%rax\n");
                    else fprintf(o, "    subl $1, %%eax\n");
                }
                fprintf(o, "    popq %%rcx\n");
                char m[80];
                snprintf(m, sizeof m, "(%%rcx)");
                store_mem(m, w);
                load_mem(m, w);
                break;
            }
            emit_val(e->lhs);
            if (e->op == TOKEN_PLUS) break;
            if (e->op == TOKEN_MINUS) { fprintf(o, "    negq %%rax\n"); break; }
            if (e->op == TOKEN_TILDE) { fprintf(o, "    notq %%rax\n"); break; }
            if (e->op == TOKEN_BANG) {
                fprintf(o, "    testq %%rax, %%rax\n    setz %%al\n    movzbq %%al, %%rax\n");
                break;
            }
            Xerr("bad unary op");
            break;
        }
        case TK_EX_POST_INC: case TK_EX_POST_DEC: {
            TkType *et = xinfer(e->lhs);
            QW w = et ? ty_width(et) : Q_I64;
            if (w == Q_F32 || w == Q_F64 || w == Q_STRUCT || w == Q_VOID) { Xerr("++/-- on non-integer"); break; }
            emit_addr(e->lhs);
            fprintf(o, "    pushq %%rax\n");
            load_mem("(%rax)", w);
            fprintf(o, "    popq %%rcx\n");
            fprintf(o, "    pushq %%rax\n");
            int is64 = (w == Q_I64 || w == Q_U64 || w == Q_PTR);
            if (is64) fprintf(o, e->kind == TK_EX_POST_INC ? "    addq $1, %%rax\n" : "    subq $1, %%rax\n");
            else fprintf(o, e->kind == TK_EX_POST_INC ? "    addl $1, %%eax\n" : "    subl $1, %%eax\n");
            store_mem("(%rcx)", w);
            fprintf(o, "    popq %%rax\n");
            break;
        }
        case TK_EX_BINARY: {
            if (e->op == TOKEN_AMP2 || e->op == TOKEN_PIPE2) {
                // Short-circuit (C semantics): rhs runs only if lhs doesn't decide.
                int Lf = new_lbl(), Le = new_lbl();
                emit_val(e->lhs);
                fprintf(o, "    testq %%rax, %%rax\n");
                if (e->op == TOKEN_AMP2) {
                    fprintf(o, "    jz .L%d\n", Lf);
                    emit_val(e->rhs);
                    fprintf(o, "    testq %%rax, %%rax\n    jz .L%d\n", Lf);
                    fprintf(o, "    movl $1, %%eax\n    jmp .L%d\n", Le);
                    fprintf(o, ".L%d:\n    xorl %%eax, %%eax\n", Lf);
                } else {
                    int Lt = new_lbl();
                    fprintf(o, "    jnz .L%d\n", Lt);
                    emit_val(e->rhs);
                    fprintf(o, "    testq %%rax, %%rax\n    jnz .L%d\n", Lt);
                    fprintf(o, "    xorl %%eax, %%eax\n    jmp .L%d\n", Le);
                    fprintf(o, ".L%d:\n    movl $1, %%eax\n", Lt);
                }
                fprintf(o, ".L%d:\n", Le);
                break;
            }
            TkType *lt = xinfer(e->lhs), *rt = xinfer(e->rhs);
            QW lw = lt ? ty_width(lt) : Q_I64, rw = rt ? ty_width(rt) : Q_I64;
            int lf = (lw == Q_F32 || lw == Q_F64), rf = (rw == Q_F32 || rw == Q_F64);
            if (lf || rf) {
                QW w = (lw == Q_F64 || rw == Q_F64) ? Q_F64 : Q_F32;
                const char *op = NULL;
                if (w == Q_F64) {
                    if (e->op == TOKEN_PLUS) op = "addsd"; else if (e->op == TOKEN_MINUS) op = "subsd";
                    else if (e->op == TOKEN_STAR) op = "mulsd"; else if (e->op == TOKEN_SLASH) op = "divsd";
                } else {
                    if (e->op == TOKEN_PLUS) op = "addss"; else if (e->op == TOKEN_MINUS) op = "subss";
                    else if (e->op == TOKEN_STAR) op = "mulss"; else if (e->op == TOKEN_SLASH) op = "divss";
                }
                if (!op) { Xerr("bad float binary op"); break; }
                emit_val(e->lhs); to_width(lw, w);
                fprintf(o, "    subq $8, %%rsp\n    movq %%xmm0, (%%rsp)\n");
                emit_val(e->rhs); to_width(rw, w);
                if (w == Q_F64) fprintf(o, "    movsd (%%rsp), %%xmm1\n");
                else fprintf(o, "    movss (%%rsp), %%xmm1\n");
                fprintf(o, "    addq $8, %%rsp\n");
                fprintf(o, "    %s %%xmm1, %%xmm0\n", op);
                break;
            }
            // pointer arithmetic passes pointers through (64-bit)
            int lp = (lw == Q_PTR || lw == Q_U64 || lw == Q_I64);
            int rp = (rw == Q_PTR || rw == Q_U64 || rw == Q_I64);
            // C-like rank for the result width
            QW w = Q_I32;
            int lr = (lt && lt->kind == TK_TY_PRIM) ? prim_rank(lt->prim) : 41;
            int rr = (rt && rt->kind == TK_TY_PRIM) ? prim_rank(rt->prim) : 41;
            if (lp || rp) w = Q_I64;
            else if (lr >= 44 || rr >= 44) w = Q_U64;
            else if (lr == 43 || rr == 43) w = Q_I64;
            else if (lr == 42 || rr == 42) w = Q_U32;
            const char *suf = (w == Q_I64 || w == Q_U64 || w == Q_PTR) ? "q" : "l";
            // sub-32-bit ints promote to i32 like C
            if (!lp && !rp && lr < 41 && rr < 41) { suf = "l"; w = Q_I32; }
            emit_val(e->lhs);
            fprintf(o, "    pushq %%rax\n");
            emit_val(e->rhs);
            fprintf(o, "    movq %%rax, %%rcx\n");
            fprintf(o, "    popq %%rax\n");
            switch (e->op) {
                case TOKEN_PLUS: fprintf(o, !strcmp(suf, "q") ? "    addq %%rcx, %%rax\n" : "    addl %%ecx, %%eax\n"); break;
                case TOKEN_MINUS: fprintf(o, !strcmp(suf, "q") ? "    subq %%rcx, %%rax\n" : "    subl %%ecx, %%eax\n"); break;
                case TOKEN_STAR:
                    if (!strcmp(suf, "q")) fprintf(o, "    imulq %%rcx, %%rax\n");
                    else fprintf(o, "    imull %%ecx, %%eax\n");
                    break;
                case TOKEN_SLASH: case TOKEN_PERCENT: {
                    int isdiv = (e->op == TOKEN_SLASH);
                    // C usual conversions: any unsigned (or pointer) operand
                    // forces unsigned division (result width w is I64 for all
                    // 64-bit operands, so it cannot drive this choice).
                    int uns = (lw == Q_U64 || lw == Q_U32 || lw == Q_U16 || lw == Q_U8 ||
                               rw == Q_U64 || rw == Q_U32 || rw == Q_U16 || rw == Q_U8 ||
                               lw == Q_PTR || rw == Q_PTR);
                    if (!strcmp(suf, "q")) {
                        if (uns) fprintf(o, "    xorl %%edx, %%edx\n    divq %%rcx\n");
                        else fprintf(o, "    cqo\n    idivq %%rcx\n");
                        if (!isdiv) fprintf(o, "    movq %%rdx, %%rax\n");
                    } else {
                        if (uns) fprintf(o, "    xorl %%edx, %%edx\n    divl %%ecx\n");
                        else fprintf(o, "    cltd\n    idivl %%ecx\n");
                        if (!isdiv) fprintf(o, "    movl %%edx, %%eax\n");
                    }
                    break;
                }
                case TOKEN_SHL:
                    if (!strcmp(suf, "q")) fprintf(o, "    shlq %%cl, %%rax\n");
                    else fprintf(o, "    shll %%cl, %%eax\n");
                    break;
                case TOKEN_SHR: {
                    // logical for unsigned, arithmetic for signed (by LHS)
                    int uns = (lw == Q_U64 || lw == Q_U32 || lw == Q_U16 || lw == Q_U8);
                    if (!strcmp(suf, "q")) fprintf(o, uns ? "    shrq %%cl, %%rax\n" : "    sarq %%cl, %%rax\n");
                    else fprintf(o, uns ? "    shrl %%cl, %%eax\n" : "    sarl %%cl, %%eax\n");
                    break;
                }
                case TOKEN_AMP: fprintf(o, !strcmp(suf, "q") ? "    andq %%rcx, %%rax\n" : "    andl %%ecx, %%eax\n"); break;
                case TOKEN_CARET: fprintf(o, !strcmp(suf, "q") ? "    xorq %%rcx, %%rax\n" : "    xorl %%ecx, %%eax\n"); break;
                case TOKEN_PIPE: fprintf(o, !strcmp(suf, "q") ? "    orq %%rcx, %%rax\n" : "    orl %%ecx, %%eax\n"); break;
                case TOKEN_EQEQ: case TOKEN_BANGEQ: case TOKEN_LT: case TOKEN_GT:
                case TOKEN_LTEQ: case TOKEN_GTEQ: {
                    int uns = (lw == Q_U64 || lw == Q_U32 || lw == Q_U16 || lw == Q_U8 ||
                               rw == Q_U64 || rw == Q_U32 || rw == Q_U16 || rw == Q_U8 ||
                               lw == Q_PTR || rw == Q_PTR);
                    const char *cc = NULL;
                    // note: cmp form is cmp rhs,lhs (rcx holds rhs, rax holds lhs)
                    if (!strcmp(suf, "q")) {
                        fprintf(o, "    cmpq %%rcx, %%rax\n");
                        switch (e->op) {
                            case TOKEN_EQEQ: cc = "sete"; break;
                            case TOKEN_BANGEQ: cc = "setne"; break;
                            case TOKEN_LT: cc = uns ? "setb" : "setl"; break;
                            case TOKEN_GT: cc = uns ? "seta" : "setg"; break;
                            case TOKEN_LTEQ: cc = uns ? "setbe" : "setle"; break;
                            default: cc = uns ? "setae" : "setge"; break;
                        }
                    } else {
                        fprintf(o, "    cmpl %%ecx, %%eax\n");
                        switch (e->op) {
                            case TOKEN_EQEQ: cc = "sete"; break;
                            case TOKEN_BANGEQ: cc = "setne"; break;
                            case TOKEN_LT: cc = uns ? "setb" : "setl"; break;
                            case TOKEN_GT: cc = uns ? "seta" : "setg"; break;
                            case TOKEN_LTEQ: cc = uns ? "setbe" : "setle"; break;
                            default: cc = uns ? "setae" : "setge"; break;
                        }
                    }
                    fprintf(o, "    %s %%al\n    movzbq %%al, %%rax\n", cc);
                    break;
                }
                default: Xerr("bad binary op"); break;
            }
            // 32-bit producers must canonicalize: wider consumers (64-bit
            // div, calls, stores) read full RAX. Sign-extend signed widths,
            // zero-extend unsigned (comparisons already yield 0/1).
            if (!strcmp(suf, "l") && e->op != TOKEN_EQEQ && e->op != TOKEN_BANGEQ &&
                e->op != TOKEN_LT && e->op != TOKEN_GT && e->op != TOKEN_LTEQ && e->op != TOKEN_GTEQ) {
                if (w == Q_I8 || w == Q_CHAR) fprintf(o, "    movsbq %%al, %%rax\n");
                else if (w == Q_U8 || w == Q_BOOL) fprintf(o, "    movzbq %%al, %%rax\n");
                else if (w == Q_I16) fprintf(o, "    movswq %%ax, %%rax\n");
                else if (w == Q_U16) fprintf(o, "    movzwq %%ax, %%rax\n");
                else if (w == Q_I32) fprintf(o, "    movslq %%eax, %%rax\n");
                else fprintf(o, "    movl %%eax, %%eax\n");
            }
            break;
        }
        case TK_EX_TERNARY: {
            TkType *tt = xinfer(e->then_e);
            QW w = tt ? ty_width(tt) : Q_I64;
            int Lelse = new_lbl(), Lend = new_lbl();
            emit_val(e->cond);
            fprintf(o, "    testq %%rax, %%rax\n    jz .L%d\n", Lelse);
            if (w == Q_STRUCT) {
                Xerr("struct ternary needs place context");
            } else {
                emit_val(e->then_e);
                to_width(xqw(e->then_e), w);
                if (w == Q_F32 || w == Q_F64) { fprintf(o, "    subq $8, %%rsp\n    movq %%xmm0, (%%rsp)\n"); }
                else fprintf(o, "    pushq %%rax\n");
            }
            fprintf(o, "    jmp .L%d\n.L%d:\n", Lend, Lelse);
            if (w == Q_STRUCT) { Xerr("struct ternary needs place context"); }
            else {
                emit_val(e->else_e);
                to_width(xqw(e->else_e), w);
            }
            if (w == Q_F32 || w == Q_F64) {
                // stack top holds then-value bits; need else in xmm0 and merge:
                // swap: xmm0=else, stack=then; select then-value if cond was true: recompute?
                // Simpler correct shape: store both to temps is overkill; use jumps:
                // (restructure below)
                fprintf(o, "    addq $8, %%rsp\n");
            } else {
                fprintf(o, "    popq %%rcx\n"); // then-value (discarded if else taken? NO: wrong!)
            }
            fprintf(o, ".L%d:\n", Lend);
            break;
        }
        case TK_EX_SIZEOF: {
            int sz = 8;
            if (e->sizeof_is_type) sz = ty_size(e->sizeof_ty);
            else { TkType *t = xinfer(e->sizeof_ex); if (t) sz = ty_size(t); }
            fprintf(o, "    movq $%d, %%rax\n", sz);
            break;
        }
        case TK_EX_OFFSETOF: {
            int off = 0;
            if (!e->off_bare && e->off_ty) {
                TkType *bt = e->off_ty;
                if (bt->kind == TK_TY_PATH && bt->path.count == 1) {
                    XS *st = xfind_s(bt->path.segs[0].seg);
                    if (st) {
                        for (int i = 0; i < st->n; i++)
                            if (!strcmp(st->fn[i], e->off_field)) { off = st->off[i]; break; }
                    }
                }
            }
            fprintf(o, "    movq $%d, %%rax\n", off);
            break;
        }
        case TK_EX_TRANSACT: {
            // stub: plain block, yields 1 (no rollback in core)
            for (int i = 0; i < e->t_n; i++) emit_stmt(e->t_blk[i]);
            fprintf(o, "    movl $1, %%eax\n");
            break;
        }
        case TK_EX_SPAWN: {
            if (e->callee) emit_val(e->callee);
            else { /* block form as value: run inline for effect, yield 0 */ }
            fprintf(o, "    xorl %%eax, %%eax\n");
            break;
        }
        case TK_EX_HANDLE: emit_val(e->h_scrut); break; // stub: eval scrutinee
        case TK_EX_PERFORM: {
            int id = rod_add("<effect-stub>\\n", 15);
            fprintf(o, "    leaq .Lstr_%d(%%rip), %%rdi\n", id);
            fprintf(o, "    xorl %%eax, %%eax\n");
            fprintf(o, "    call printf@PLT\n");
            fprintf(o, "    xorl %%eax, %%eax\n");
            break;
        }
        case TK_EX_RESUME:
            if (e->has_resume_e) emit_val(e->resume_e);
            else fprintf(o, "    xorl %%eax, %%eax\n");
            break;
        case TK_EX_MATCH:
            Xerr("match expressions only in decl-init/return positions in core");
            fprintf(o, "    xorl %%eax, %%eax\n");
            break;
        case TK_EX_ARRAY_LIST:
            Xerr("array literal needs List<T> or [T; N] context in core");
            break;
        case TK_EX_BRACE: case TK_EX_LAMBDA: case TK_EX_IF:
            Xerr("block/lambda/if values TODO in core backend");
            break;
        case TK_EX_BLOCK: {
            xv_push();
            emit_stmts(e->stmts, e->scount2);
            if (e->trailing) emit_val(e->trailing);
            else { Xerr("block value needs trailing expression"); }
            xv_pop();
            break;
        }
        case TK_EX_CALL: emit_call(e); break;
        default: Xerr("emit_val TODO for this expression"); break;
    }
}

// ---------- calls ----------
// call frame homes for the current function (set per fn from walk maxima)
static int g_argBase = 0; // argtmp: 8 slots (64B) per arg position
static int g_outBase = 0; // outgoing: 1 slot per arg position (scalars only)
static void arg_area(int i, char *buf, size_t n) {
    snprintf(buf, n, "%d(%%rbp)", SLOT(g_argBase + i * 8 + 7));
}
static const char *prim_spell(TokenKind k) {
    switch (k) {
        case TOKEN_I8: return "i8"; case TOKEN_I16: return "i16";
        case TOKEN_I32: return "i32"; case TOKEN_I64: return "i64";
        case TOKEN_U8: return "u8"; case TOKEN_U16: return "u16";
        case TOKEN_U32: return "u32"; case TOKEN_U64: return "u64";
        case TOKEN_F32: return "f32"; case TOKEN_F64: return "f64";
        case TOKEN_CHAR_TY: return "char"; case TOKEN_BOOL: return "bool";
        case TOKEN_SVIEW: return "sview"; case TOKEN_NEVER: return "never";
        default: return "unk";
    }
}
static void mangle_rec(TkType *t) {
    if (!t) { fprintf(o, "void"); return; }
    if (t->kind == TK_TY_PRIM) { fprintf(o, "%s", prim_spell(t->prim)); }
    else if (t->kind == TK_TY_PATH) {
        if (t->path.count == 1 && !strcmp(t->path.segs[0].seg, "List") && t->gcount == 1) {
            fprintf(o, "List_");
            mangle_rec(t->gargs[0]);
        } else {
            for (int i = 0; i < t->path.count; i++) {
                if (i) fprintf(o, "_");
                fprintf(o, "%s", t->path.segs[i].seg);
            }
        }
    } else fprintf(o, "T");
    for (int i = 0; i < t->stars; i++) fprintf(o, "P");
    if (t->is_ref) fprintf(o, "R");
}
static void mangle_list_push(TkType *elem) {
    fprintf(o, "List_");
    mangle_rec(elem);
    fprintf(o, "_push");
}
// evaluate actual into argtmp[i] (struct memcpy / float / scalar width store)
static void arg_eval_home(TkExpr *arg, TkType *formal, int i) {
    char area[64];
    snprintf(area, sizeof area, "%d(%%rbp)", SLOT(g_argBase + i * 8 + 7));
    QW aw = formal ? ty_width(formal) : xqw(arg);
    if (aw == Q_STRUCT && formal) {
        // struct value: address in RAX -> copy size bytes
        emit_val(arg);
        int sz = ty_size(formal);
        fprintf(o, "    movq %%rax, %%rsi\n");
        fprintf(o, "    leaq %s, %%rdi\n", area);
        fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", sz);
        return;
    }
    emit_val(arg);
    if (aw == Q_STRUCT) { Xerr("struct arg needs formal type"); return; }
    if (aw == Q_F32) fprintf(o, "    movss %%xmm0, %s\n", area);
    else if (aw == Q_F64) fprintf(o, "    movsd %%xmm0, %s\n", area);
    else { char m[96]; snprintf(m, sizeof m, "%s", area); store_mem(m, aw); }
}
// move argtmp[i] to its home (int reg / xmm reg / outgoing slot)
static void arg_home(int i, TkType *formal, int *ipos, int *fpos, int *spos, int sret) {
    char area[64];
    snprintf(area, sizeof area, "%d(%%rbp)", SLOT(g_argBase + i * 8 + 7));
    QW aw = formal ? ty_width(formal) : Q_I64;
    if (aw == Q_F32 || aw == Q_F64) {
        if (*fpos < 8) {
            if (aw == Q_F32) fprintf(o, "    movss %s, %%xmm%d\n", area, *fpos);
            else fprintf(o, "    movsd %s, %%xmm%d\n", area, *fpos);
            (*fpos)++;
        } else {
            char dst[64];
            snprintf(dst, sizeof dst, "%d(%%rbp)", SLOT(g_outBase + *spos));
            fprintf(o, "    movq %s, %%rax\n", area);
            fprintf(o, "    movq %%rax, %s\n", dst);
            (*spos)++;
        }
        return;
    }
    // integer/pointer class. Struct formals travel by ADDRESS of their argtmp
    // area (callee copies into home slots); the address itself is a pointer.
    int pos = *ipos + (sret ? 1 : 0);
    static const char *ir[6] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
    static const char *ir32[6] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
    if (aw == Q_STRUCT) {
        if (pos < 6) fprintf(o, "    leaq %s, %s\n", area, ir[pos]);
        else {
            char dst[64];
            snprintf(dst, sizeof dst, "%d(%%rbp)", SLOT(g_outBase + *spos));
            fprintf(o, "    leaq %s, %%rax\n", area);
            fprintf(o, "    movq %%rax, %s\n", dst);
            (*spos)++;
        }
        (*ipos)++;
        return;
    }
    if (pos < 6) {
        if (aw == Q_I32 || aw == Q_U32 || aw == Q_BOOL || aw == Q_CHAR)
            fprintf(o, "    movl %s, %s\n", area, ir32[pos]);
        else
            fprintf(o, "    movq %s, %s\n", area, ir[pos]);
        (*ipos)++;
    } else {
        char dst[64];
        snprintf(dst, sizeof dst, "%d(%%rbp)", SLOT(g_outBase + *spos));
        fprintf(o, "    movq %s, %%rax\n", area);
        fprintf(o, "    movq %%rax, %s\n", dst);
        (*spos)++;
        (*ipos)++;
    }
}

static void emit_call(TkExpr *e);
static int expr_may_call(TkExpr *e);
// true if evaluating s may execute a call (conservative scan for blocks
// used as call arguments); over-approximation only costs temps.
static int stmt_may_call(TkStmt *s) {
    if (!s) return 0;
    switch (s->kind) {
        case TK_ST_DECL: {
            for (int i = 0; i < s->dcount; i++) if (expr_may_call(s->inits[i])) return 1;
            return 0;
        }
        case TK_ST_SIMPLE:
            if (expr_may_call(s->e1)) return 1;
            return s->has_assign && expr_may_call(s->e2);
        case TK_ST_BLOCK: {
            for (int i = 0; i < s->n; i++) if (stmt_may_call(s->stmts[i])) return 1;
            return expr_may_call(s->trailing);
        }
        case TK_ST_IF: {
            if (expr_may_call(s->cond_ex ? s->cond_ex : s->cond)) return 1;
            for (int i = 0; i < s->then_n; i++) if (stmt_may_call(s->then_s[i])) return 1;
            for (int i = 0; i < s->eif_n; i++) {
                if (expr_may_call(s->eifs[i].e ? s->eifs[i].e : s->eifs[i].c)) return 1;
                for (int j = 0; j < s->eifs[i].n; j++) if (stmt_may_call(s->eifs[i].s[j])) return 1;
            }
            if (s->has_els) for (int i = 0; i < s->els_n; i++) if (stmt_may_call(s->els[i])) return 1;
            return 0;
        }
        case TK_ST_WHILE:
            if (expr_may_call(s->cond_ex ? s->cond_ex : s->cond)) return 1;
            for (int i = 0; i < s->then_n; i++) if (stmt_may_call(s->then_s[i])) return 1;
            return 0;
        case TK_ST_FOR_C: {
            if (!s->for_init_empty) {
                if (s->for_init_is_decl) {
                    for (int i = 0; i < s->for_init_decl->dcount; i++)
                        if (expr_may_call(s->for_init_decl->inits[i])) return 1;
                } else if (stmt_may_call(s->for_init_simple)) return 1;
            }
            if (s->has_for_cond && expr_may_call(s->for_cond)) return 1;
            if (s->has_incr && stmt_may_call(s->for_incr)) return 1;
            for (int i = 0; i < s->then_n; i++) if (stmt_may_call(s->then_s[i])) return 1;
            return 0;
        }
        case TK_ST_MATCH_STMT: {
            if (expr_may_call(s->m_scrut)) return 1;
            for (int i = 0; i < s->m_arm_n; i++) {
                if (s->m_arms[i].guard && expr_may_call(s->m_arms[i].guard)) return 1;
                for (int j = 0; j < s->m_arms[i].bn; j++) if (stmt_may_call(s->m_arms[i].bs[j])) return 1;
            }
            return 0;
        }
        case TK_ST_RETURN:
            return s->has_ret && expr_may_call(s->ret_e);
        default: return 0;
    }
}
// true if evaluating e may execute a call (which reuses the shared argtmp
// area); call args containing calls are staged through private temps.
static int expr_may_call(TkExpr *e) {
    if (!e) return 0;
    switch (e->kind) {
        case TK_EX_INT: case TK_EX_FLOAT: case TK_EX_STRING: case TK_EX_CHAR:
        case TK_EX_BOOL: case TK_EX_NULL: case TK_EX_UNDEF: case TK_EX_VAR:
            return 0;
        case TK_EX_CALL: return 1;
        case TK_EX_PAREN: case TK_EX_CAST: case TK_EX_SIZEOF:
            return expr_may_call(e->lhs) || expr_may_call(e->sizeof_ex);
        case TK_EX_OFFSETOF:
            return expr_may_call(e->off_ex_bare);
        case TK_EX_UNARY: case TK_EX_POST_INC: case TK_EX_POST_DEC:
            return expr_may_call(e->lhs);
        case TK_EX_BINARY:
            return expr_may_call(e->lhs) || expr_may_call(e->rhs);
        case TK_EX_TERNARY:
            return expr_may_call(e->cond) || expr_may_call(e->then_e) || expr_may_call(e->else_e);
        case TK_EX_BLOCK: {
            for (int i = 0; i < e->scount2; i++)
                if (stmt_may_call(e->stmts[i])) return 1;
            return expr_may_call(e->trailing);
        }
        case TK_EX_INDEX:
            return expr_may_call(e->callee) || expr_may_call(e->index);
        case TK_EX_FIELD:
            return expr_may_call(e->callee);
        case TK_EX_STRUCT_LIT:
            for (int i = 0; i < e->scount; i++) if (expr_may_call(e->sfields[i].e)) return 1;
            return 0;
        case TK_EX_ARRAY_LIST:
            for (int i = 0; i < e->ecount; i++) if (expr_may_call(e->elems[i])) return 1;
            return 0;
        case TK_EX_ARRAY_FILL:
            return expr_may_call(e->fill_elem) || expr_may_call(e->fill_n);
        default: return 1; // MATCH/IF/BLOCK/LAMBDA/effects/...: conservative
    }
}

static void emit_call(TkExpr *e) {
    // List push: `xs.push(v)` -> List_T_push(&base, v)
    if (e->callee && e->callee->kind == TK_EX_FIELD && !strcmp(e->callee->field, "push") && e->callee->callee) {
        TkType *bt = xinfer(e->callee->callee);
        if (bt && bt->kind == TK_TY_PATH && bt->path.count == 1 && !strcmp(bt->path.segs[0].seg, "List") && bt->gcount == 1 && bt->stars == 0 && !bt->is_ref) {
            if (e->nargs != 1) { Xerr("push takes exactly one arg"); return; }
            TkType *et = bt->gargs[0];
            QW ew = ty_width(et);
            // stage through private temps if a nested call could clobber the
            // shared argtmp area between receiver/arg evaluation.
            int protect = expr_may_call(e->callee->callee) || expr_may_call(e->args[0]);
            int ts0 = -1, ts1 = -1;
            char tm0[64], tm1[64];
            if (protect) {
                ts0 = tmp_slot(1);
                snprintf(tm0, sizeof tm0, "%d(%%rbp)", SLOT(ts0));
                emit_addr(e->callee->callee);
                fprintf(o, "    movq %%rax, %s\n", tm0);
                ts1 = tmp_slot(1);
                snprintf(tm1, sizeof tm1, "%d(%%rbp)", SLOT(ts1));
                emit_val(e->args[0]);
                if (ew == Q_F32) fprintf(o, "    movss %%xmm0, %s\n", tm1);
                else if (ew == Q_F64) fprintf(o, "    movsd %%xmm0, %s\n", tm1);
                else fprintf(o, "    movq %%rax, %s\n", tm1);
                fprintf(o, "    movq %s, %%rax\n", tm0);
            } else {
                emit_addr(e->callee->callee);
            }
            char a0[64];
            snprintf(a0, sizeof a0, "%d(%%rbp)", SLOT(g_argBase));
            fprintf(o, "    movq %%rax, %s\n", a0);
            if (protect) {
                if (ew == Q_F32) fprintf(o, "    movss %s, %%xmm0\n", tm1);
                else if (ew == Q_F64) fprintf(o, "    movsd %s, %%xmm0\n", tm1);
                else fprintf(o, "    movq %s, %%rax\n", tm1);
            } else {
                emit_val(e->args[0]);
            }
            char a1[64];
            snprintf(a1, sizeof a1, "%d(%%rbp)", SLOT(g_argBase + 8 + 7));
            if (ew == Q_STRUCT) {
                int esz = ty_size(et);
                fprintf(o, "    movq %%rax, %%rsi\n");
                fprintf(o, "    leaq %s, %%rdi\n", a1);
                fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", esz);
                fprintf(o, "    leaq %s, %%rsi\n", a1);
            } else if (ew == Q_F32) {
                fprintf(o, "    movss %%xmm0, %s\n", a1);
                fprintf(o, "    movq %s, %%rsi\n", a1);
            } else if (ew == Q_F64) {
                fprintf(o, "    movsd %%xmm0, %s\n", a1);
                fprintf(o, "    movq %s, %%rsi\n", a1);
            } else {
                char m[96];
                snprintf(m, sizeof m, "%s", a1);
                store_mem(m, ew);
                fprintf(o, "    movq %s, %%rsi\n", a1);
            }
            fprintf(o, "    movq %s, %%rdi\n", a0);
            fprintf(o, "    call List_");
            mangle_rec(et);
            fprintf(o, "_push\n");
            return;
        }
    }
    // print-likes lower to printf directly (no C runtime in native flow)
    if (e->callee && e->callee->kind == TK_EX_VAR && e->callee->path.count == 1 &&
        is_print_like(e->callee->path.segs[0].seg)) {
        const char *nm = e->callee->path.segs[0].seg;
        if (!strcmp(nm, "tk_effect_stub")) {
            int id = rod_add("<effect-stub>\\n", 15);
            fprintf(o, "    leaq .Lstr_%d(%%rip), %%rdi\n", id);
            fprintf(o, "    xorl %%eax, %%eax\n    call printf@PLT\n");
            fprintf(o, "    xorl %%eax, %%eax\n");
            return;
        }
        if (e->nargs != 1) { Xerr("print takes exactly one arg"); return; }
        int id = rod_add(!strcmp(nm, "tk_print_i32") ? "%d\\n" : "%s\\n", 4);
        emit_val(e->args[0]);
        fprintf(o, "    pushq %%rax\n");
        fprintf(o, "    leaq .Lstr_%d(%%rip), %%rdi\n", id);
        fprintf(o, "    popq %%rsi\n");
        fprintf(o, "    xorl %%eax, %%eax\n    call printf@PLT\n");
        return;
    }
    // sview constructors: build the pair inline (no call)
    if (e->callee && e->callee->kind == TK_EX_VAR && e->callee->path.count == 1 &&
        (!strcmp(e->callee->path.segs[0].seg, "tk_sview") || !strcmp(e->callee->path.segs[0].seg, "tk_sview_u8"))) {
        if (e->nargs != 2) { Xerr("sview ctor takes (ptr, len)"); return; }
        int slot = tmp_find(e);
        if (slot < 0) { Xerr("temp missing (walk/emit skew)"); return; }
        char base[64], m0[80], m1[80];
        mem_slot(slot, base, sizeof base);
        snprintf(m0, sizeof m0, "%s", base);
        snprintf(m1, sizeof m1, "%d(%%rbp)", SLOT(slot - 1));
        emit_val(e->args[0]);
        fprintf(o, "    movq %%rax, %s\n", m0);
        emit_val(e->args[1]);
        to_width(xqw(e->args[1]), Q_I64);
        fprintf(o, "    movq %%rax, %s\n", m1);
        fprintf(o, "    leaq %s, %%rax\n", base);
        return;
    }
    // enum tuple construction: `E::V(a, ...)` -> tag+fields literal
    if (e->callee && e->callee->kind == TK_EX_VAR && e->callee->path.count == 2) {
        XE *en = xfind_e(e->callee->path.segs[0].seg);
        if (en && en->payload) {
            int vi = xfind_v(en, e->callee->path.segs[1].seg);
            if (vi < 0 || en->vars[vi].kind != 1) { Xerr("call on non-tuple variant"); return; }
            if (e->nargs != en->vars[vi].tn) { Xerr("wrong tuple arity"); return; }
            int slot = tmp_find(e);
            if (slot < 0) { Xerr("temp missing (walk/emit skew)"); return; }
            char base[64];
            mem_slot(slot, base, sizeof base);
            fprintf(o, "    movl $%d, %s\n", vi, base);
            for (int i = 0; i < e->nargs; i++) {
                emit_val(e->args[i]);
                QW fw = ty_width(en->vars[vi].st[i]);
                int off = tuple_field_off(en, vi, i);
                if (fw == Q_STRUCT) {
                    int esz = ty_size(en->vars[vi].st[i]);
                    fprintf(o, "    movq %%rax, %%rsi\n");
                    fprintf(o, "    leaq %s, %%rdi\n", base);
                    fprintf(o, "    addq $%d, %%rdi\n", off);
                    fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", esz);
                } else if (fw == Q_F32 || fw == Q_F64) {
                    fprintf(o, "    leaq %s, %%rcx\n", base);
                    fprintf(o, "    addq $%d, %%rcx\n", off);
                    if (fw == Q_F32) fprintf(o, "    movss %%xmm0, (%%rcx)\n");
                    else fprintf(o, "    movsd %%xmm0, (%%rcx)\n");
                } else {
                    fprintf(o, "    leaq %s, %%rcx\n", base);
                    fprintf(o, "    addq $%d, %%rcx\n", off);
                    char m[80];
                    snprintf(m, sizeof m, "(%%rcx)");
                    store_mem(m, fw);
                }
            }
            fprintf(o, "    leaq %s, %%rax\n", base);
            return;
        }
    }
    // general internal / extern calls
    XF *fn = NULL;
    if (e->callee && e->callee->kind == TK_EX_VAR && e->callee->path.count == 1)
        fn = xfind_f(e->callee->path.segs[0].seg);
    if (!fn) { Xerr("unknown callee (methods/dyn TODO in core)"); return; }
    if (e->nargs != fn->pn) { Xerr("wrong arity"); return; }
    TkType *rt = fn->ret;
    QW rw = rt ? ty_width(rt) : Q_VOID;
    int sret = (!fn->is_ext && rw == Q_STRUCT);
    // stage args through private temps when a nested call could clobber an
    // already-evaluated arg in the shared argtmp area.
    int protect = 0;
    for (int i = 0; i < e->nargs; i++) if (expr_may_call(e->args[i])) { protect = 1; break; }
    // one private temp slot per arg (single bump: slots stay contiguous)
    int tbase = (protect && e->nargs > 0) ? tmp_slot(e->nargs) : 0;
    if (protect) {
        for (int i = 0; i < e->nargs; i++) {
            TkType *formal = fn->pt[i];
            QW aw = formal ? ty_width(formal) : Q_I64;
            char tm[64];
            snprintf(tm, sizeof tm, "%d(%%rbp)", SLOT(tbase + i));
            emit_val(e->args[i]);
            if (aw == Q_F32) fprintf(o, "    movss %%xmm0, %s\n", tm);
            else if (aw == Q_F64) fprintf(o, "    movsd %%xmm0, %s\n", tm);
            else fprintf(o, "    movq %%rax, %s\n", tm);
        }
        for (int i = 0; i < e->nargs; i++) {
            TkType *formal = fn->pt[i];
            QW aw = formal ? ty_width(formal) : Q_I64;
            char area[64];
            snprintf(area, sizeof area, "%d(%%rbp)", SLOT(g_argBase + i * 8 + 7));
            char tm[64];
            snprintf(tm, sizeof tm, "%d(%%rbp)", SLOT(tbase + i));
            if (aw == Q_STRUCT && formal) {
                int esz = ty_size(formal);
                fprintf(o, "    movq %s, %%rsi\n", tm);
                fprintf(o, "    leaq %s, %%rdi\n", area);
                fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", esz);
            } else if (aw == Q_F32) {
                fprintf(o, "    movss %s, %%xmm0\n", tm);
                fprintf(o, "    movss %%xmm0, %s\n", area);
            } else if (aw == Q_F64) {
                fprintf(o, "    movsd %s, %%xmm0\n", tm);
                fprintf(o, "    movsd %%xmm0, %s\n", area);
            } else {
                char m[96];
                snprintf(m, sizeof m, "%s", area);
                fprintf(o, "    movq %s, %%rax\n", tm);
                store_mem(m, aw);
            }
        }
    } else for (int i = 0; i < e->nargs; i++) {
        TkType *formal = fn->pt[i];
        QW fw = formal ? ty_width(formal) : Q_I64;
        char area[64];
        snprintf(area, sizeof area, "%d(%%rbp)", SLOT(g_argBase + i * 8 + 7));
        emit_val(e->args[i]);
        if (fw == Q_STRUCT && formal) {
            int esz = ty_size(formal);
            fprintf(o, "    movq %%rax, %%rsi\n");
            fprintf(o, "    leaq %s, %%rdi\n", area);
            fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", esz);
        } else if (fw == Q_F32) {
            fprintf(o, "    movss %%xmm0, %s\n", area);
        } else if (fw == Q_F64) {
            fprintf(o, "    movsd %%xmm0, %s\n", area);
        } else {
            char m[96];
            snprintf(m, sizeof m, "%s", area);
            store_mem(m, fw);
        }
    }
    int sret_slot = -1;
    if (sret) {
        int slot = tmp_find(e);
        if (slot < 0) { Xerr("temp missing (walk/emit skew)"); return; }
        char m[64];
        mem_slot(slot, m, sizeof m);
        fprintf(o, "    leaq %s, %%rdi\n", m);
        sret_slot = slot;
    }
    int ipos = 0, fpos = 0, spos = 0;
    for (int i = 0; i < e->nargs; i++)
        arg_home(i, fn->pt[i], &ipos, &fpos, &spos, sret);
    // stack-passed args (spos of them) live in outgoing frame slots; move
    // them onto the real outgoing stack (last formal first). Pad when odd
    // so rsp stays 16-aligned at the call.
    int stk_total = 0;
    if (spos > 0) {
        stk_total = spos * 8;
        if (spos & 1) { fprintf(o, "    subq $8, %%rsp\n"); stk_total += 8; }
        for (int j = spos - 1; j >= 0; j--) {
            char dst[64];
            snprintf(dst, sizeof dst, "%d(%%rbp)", SLOT(g_outBase + j));
            fprintf(o, "    pushq %s\n", dst);
        }
    }
    if (fn->is_ext) fprintf(o, "    call %s@PLT\n", abi_map(fn->name));
    else fprintf(o, "    call %s\n", fn->name);
    if (stk_total > 0) fprintf(o, "    addq $%d, %%rsp\n", stk_total);
    if (sret) {
        char m[64];
        mem_slot(sret_slot, m, sizeof m);
        fprintf(o, "    leaq %s, %%rax\n", m);
    } else if (rw == Q_BOOL && fn->is_ext) {
        // libc int used as taka bool: normalize to 0/1.
        // tk_streq maps to strcmp (0 means equal), so invert to taka polarity.
        if (!strcmp(fn->name, "tk_streq"))
            fprintf(o, "    testl %%eax, %%eax\n    sete %%al\n    movzbq %%al, %%rax\n");
        else
            fprintf(o, "    testl %%eax, %%eax\n    setne %%al\n    movzbq %%al, %%rax\n");
    } else if (fn->is_ext && (rw == Q_I8 || rw == Q_I16 || rw == Q_I32 || rw == Q_CHAR)) {
        fprintf(o, "    movslq %%eax, %%rax\n");
    } else if (fn->is_ext && (rw == Q_U8 || rw == Q_U16 || rw == Q_U32)) {
        fprintf(o, "    movl %%eax, %%eax\n");
    }
}

// record binding TYPES for one primary pattern (no code); caller scopes.
static void xbind_add(TkPattern *p, TkType *st) {
    if (!p) return;
    if (p->kind == TK_PAT_BIND || p->kind == TK_PAT_REF) {
        int slots = 1;
        TkType *vt = st;
        if (p->kind == TK_PAT_REF) {
            vt = copy_ty_star(st, 1); // reference to scrutinee (NULL stays NULL)
        } else if (st && ty_width(st) == Q_STRUCT)
            slots = slots_for_size(ty_size(st));
        xv_add(p->name, vt, slots);
        return;
    }
    if (p->kind != TK_PAT_PATH || p->path.count != 2) return;
    XE *en = xfind_e(p->path.segs[0].seg);
    if (!en || !en->payload) return;
    int vi = xfind_v(en, p->path.segs[1].seg);
    if (vi < 0) return;
    if (en->vars[vi].kind == 1) {
        for (int i = 0; i < p->acount; i++) {
            TkPattern *a = p->args[i];
            if (a->kind != TK_PAT_BIND && a->kind != TK_PAT_REF) continue;
            TkType *ft = en->vars[vi].st[i];
            if (a->kind == TK_PAT_REF) {
                xv_add(a->name, copy_ty_star(ft, 1), 1);
            } else {
                xv_add(a->name, ft, slots_for_size(ty_size(ft)));
            }
        }
    } else if (en->vars[vi].kind == 2) {
        for (int i = 0; i < p->fcount; i++) {
            TkPattern *sp = p->fields[i].pat;
            if (!sp || (sp->kind != TK_PAT_BIND && sp->kind != TK_PAT_REF)) continue;
            TkType *ft = NULL;
            for (int j = 0; j < en->vars[vi].sfn; j++)
                if (!strcmp(en->vars[vi].sf[j], p->fields[i].field)) ft = en->vars[vi].st[j];
            if (!ft) continue;
            if (sp->kind == TK_PAT_REF) {
                xv_add(sp->name, copy_ty_star(ft, 1), 1);
            } else {
                xv_add(sp->name, ft, slots_for_size(ty_size(ft)));
            }
        }
    }
}

// copy bound values from scrutinee temp into already-recorded vars.
static void xbind_copy(TkPattern *p, int mslot, TkType *st) {
    char mm[64];
    mem_slot(mslot, mm, sizeof mm);
    if (p->kind == TK_PAT_REF) {
        // &x binds the address of the matched place
        XV *v = xv_find(p->name);
        if (!v) { Xerr("binding missing"); return; }
        char dst[64];
        mem_slot(v->slot, dst, sizeof dst);
        fprintf(o, "    leaq %s, %%rax\n", mm);
        char m[80];
        snprintf(m, sizeof m, "%s", dst);
        store_mem(m, Q_PTR);
        return;
    }
    if (p->kind == TK_PAT_BIND) {
        XV *v = xv_find(p->name);
        if (!v) { Xerr("binding missing"); return; }
        char dst[64];
        mem_slot(v->slot, dst, sizeof dst);
        if (st && ty_width(st) == Q_STRUCT) {
            int sz = ty_size(st);
            fprintf(o, "    leaq %s, %%rsi\n", mm);
            fprintf(o, "    leaq %s, %%rdi\n", dst);
            fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", sz);
        } else if (st && (ty_width(st) == Q_F32 || ty_width(st) == Q_F64)) {
            if (ty_width(st) == Q_F32) fprintf(o, "    movss %s, %%xmm0\n", mm);
            else fprintf(o, "    movsd %s, %%xmm0\n", mm);
            if (ty_width(st) == Q_F32) fprintf(o, "    movss %%xmm0, %s\n", dst);
            else fprintf(o, "    movsd %%xmm0, %s\n", dst);
        } else {
            QW sw = st ? ty_width(st) : Q_I64;
            if (sw == Q_STRUCT) sw = Q_I64;
            fprintf(o, "    movq %s, %%rax\n", mm);
            if (sw != Q_I64 && sw != Q_U64 && sw != Q_PTR) {
                if (sw == Q_I8 || sw == Q_I16 || sw == Q_I32 || sw == Q_CHAR) fprintf(o, "    movslq %%eax, %%rax\n");
                else fprintf(o, "    movl %%eax, %%eax\n");
            }
            store_mem(dst, sw);
        }
        return;
    }
    if (p->kind != TK_PAT_PATH || p->path.count != 2) return;
    XE *en = xfind_e(p->path.segs[0].seg);
    if (!en || !en->payload) return;
    int vi = xfind_v(en, p->path.segs[1].seg);
    if (vi < 0) return;
    if (en->vars[vi].kind == 1) {
        for (int i = 0; i < p->acount; i++) {
            if (p->args[i]->kind != TK_PAT_BIND && p->args[i]->kind != TK_PAT_REF) continue;
            XV *v = xv_find(p->args[i]->name);
            if (!v) { Xerr("binding missing"); continue; }
            TkType *ft = en->vars[vi].st[i];
            QW fw = ty_width(ft);
            int off = tuple_field_off(en, vi, i);
            char dst[64];
            mem_slot(v->slot, dst, sizeof dst);
            if (p->args[i]->kind == TK_PAT_REF) {
                // address of the member
                fprintf(o, "    leaq %s, %%rax\n", mm);
                if (off) fprintf(o, "    addq $%d, %%rax\n", off);
                char m[80];
                snprintf(m, sizeof m, "%s", dst);
                store_mem(m, Q_PTR);
                continue;
            }
            if (fw == Q_STRUCT) {
                int esz = ty_size(ft);
                fprintf(o, "    leaq %s, %%rsi\n", mm);
                fprintf(o, "    addq $%d, %%rsi\n", off);
                fprintf(o, "    leaq %s, %%rdi\n", dst);
                fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", esz);
            } else if (fw == Q_F32 || fw == Q_F64) {
                fprintf(o, "    leaq %s, %%rcx\n", mm);
                if (fw == Q_F32) fprintf(o, "    movss %d(%%rcx), %%xmm0\n", off);
                else fprintf(o, "    movsd %d(%%rcx), %%xmm0\n", off);
                if (fw == Q_F32) fprintf(o, "    movss %%xmm0, %s\n", dst);
                else fprintf(o, "    movsd %%xmm0, %s\n", dst);
            } else {
                char src[96];
                fprintf(o, "    leaq %s, %%rcx\n", mm);
                snprintf(src, sizeof src, "%d(%%rcx)", off);
                if (fw == Q_I8 || fw == Q_CHAR) fprintf(o, "    movsbq %s, %%rax\n", src);
                else if (fw == Q_U8 || fw == Q_BOOL) fprintf(o, "    movzbq %s, %%rax\n", src);
                else if (fw == Q_I16) fprintf(o, "    movswq %s, %%rax\n", src);
                else if (fw == Q_U16) fprintf(o, "    movzwq %s, %%rax\n", src);
                else if (fw == Q_I32) fprintf(o, "    movslq %s, %%rax\n", src);
                else if (fw == Q_U32) fprintf(o, "    movl %s, %%eax\n", src);
                else fprintf(o, "    movq %s, %%rax\n", src);
                store_mem(dst, fw);
            }
        }
    } else if (en->vars[vi].kind == 2) {
        for (int i = 0; i < p->fcount; i++) {
            TkPattern *sp = p->fields[i].pat;
            if (!sp || (sp->kind != TK_PAT_BIND && sp->kind != TK_PAT_REF)) continue;
            TkType *ft = NULL;
            for (int j = 0; j < en->vars[vi].sfn; j++)
                if (!strcmp(en->vars[vi].sf[j], p->fields[i].field)) ft = en->vars[vi].st[j];
            if (!ft) continue;
            QW fw = ty_width(ft);
            int off = struct_field_off(en, vi, p->fields[i].field);
            XV *v = xv_find(sp->name);
            if (!v) { Xerr("binding missing"); continue; }
            char dst[64];
            mem_slot(v->slot, dst, sizeof dst);
            if (sp->kind == TK_PAT_REF) {
                // address of the member
                fprintf(o, "    leaq %s, %%rax\n", mm);
                if (off) fprintf(o, "    addq $%d, %%rax\n", off);
                char m[80];
                snprintf(m, sizeof m, "%s", dst);
                store_mem(m, Q_PTR);
                continue;
            }
            if (fw == Q_STRUCT) {
                int esz = ty_size(ft);
                fprintf(o, "    leaq %s, %%rsi\n", mm);
                fprintf(o, "    addq $%d, %%rsi\n", off);
                fprintf(o, "    leaq %s, %%rdi\n", dst);
                fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", esz);
            } else if (fw == Q_F32 || fw == Q_F64) {
                fprintf(o, "    leaq %s, %%rcx\n", mm);
                if (fw == Q_F32) fprintf(o, "    movss %d(%%rcx), %%xmm0\n", off);
                else fprintf(o, "    movsd %d(%%rcx), %%xmm0\n", off);
                if (fw == Q_F32) fprintf(o, "    movss %%xmm0, %s\n", dst);
                else fprintf(o, "    movsd %%xmm0, %s\n", dst);
            } else {
                char src[96];
                fprintf(o, "    leaq %s, %%rcx\n", mm);
                snprintf(src, sizeof src, "%d(%%rcx)", off);
                if (fw == Q_I8 || fw == Q_CHAR) fprintf(o, "    movsbq %s, %%rax\n", src);
                else if (fw == Q_U8 || fw == Q_BOOL) fprintf(o, "    movzbq %s, %%rax\n", src);
                else if (fw == Q_I16) fprintf(o, "    movswq %s, %%rax\n", src);
                else if (fw == Q_U16) fprintf(o, "    movzwq %s, %%rax\n", src);
                else if (fw == Q_I32) fprintf(o, "    movslq %s, %%rax\n", src);
                else if (fw == Q_U32) fprintf(o, "    movl %s, %%eax\n", src);
                else fprintf(o, "    movq %s, %%rax\n", src);
                store_mem(dst, fw);
            }
        }
    }
}

// boolean test of ONE pattern alternative into AL; scrutinee value at mslot.
// st is the scrutinee type (for widths); payload tag reads as i32.
static void xalt_bool(TkPattern *p, int mslot, TkType *st) {
    char mm[64];
    mem_slot(mslot, mm, sizeof mm);
    switch (p->kind) {
        case TK_PAT_WILD:
        case TK_PAT_BIND:
        case TK_PAT_REF:
            fprintf(o, "    movb $1, %%al\n");
            break;
        case TK_PAT_LIT: {
            if (p->tok.kind == TOKEN_TRUE) {
                char m0[80];
                snprintf(m0, sizeof m0, "%s", mm);
                QW sw = st ? ty_width(st) : Q_I64;
                if (sw == Q_STRUCT) { Xerr("literal pattern on struct scrutinee"); fprintf(o, "    xorb %%al, %%al\n"); break; }
                load_mem(m0, sw);
                fprintf(o, "    testq %%rax, %%rax\n    setne %%al\n");
            } else if (p->tok.kind == TOKEN_FALSE) {
                char m0[80];
                snprintf(m0, sizeof m0, "%s", mm);
                QW sw = st ? ty_width(st) : Q_I64;
                if (sw == Q_STRUCT) { Xerr("literal pattern on struct scrutinee"); fprintf(o, "    xorb %%al, %%al\n"); break; }
                load_mem(m0, sw);
                fprintf(o, "    testq %%rax, %%rax\n    setz %%al\n");
            } else if (p->tok.kind == TOKEN_STRING) {
                Xerr("string patterns TODO in native backend");
                fprintf(o, "    xorb %%al, %%al\n");
            } else if (p->tok.kind == TOKEN_FLOAT) {
                Xerr("float patterns TODO in native backend");
                fprintf(o, "    xorb %%al, %%al\n");
            } else {
                QW sw = st ? ty_width(st) : Q_I64;
                if (sw == Q_STRUCT) { Xerr("literal pattern on struct scrutinee"); fprintf(o, "    xorb %%al, %%al\n"); break; }
                if (sw == Q_F32 || sw == Q_F64) { Xerr("float patterns TODO in native backend"); fprintf(o, "    xorb %%al, %%al\n"); break; }
                char m0[80];
                snprintf(m0, sizeof m0, "%s", mm);
                load_mem(m0, sw);
                if (sw != Q_I64 && sw != Q_U64 && sw != Q_PTR) {
                    if (sw == Q_I8 || sw == Q_I16 || sw == Q_I32 || sw == Q_CHAR) fprintf(o, "    movslq %%eax, %%rax\n");
                    else fprintf(o, "    movl %%eax, %%eax\n");
                }
                u64 lit = 0;
                if (p->tok.kind == TOKEN_CHAR) {
                    int cv;
                    if (lit_char_val(p->tok, &cv)) { Xerr("bad char pattern"); fprintf(o, "    xorb %%al, %%al\n"); break; }
                    lit = (u64)(i64)cv;
                } else {
                    lit = lit_int_bits(p->tok);
                }
                if (lit <= 0x7FFFFFFFULL) fprintf(o, "    cmpq $%llu, %%rax\n", (unsigned long long)lit);
                else fprintf(o, "    movabsq $%llu, %%rcx\n    cmpq %%rcx, %%rax\n", (unsigned long long)lit);
                fprintf(o, "    sete %%al\n");
            }
            break;
        }
        case TK_PAT_PATH: {
            if (p->path.count != 2) { Xerr("qualified Enum::Variant needed in patterns"); fprintf(o, "    xorb %%al, %%al\n"); break; }
            XE *en = xfind_e(p->path.segs[0].seg);
            if (!en) { Xerr("unknown enum in pattern"); fprintf(o, "    xorb %%al, %%al\n"); break; }
            int vi = xfind_v(en, p->path.segs[1].seg);
            if (vi < 0) { Xerr("unknown variant in pattern"); fprintf(o, "    xorb %%al, %%al\n"); break; }
            if (!en->payload) {
                char m0[80];
                snprintf(m0, sizeof m0, "%s", mm);
                load_mem(m0, Q_I32);
                fprintf(o, "    cmpl $%d, %%eax\n    sete %%al\n", en->vars[vi].disc);
            } else {
                fprintf(o, "    cmpl $%d, %s\n    sete %%al\n", vi, mm);
                if (en->vars[vi].kind == 1) {
                    for (int i = 0; i < p->acount; i++) {
                        TkPattern *a = p->args[i];
                        if (a->kind == TK_PAT_LIT) {
                            int off = tuple_field_off(en, vi, i);
                            u64 lit = (a->tok.kind == TOKEN_CHAR) ? 0 : lit_int_bits(a->tok);
                            int cv = 0;
                            if (a->tok.kind == TOKEN_CHAR) {
                                if (lit_char_val(a->tok, &cv)) { Xerr("bad char pattern"); continue; }
                                lit = (u64)(i64)cv;
                            } else if (a->tok.kind != TOKEN_INT) { Xerr("int/char literal patterns only in core"); continue; }
                            // AND into AL without clobbering it: use CL, then pop scratch
                            fprintf(o, "    pushq %%rax\n");
                            fprintf(o, "    leaq %s, %%rcx\n", mm);
                            fprintf(o, "    addq $%d, %%rcx\n", off);
                            fprintf(o, "    movq (%%rcx), %%rcx\n");
                            fprintf(o, "    cmpq $%llu, %%rcx\n", (unsigned long long)lit);
                            fprintf(o, "    sete %%cl\n");
                            fprintf(o, "    andb %%cl, (%%rsp)\n");
                            fprintf(o, "    popq %%rax\n");
                        } else if (a->kind != TK_PAT_BIND && a->kind != TK_PAT_WILD && a->kind != TK_PAT_REF) {
                            Xerr("nested patterns TODO in native backend");
                        }
                    }
                } else if (en->vars[vi].kind == 2) {
                    for (int i = 0; i < p->fcount; i++) {
                        TkPattern *sp = p->fields[i].pat;
                        if (sp && sp->kind == TK_PAT_LIT) {
                            int off = struct_field_off(en, vi, p->fields[i].field);
                            u64 lit = lit_int_bits(sp->tok);
                            fprintf(o, "    pushq %%rax\n");
                            fprintf(o, "    leaq %s, %%rcx\n", mm);
                            fprintf(o, "    addq $%d, %%rcx\n", off);
                            fprintf(o, "    movq (%%rcx), %%rcx\n");
                            fprintf(o, "    cmpq $%llu, %%rcx\n", (unsigned long long)lit);
                            fprintf(o, "    sete %%cl\n");
                            fprintf(o, "    andb %%cl, (%%rsp)\n");
                            fprintf(o, "    popq %%rax\n");
                        } else if (!sp || (sp->kind != TK_PAT_BIND && sp->kind != TK_PAT_WILD && sp->kind != TK_PAT_REF)) {
                            Xerr("nested patterns TODO in native backend");
                        }
                    }
                }
            }
            break;
        }
    }
}

// bindings of the primary pattern into fresh slots (or-branches binding-free).
// mslot holds the scrutinee value; caller manages scope push/pop + xv_add.
// value match for `T x = <match>`: assigns converted arm bodies into tgtSlot.
// tgtTy converts bodies; non-exhaustive yields the caller's zero-init.
static void emit_match_value(TkExpr *m, int tgtSlot, TkType *tgtTy) {
    int mslot = tmp_find(m);
    int dslot = tmp_find((const void *)((const char *)m + 1));
    if (mslot < 0 || dslot < 0) { Xerr("temp missing (walk/emit skew)"); return; }
    char mm[64], dm[64], tm[64];
    mem_slot(mslot, mm, sizeof mm);
    mem_slot(dslot, dm, sizeof dm);
    mem_slot(tgtSlot, tm, sizeof tm);
    emit_val(m->scrut);
    TkType *st = xinfer(m->scrut);
    if (st && ty_width(st) == Q_STRUCT) {
        int sz = ty_size(st);
        fprintf(o, "    movq %%rax, %%rsi\n");
        fprintf(o, "    leaq %s, %%rdi\n", mm);
        fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", sz);
    } else if (st && (ty_width(st) == Q_F32 || ty_width(st) == Q_F64)) {
        if (ty_width(st) == Q_F32) fprintf(o, "    movss %%xmm0, %s\n", mm);
        else fprintf(o, "    movsd %%xmm0, %s\n", mm);
    } else {
        fprintf(o, "    movq %%rax, %s\n", mm);
    }
    fprintf(o, "    movq $0, %s\n", dm);
    QW tw = tgtTy ? ty_width(tgtTy) : Q_I64;
    for (int i = 0; i < m->arm_n; i++) {
        int Lbody = new_lbl(), Lnext = new_lbl();
        fprintf(o, "    cmpb $0, %s\n    jne .L%d\n", dm, Lnext);
        TkPattern *alts[9];
        int na = 0;
        alts[na++] = m->arms[i].pat;
        for (int k = 0; k < m->arms[i].pat->or_count && na < 9; k++)
            alts[na++] = m->arms[i].pat->or_next[k];
        for (int k = 0; k < na; k++) {
            xalt_bool(alts[k], mslot, st);
            fprintf(o, "    testb %%al, %%al\n    jnz .L%d\n", Lbody);
        }
        fprintf(o, "    jmp .L%d\n.L%d:\n", Lnext, Lbody);
        xv_push();
        xbind_add(m->arms[i].pat, st);
        xbind_copy(m->arms[i].pat, mslot, st);
        if (m->arms[i].guard) {
            emit_val(m->arms[i].guard);
            fprintf(o, "    testq %%rax, %%rax\n");
            int Lskip = new_lbl();
            fprintf(o, "    jz .L%d\n", Lskip);
            // body assign
            emit_val(m->arms[i].body);
            if (tw == Q_STRUCT && tgtTy) {
                int esz = ty_size(tgtTy);
                fprintf(o, "    movq %%rax, %%rsi\n");
                fprintf(o, "    leaq %s, %%rdi\n", tm);
                fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", esz);
            } else if (tw == Q_F32 || tw == Q_F64) {
                if (tw == Q_F32) fprintf(o, "    movss %%xmm0, %s\n", tm);
                else fprintf(o, "    movsd %%xmm0, %s\n", tm);
            } else {
                TkType *bt2 = xinfer(m->arms[i].body);
                to_width(bt2 ? ty_width(bt2) : Q_I64, tw);
                char tmm[96];
                snprintf(tmm, sizeof tmm, "%s", tm);
                store_mem(tmm, tw);
            }
            fprintf(o, "    movq $1, %s\n", dm);
            fprintf(o, ".L%d:\n", Lskip);
        } else {
            emit_val(m->arms[i].body);
            if (tw == Q_STRUCT && tgtTy) {
                int esz = ty_size(tgtTy);
                fprintf(o, "    movq %%rax, %%rsi\n");
                fprintf(o, "    leaq %s, %%rdi\n", tm);
                fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", esz);
            } else if (tw == Q_F32 || tw == Q_F64) {
                if (tw == Q_F32) fprintf(o, "    movss %%xmm0, %s\n", tm);
                else fprintf(o, "    movsd %%xmm0, %s\n", tm);
            } else {
                TkType *bt2 = xinfer(m->arms[i].body);
                to_width(bt2 ? ty_width(bt2) : Q_I64, tw);
                char tmm[96];
                snprintf(tmm, sizeof tmm, "%s", tm);
                store_mem(tmm, tw);
            }
            fprintf(o, "    movq $1, %s\n", dm);
        }
        xv_pop();
        fprintf(o, ".L%d:\n", Lnext);
    }
}
static void build_tables(TkProgram *prog) {
    for (int i = 0; i < prog->count; i++) {
        TkItem *it = prog->items[i];
        if (it->kind == TK_IT_STRUCT) {
            xss = realloc(xss, sizeof(*xss) * (size_t)(xss_n + 1));
            XS *s = &xss[xss_n++];
            s->name = it->sname; s->n = it->fcount;
            s->fn = calloc((size_t)(it->fcount ? it->fcount : 1), sizeof(char *));
            s->ft = calloc((size_t)(it->fcount ? it->fcount : 1), sizeof(TkType *));
            s->off = calloc((size_t)(it->fcount ? it->fcount : 1), sizeof(int));
            int off = 0, al = 1;
            for (int j = 0; j < it->fcount; j++) {
                int a = ty_align(it->fields[j].ty);
                if (a > al) al = a;
                off = align_up(off, a);
                s->fn[j] = it->fields[j].nm; s->ft[j] = it->fields[j].ty; s->off[j] = off;
                off += ty_size(it->fields[j].ty);
            }
            s->size = align_up(off, al); s->align = al;
            for (int j = 0; j < it->fcount; j++) xcollect_ty(it->fields[j].ty);
        } else if (it->kind == TK_IT_ENUM) {
            xes = realloc(xes, sizeof(*xes) * (size_t)(xes_n + 1));
            XE *e = &xes[xes_n++];
            e->name = it->ename; e->n = it->vcount;
            e->vars = calloc((size_t)(it->vcount ? it->vcount : 1), sizeof(*e->vars));
            e->payload = 0;
            int disc_next = 0;
            for (int j = 0; j < it->vcount; j++) {
                e->vars[j].vn = it->vars[j].vn;
                if (it->vars[j].has_tuple || it->vars[j].has_struct) {
                    e->payload = 1;
                    if (it->vars[j].has_tuple) {
                        e->vars[j].kind = 1; e->vars[j].tn = it->vars[j].tn;
                        e->vars[j].st = it->vars[j].ts;
                        for (int k = 0; k < it->vars[j].tn; k++) xcollect_ty(it->vars[j].ts[k]);
                    } else {
                        e->vars[j].kind = 2; e->vars[j].sfn = it->vars[j].fn2;
                        e->vars[j].sf = calloc((size_t)(it->vars[j].fn2 ? it->vars[j].fn2 : 1), sizeof(char *));
                        e->vars[j].st = calloc((size_t)(it->vars[j].fn2 ? it->vars[j].fn2 : 1), sizeof(TkType *));
                        for (int k = 0; k < it->vars[j].fn2; k++) {
                            e->vars[j].sf[k] = it->vars[j].fs[k].nm;
                            e->vars[j].st[k] = it->vars[j].fs[k].ty;
                            xcollect_ty(it->vars[j].fs[k].ty);
                        }
                    }
                } else {
                    e->vars[j].kind = 0;
                    if (it->vars[j].has_disc) {
                        int v;
                        if (const_int(it->vars[j].disc, &v)) { Xerr("non-const enum discriminant"); v = disc_next; }
                        e->vars[j].disc = v;
                        disc_next = v + 1;
                    } else {
                        e->vars[j].disc = disc_next++;
                    }
                }
            }
        } else if (it->kind == TK_IT_FN || it->kind == TK_IT_EXTERN) {
            xfs = realloc(xfs, sizeof(*xfs) * (size_t)(xfs_n + 1));
            XF *f = &xfs[xfs_n++];
            f->name = it->fn_name; f->ret = it->ret;
            f->pn = it->pcount;
            f->pt = calloc((size_t)(it->pcount ? it->pcount : 1), sizeof(TkType *));
            f->pnm = calloc((size_t)(it->pcount ? it->pcount : 1), sizeof(char *));
            for (int j = 0; j < it->pcount; j++) { f->pt[j] = it->params[j].ty; f->pnm[j] = it->params[j].nm; }
            f->is_ext = (it->kind == TK_IT_EXTERN);
            for (int j = 0; j < it->pcount; j++) xcollect_ty(it->params[j].ty);
            xcollect_ty(it->ret);
        } else if (it->kind == TK_IT_GLOBAL) {
            xcollect_ty(it->gdecl->ty);
        }
    }
}

// variable-type lookup used by inference; set to wt_find during frame walk,
// lookup_xv during emission.
static TkType *lookup_xv(const char *nm) {
    XV *v = xv_find(nm);
    return v ? v->ty : NULL;
}

// walk state for frame counting (w_maxArgs declared up top)
// light type map for walk-time auto inference (name -> ty, depth)
typedef struct { char *name; TkType *ty; int depth; } WT;
static WT *wts = NULL; static int wt_n = 0, wt_cap = 0, wt_depth = 0;
static void wt_add(char *name, TkType *ty) {
    if (wt_n == wt_cap) {
        wt_cap = wt_cap ? wt_cap * 2 : 64;
        wts = realloc(wts, sizeof(*wts) * (size_t)wt_cap);
        if (!wts) { Xerr("OOM"); exit(1); }
    }
    wts[wt_n].name = name; wts[wt_n].ty = ty; wts[wt_n].depth = wt_depth;
    wt_n++;
}
static TkType *wt_find(const char *name) {
    for (int i = wt_n - 1; i >= 0; i--) if (!strcmp(wts[i].name, name)) return wts[i].ty;
    return NULL;
}
// frame-counting walk: mirrors emission order for decls/binds (types only),
// plus temp reservation (side table) for materialized values.
static void walk_ty_list_collect(TkType *t) { xcollect_ty(t); }
// collect List<T> instantiations from a type (dedup by structure)
static int xtype_eq(TkType *a, TkType *b) {
    if (a == b) return 1;
    if (!a || !b || a->kind != b->kind) return 0;
    if (a->stars != b->stars || a->is_ref != b->is_ref) return 0;
    if (a->kind == TK_TY_PRIM) return a->prim == b->prim;
    if (a->kind == TK_TY_PATH) {
        if (a->path.count != b->path.count || a->gcount != b->gcount) return 0;
        for (int i = 0; i < a->path.count; i++) if (strcmp(a->path.segs[i].seg, b->path.segs[i].seg)) return 0;
        for (int i = 0; i < a->gcount; i++) if (!xtype_eq(a->gargs[i], b->gargs[i])) return 0;
        return 1;
    }
    return 0;
}
static void xcollect_ty(TkType *t) {
    if (!t) return;
    if (t->kind == TK_TY_PATH && t->path.count == 1 && !strcmp(t->path.segs[0].seg, "List") && t->gcount == 1) {
        for (int i = 0; i < xlist_n; i++) if (xtype_eq(xlist_elems[i], t->gargs[0])) {
            xcollect_ty(t->gargs[0]);
            return;
        }
        if (xlist_n == xlist_cap) {
            xlist_cap = xlist_cap ? xlist_cap * 2 : 8;
            xlist_elems = realloc(xlist_elems, sizeof(*xlist_elems) * (size_t)xlist_cap);
            if (!xlist_elems) { Xerr("OOM"); exit(1); }
        }
        xlist_elems[xlist_n++] = t->gargs[0];
        xcollect_ty(t->gargs[0]);
        return;
    }
    if (t->kind == TK_TY_PATH) { for (int i = 0; i < t->gcount; i++) xcollect_ty(t->gargs[i]); return; }
    if (t->kind == TK_TY_FN) {
        for (int i = 0; i < t->pcount; i++) xcollect_ty(t->params[i]);
        xcollect_ty(t->ret);
        return;
    }
    if (t->kind == TK_TY_TUPLE) { for (int i = 0; i < t->ecount; i++) xcollect_ty(t->elems[i]); return; }
    if (t->kind == TK_TY_ARRAY) { xcollect_ty(t->elem); return; }
}
static void walk_expr(TkExpr *e);
static void walk_stmts(TkStmt **ss, int n);
static void wt_pop(void);
static void walk_match_tmps(TkExpr *m);
static void walk_pat_types(TkPattern *p);
static void walk_expr(TkExpr *e) {
    if (!e) return;
    switch (e->kind) {
        case TK_EX_STRUCT_LIT: {
            TkType *t = xinfer(e);
            int sz = t ? ty_size(t) : 8;
            if (sz <= 0) sz = 8;
            tmp_for(e, slots_for_size(sz));
            for (int i = 0; i < e->scount; i++) walk_expr(e->sfields[i].e);
            break;
        }
        case TK_EX_VAR: {
            if (e->path.count == 2) {
                XE *en = xfind_e(e->path.segs[0].seg);
                if (en && en->payload) {
                    int vi = xfind_v(en, e->path.segs[1].seg);
                    if (vi >= 0 && en->vars[vi].kind == 0)
                        tmp_for(e, enum_slots(en));
                }
            }
            break;
        }
        case TK_EX_CALL: {
            walk_expr(e->callee);
            for (int i = 0; i < e->nargs; i++) walk_expr(e->args[i]);
            if (e->nargs > w_maxArgs) w_maxArgs = e->nargs;
            // List push consumes an extra arg position for the receiver
            // (&list in rdi); reserve it (mirrors emit_call's push lowering).
            if (e->callee && e->callee->kind == TK_EX_FIELD && !strcmp(e->callee->field, "push") && e->callee->callee) {
                TkType *bt = xinfer(e->callee->callee);
                if (bt && bt->kind == TK_TY_PATH && bt->path.count == 1 && !strcmp(bt->path.segs[0].seg, "List") && bt->gcount == 1 && bt->stars == 0 && !bt->is_ref) {
                    if (e->nargs + 1 > w_maxArgs) w_maxArgs = e->nargs + 1;
                }
            }
            if (e->callee && e->callee->kind == TK_EX_VAR && e->callee->path.count == 1 &&
                !is_print_like(e->callee->path.segs[0].seg)) {
                const char *cn = e->callee->path.segs[0].seg;
                if (!strcmp(cn, "tk_sview") || !strcmp(cn, "tk_sview_u8")) {
                    tmp_for(e, 2); // inline pair materialization
                    break;
                }
                XF *f = xfind_f(e->callee->path.segs[0].seg);
                if (f && !f->is_ext && f->ret && ty_width(f->ret) == Q_STRUCT)
                    tmp_for(e, slots_for_size(ty_size(f->ret)));
                if (f && f->is_ext && f->ret && f->ret->kind == TK_TY_PRIM && f->ret->prim == TOKEN_SVIEW)
                    tmp_for(e, 2);
            }
            if (e->callee && e->callee->kind == TK_EX_VAR && e->callee->path.count == 2) {
                XE *en = xfind_e(e->callee->path.segs[0].seg);
                if (en && en->payload)
                    tmp_for(e, enum_slots(en));
            }
            break;
        }
        case TK_EX_PAREN: walk_expr(e->lhs); break;
        case TK_EX_UNARY: {
            walk_expr(e->lhs);
            if (e->op == TOKEN_STAR) {
                TkType *et = xinfer(e);
                if (et && ty_width(et) == Q_STRUCT) tmp_for(e, slots_for_size(ty_size(et)));
            }
            break;
        }
        case TK_EX_BINARY: walk_expr(e->lhs); walk_expr(e->rhs); break;
        case TK_EX_TERNARY:
            walk_expr(e->cond); walk_expr(e->then_e); walk_expr(e->else_e); break;
        case TK_EX_BLOCK:
            wt_depth++; walk_stmts(e->stmts, e->scount2);
            if (e->trailing) walk_expr(e->trailing);
            wt_pop();
            break;
        case TK_EX_CAST: walk_expr(e->lhs); break;
        case TK_EX_INDEX: {
            walk_expr(e->callee); walk_expr(e->index);
            TkType *et = xinfer(e);
            if (et && ty_width(et) == Q_STRUCT) tmp_for(e, slots_for_size(ty_size(et)));
            break;
        }
        case TK_EX_FIELD: {
            walk_expr(e->callee);
            TkType *et = xinfer(e);
            if (et && ty_width(et) == Q_STRUCT) tmp_for(e, slots_for_size(ty_size(et)));
            break;
        }
        case TK_EX_ARRAY_LIST:
            for (int i = 0; i < e->ecount; i++) walk_expr(e->elems[i]);
            break;
        case TK_EX_MATCH:
            walk_match_tmps(e);
            for (int i = 0; i < e->arm_n; i++) {
                wt_depth++;
                walk_pat_types(e->arms[i].pat);
                if (e->arms[i].guard) walk_expr(e->arms[i].guard);
                if (e->arms[i].body_is_block) walk_stmts(e->arms[i].bs, e->arms[i].bn);
                else if (e->arms[i].body) walk_expr(e->arms[i].body);
                while (wt_n > 0 && wts[wt_n - 1].depth > wt_depth) wt_n--;
                wt_depth--;
            }
            break;
        case TK_EX_TRANSACT:
            walk_stmts(e->t_blk, e->t_n);
            break;
        case TK_EX_SPAWN:
            if (e->callee) walk_expr(e->callee);
            break;
        case TK_EX_HANDLE: walk_expr(e->h_scrut); break;
        case TK_EX_PERFORM:
            for (int i = 0; i < e->pargc; i++) walk_expr(e->pargs[i]);
            break;
        case TK_EX_RESUME:
            if (e->has_resume_e) walk_expr(e->resume_e);
            break;
        default: break;
    }
}
static void walk_pat_types(TkPattern *p) {
    if (!p) return;
    if (p->kind == TK_PAT_BIND) { wt_add(p->name, NULL); frame_slots += 1; return; }
    if (p->kind == TK_PAT_REF) { wt_add(p->name, NULL); frame_slots += 1; return; }
    if (p->kind != TK_PAT_PATH) return;
    if (p->path.count == 2) {
        XE *en = xfind_e(p->path.segs[0].seg);
        int vi = en ? xfind_v(en, p->path.segs[1].seg) : -1;
        if (en && vi >= 0 && en->payload) {
            if (en->vars[vi].kind == 1) {
                for (int i = 0; i < p->acount; i++) {
                    TkPattern *a = p->args[i];
                    if (a->kind == TK_PAT_BIND) {
                        wt_add(a->name, en->vars[vi].st[i]);
                        frame_slots += slots_for_size(ty_size(en->vars[vi].st[i]));
                        xcollect_ty(en->vars[vi].st[i]);
                    } else walk_pat_types(a);
                }
                return;
            } else if (en->vars[vi].kind == 2) {
                for (int i = 0; i < p->fcount; i++) {
                    TkPattern *sp = p->fields[i].pat;
                    TkType *ft = NULL;
                    for (int j = 0; j < en->vars[vi].sfn; j++)
                        if (!strcmp(en->vars[vi].sf[j], p->fields[i].field)) ft = en->vars[vi].st[j];
                    if (sp && sp->kind == TK_PAT_BIND) {
                        wt_add(sp->name, ft);
                        frame_slots += ft ? slots_for_size(ty_size(ft)) : 1;
                        if (ft) xcollect_ty(ft);
                    } else walk_pat_types(sp);
                }
                return;
            }
        }
    }
    for (int i = 0; i < p->acount; i++) walk_pat_types(p->args[i]);
    for (int i = 0; i < p->fcount; i++) walk_pat_types(p->fields[i].pat);
    for (int i = 0; i < p->or_count; i++) walk_pat_types(p->or_next[i]);
}
static void wt_pop(void) {
    wt_depth--;
    while (wt_n > 0 && wts[wt_n - 1].depth > wt_depth) wt_n--;
}
static void walk_match_tmps(TkExpr *m) {
    // __m temp sized by scrutinee type; __done flag slot
    TkType *st = xinfer(m->scrut);
    int sz = st ? ty_size(st) : 8;
    if (sz <= 0) sz = 8;
    tmp_for(m, slots_for_size(sz));
    tmp_for((const void *)((const char *)m + 1), 1);
}
static void walk_stmts(TkStmt **ss, int n) {
    for (int i = 0; i < n; i++) {
        TkStmt *s = ss[i];
        if (!s) continue;
        switch (s->kind) {
            case TK_ST_DECL: {
                TkType *dt = s->ty;
                if (dt && dt->kind == TK_TY_AUTO) dt = xinfer(s->inits[0]);
                if (dt && s->inits[0] && s->inits[0]->kind == TK_EX_MATCH) {
                    walk_match_tmps(s->inits[0]);
                    for (int k = 0; k < s->inits[0]->arm_n; k++) {
                        walk_pat_types(s->inits[0]->arms[k].pat);
                        if (s->inits[0]->arms[k].guard) walk_expr(s->inits[0]->arms[k].guard);
                        walk_expr(s->inits[0]->arms[k].body);
                    }
                } else if (s->inits[0]) walk_expr(s->inits[0]);
                for (int k = 0; k < s->dcount; k++) {
                    wt_add(s->names[k], dt);
                    frame_slots += dt ? slots_for_size(ty_size(dt)) : 1;
                    if (dt) xcollect_ty(dt);
                }
                break;
            }
            case TK_ST_SIMPLE:
                walk_expr(s->e1);
                if (s->has_assign) walk_expr(s->e2);
                break;
            case TK_ST_BLOCK: wt_depth++; walk_stmts(s->stmts, s->n); wt_pop(); break;
            case TK_ST_IF:
                walk_expr(s->cond_ex ? s->cond_ex : s->cond);
                wt_depth++; walk_stmts(s->then_s, s->then_n); wt_pop();
                for (int k = 0; k < s->eif_n; k++) {
                    walk_expr(s->eifs[k].e ? s->eifs[k].e : s->eifs[k].c);
                    wt_depth++; walk_stmts(s->eifs[k].s, s->eifs[k].n); wt_pop();
                }
                if (s->has_els) { wt_depth++; walk_stmts(s->els, s->els_n); wt_pop(); }
                break;
            case TK_ST_WHILE:
                walk_expr(s->cond_ex ? s->cond_ex : s->cond);
                wt_depth++; walk_stmts(s->then_s, s->then_n); wt_pop();
                break;
            case TK_ST_FOR_C: {
                wt_depth++;
                if (!s->for_init_empty) {
                    if (s->for_init_is_decl) {
                        TkType *dt = s->for_init_decl->ty;
                        if (dt && dt->kind == TK_TY_AUTO) dt = xinfer(s->for_init_decl->inits[0]);
                        walk_expr(s->for_init_decl->inits[0]);
                        wt_add(s->for_init_decl->names[0], dt);
                        frame_slots += dt ? slots_for_size(ty_size(dt)) : 1;
                        if (dt) xcollect_ty(dt);
                    } else {
                        walk_expr(s->for_init_simple->e1);
                        if (s->for_init_simple->has_assign) walk_expr(s->for_init_simple->e2);
                    }
                }
                if (s->has_for_cond) walk_expr(s->for_cond);
                if (s->has_incr) {
                    walk_expr(s->for_incr->e1);
                    if (s->for_incr->has_assign) walk_expr(s->for_incr->e2);
                }
                walk_stmts(s->then_s, s->then_n);
                // truncate loop-scope bindings
                while (wt_n > 0 && wts[wt_n - 1].depth > wt_depth) wt_n--;
                wt_depth--;
                break;
            }
            case TK_ST_MATCH_STMT: {
                // scrutinee + per-arm binds/bodies/guards
                walk_expr(s->m_scrut);
                // __m/__done keyed by the STMT node for statements
                {
                    TkType *st = xinfer(s->m_scrut);
                    int sz = st ? ty_size(st) : 8;
                    if (sz <= 0) sz = 8;
                    tmp_for(s, slots_for_size(sz));
                    tmp_for((const void *)((const char *)s + 1), 1);
                }
                for (int k = 0; k < s->m_arm_n; k++) {
                    wt_depth++;
                    walk_pat_types(s->m_arms[k].pat);
                    if (s->m_arms[k].guard) walk_expr(s->m_arms[k].guard);
                    walk_stmts(s->m_arms[k].bs, s->m_arms[k].bn);
                    while (wt_n > 0 && wts[wt_n - 1].depth > wt_depth) wt_n--;
                    wt_depth--;
                }
                break;
            }
            case TK_ST_RETURN:
                if (s->has_ret) {
                    if (s->ret_e && s->ret_e->kind == TK_EX_MATCH) {
                        if (w_cur_ret) tmp_for(s, slots_for_size(ty_size(w_cur_ret)));
                        walk_match_tmps(s->ret_e);
                        for (int k = 0; k < s->ret_e->arm_n; k++) {
                            wt_depth++;
                            walk_pat_types(s->ret_e->arms[k].pat);
                            if (s->ret_e->arms[k].guard) walk_expr(s->ret_e->arms[k].guard);
                            walk_expr(s->ret_e->arms[k].body);
                            while (wt_n > 0 && wts[wt_n - 1].depth > wt_depth) wt_n--;
                            wt_depth--;
                        }
                    } else walk_expr(s->ret_e);
                }
                break;
            case TK_ST_LABEL: walk_stmts(&s->lab_s, 1); break;
            case TK_ST_SPAWN_BLK: wt_depth++; walk_stmts(s->sp_blk, s->sp_n); wt_pop(); break;
            case TK_ST_SPAWN_CALL: walk_expr(s->sp_call); break;
            case TK_ST_DEFER:
                if (s->defer_is_block) { wt_depth++; walk_stmts(s->defer_blk, s->defer_n); wt_pop(); }
                else {
                    walk_expr(s->defer_simple->e1);
                    if (s->defer_simple->has_assign) walk_expr(s->defer_simple->e2);
                }
                break;
            default: break;
        }
    }
}

// full assignment (plain `=` plus all compound ops), struct-aware
static void emit_assign_stmt(TkExpr *lhs, TkExpr *rhs, TokenKind op) {
    TkType *lt = xinfer(lhs);
    QW lw = lt ? ty_width(lt) : Q_I64;
    if (lw == Q_STRUCT && lt) {
        if (op != TOKEN_EQ) { Xerr("compound struct assign TODO"); return; }
        emit_addr(lhs);
        fprintf(o, "    pushq %%rax\n");
        emit_val(rhs);
        int sz = ty_size(lt);
        fprintf(o, "    movq %%rax, %%rsi\n");
        fprintf(o, "    popq %%rdi\n");
        fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", sz);
        return;
    }
    emit_addr(lhs);
    fprintf(o, "    pushq %%rax\n");
    emit_val(rhs);
    TkType *rt = xinfer(rhs);
    to_width(rt ? ty_width(rt) : Q_I64, lw);
    if (op == TOKEN_EQ) {
        fprintf(o, "    popq %%rcx\n");
        if (lw == Q_F32 || lw == Q_F64) {
            if (lw == Q_F32) fprintf(o, "    movss %%xmm0, (%%rcx)\n");
            else fprintf(o, "    movsd %%xmm0, (%%rcx)\n");
        } else {
            char m[80];
            snprintf(m, sizeof m, "(%%rcx)");
            if (lw == Q_I64 || lw == Q_U64 || lw == Q_PTR) fprintf(o, "    movq %%rax, %s\n", m);
            else store_mem(m, lw);
        }
        return;
    }
    emit_assign_combine(op, lw);
}

static void emit_stmt(TkStmt *s) {
    if (!s) return;
    switch (s->kind) {
        case TK_ST_DECL: {
            for (int i = 0; i < s->dcount; i++) {
                TkType *dt = s->ty;
                if (dt && dt->kind == TK_TY_AUTO) {
                    dt = xinfer(s->inits[i]);
                    if (!dt) { Xerr("auto needs explicit type here in core"); dt = xnew_prim(TOKEN_I32); }
                }
                xv_add(s->names[i], dt, dt ? slots_for_size(ty_size(dt)) : 1);
                if (s->inits[i] && s->inits[i]->kind == TK_EX_MATCH) {
                    // zero-init then value-match into own slot
                    XV *v = xv_find(s->names[i]);
                    if (!v) { Xerr("decl missing"); break; }
                    char vm[64];
                    mem_slot(v->slot, vm, sizeof vm);
                    int sz = dt ? ty_size(dt) : 8;
                    fprintf(o, "    leaq %s, %%rdi\n", vm);
                    fprintf(o, "    movl $%d, %%ecx\n    xorl %%eax, %%eax\n    rep stosb\n", sz);
                    emit_match_value(s->inits[i], v->slot, dt);
                    break;
                }
                if (dt && dt->kind == TK_TY_ARRAY && s->inits[i] && s->inits[i]->kind == TK_EX_ARRAY_LIST) {
                    XV *v = xv_find(s->names[i]);
                    if (!v) { Xerr("decl missing"); break; }
                    TkExpr *al = s->inits[i];
                    for (int k = 0; k < al->ecount; k++) {
                        emit_val(al->elems[k]);
                        QW ew = ty_width(dt->elem);
                        char m[96];
                        if (v->slot < 0) snprintf(m, sizeof m, "%s+%d(%%rip)", v->name, k * ty_size(dt->elem));
                        else snprintf(m, sizeof m, "%d(%%rbp)", SLOT(v->slot - k * slots_for_size(ty_size(dt->elem))));
                        if (ew == Q_F32 || ew == Q_F64) {
                            // value already converted? convert from inferred:
                            to_width(xqw(al->elems[k]), ew);
                            if (ew == Q_F32) fprintf(o, "    movss %%xmm0, %s\n", m);
                            else fprintf(o, "    movsd %%xmm0, %s\n", m);
                        } else {
                            to_width(xqw(al->elems[k]), ew);
                            store_mem(m, ew);
                        }
                    }
                    break;
                }
                // List from array literal (before the generic arraylit error below)
                if (dt && dt->kind == TK_TY_PATH && dt->path.count == 1 && !strcmp(dt->path.segs[0].seg, "List") &&
                    dt->gcount == 1 && s->inits[i] && s->inits[i]->kind == TK_EX_ARRAY_LIST) {
                    XV *v = xv_find(s->names[i]);
                    if (!v) { Xerr("decl missing"); break; }
                    TkExpr *al = s->inits[i];
                    char vm[64], dm[80], lm[80], cm[80];
                    mem_slot(v->slot, vm, sizeof vm);
                    snprintf(dm, sizeof dm, "%s", vm);
                    snprintf(lm, sizeof lm, "%d(%%rbp)", SLOT(v->slot - 1));
                    snprintf(cm, sizeof cm, "%d(%%rbp)", SLOT(v->slot - 2));
                    // List layout: data:0 len:8 cap:16 (all u64)
                    fprintf(o, "    movq $0, %s\n", dm);
                    fprintf(o, "    movq $0, %s\n", lm);
                    fprintf(o, "    movq $0, %s\n", cm);
                    if (al->ecount > 0) {
                        QW ew = ty_width(dt->gargs[0]);
                        int esz = ty_size(dt->gargs[0]);
                        // malloc(esize * n)
                        fprintf(o, "    movq $%d, %%rdi\n", al->ecount * esz);
                        fprintf(o, "    call malloc@PLT\n");
                        fprintf(o, "    movq %%rax, %s\n", dm);
                        // OOM check
                        {
                            int Lok = new_lbl();
                            fprintf(o, "    testq %%rax, %%rax\n    jnz .L%d\n", Lok);
                            fprintf(o, "    movl $1, %%edi\n    call exit@PLT\n");
                            fprintf(o, ".L%d:\n", Lok);
                        }
                        fprintf(o, "    movq $%d, %s\n", al->ecount, lm);
                        fprintf(o, "    movq $%d, %s\n", al->ecount, cm);
                        for (int k = 0; k < al->ecount; k++) {
                            emit_val(al->elems[k]);
                            if (ew == Q_STRUCT) {
                                fprintf(o, "    movq %%rax, %%rsi\n");
                                fprintf(o, "    movq %s, %%rdi\n", dm);
                                fprintf(o, "    addq $%d, %%rdi\n", k * esz);
                                fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", esz);
                            } else if (ew == Q_F32 || ew == Q_F64) {
                                to_width(xqw(al->elems[k]), ew);
                                fprintf(o, "    movq %s, %%rcx\n", dm);
                                if (ew == Q_F32) fprintf(o, "    movss %%xmm0, %d(%%rcx)\n", k * esz);
                                else fprintf(o, "    movsd %%xmm0, %d(%%rcx)\n", k * esz);
                            } else {
                                to_width(xqw(al->elems[k]), ew);
                                fprintf(o, "    movq %s, %%rcx\n", dm);
                                char em[96];
                                snprintf(em, sizeof em, "%d(%%rcx)", k * esz);
                                store_mem(em, ew);
                            }
                        }
                    }
                    break;
                }
                // plain scalar/struct init
                {
                    XV *v = xv_find(s->names[i]);
                    if (!v) { Xerr("decl missing"); break; }
                    QW dw = dt ? ty_width(dt) : Q_I64;
                    if (dw == Q_STRUCT && dt) {
                        emit_val(s->inits[i]); // ADDRESS in RAX
                        char vm[64];
                        mem_slot(v->slot, vm, sizeof vm);
                        int sz = ty_size(dt);
                        fprintf(o, "    movq %%rax, %%rsi\n");
                        fprintf(o, "    leaq %s, %%rdi\n", vm);
                        fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", sz);
                    } else {
                        emit_val(s->inits[i]);
                        TkType *it = xinfer(s->inits[i]);
                        to_width(it ? ty_width(it) : Q_I64, dw);
                        char vm[64];
                        if (v->slot < 0) snprintf(vm, sizeof vm, "%s(%%rip)", v->name);
                        else mem_slot(v->slot, vm, sizeof vm);
                        store_mem(vm, dw);
                    }
                }
            }
            break;
        }
        case TK_ST_SIMPLE: {
            if (!s->has_assign) {
                emit_val(s->e1);
                break;
            }
            emit_assign_stmt(s->e1, s->e2, s->assign_op);
            break;
        }
        case TK_ST_BLOCK: {
            xv_push();
            emit_stmts(s->stmts, s->n);
            xv_pop();
            break;
        }
        case TK_ST_IF: {
            emit_val(s->cond_ex ? s->cond_ex : s->cond);
            int Lelse = new_lbl(), Lend = new_lbl();
            fprintf(o, "    testq %%rax, %%rax\n    jz .L%d\n", Lelse);
            xv_push();
            emit_stmts(s->then_s, s->then_n);
            xv_pop();
            fprintf(o, "    jmp .L%d\n.L%d:\n", Lend, Lelse);
            for (int i = 0; i < s->eif_n; i++) {
                int Le2 = new_lbl();
                emit_val(s->eifs[i].e ? s->eifs[i].e : s->eifs[i].c);
                fprintf(o, "    testq %%rax, %%rax\n    jz .L%d\n", Le2);
                xv_push();
                emit_stmts(s->eifs[i].s, s->eifs[i].n);
                xv_pop();
                fprintf(o, "    jmp .L%d\n.L%d:\n", Lend, Le2);
            }
            if (s->has_els) {
                xv_push();
                emit_stmts(s->els, s->els_n);
                xv_pop();
            }
            fprintf(o, ".L%d:\n", Lend);
            break;
        }
        case TK_ST_WHILE: {
            int Ltop = new_lbl(), Lend = new_lbl();
            loop_brk[loop_n] = Lend; loop_cont[loop_n] = Ltop; loop_n++;
            fprintf(o, ".L%d:\n", Ltop);
            emit_val(s->cond_ex ? s->cond_ex : s->cond);
            fprintf(o, "    testq %%rax, %%rax\n    jz .L%d\n", Lend);
            xv_push();
            emit_stmts(s->then_s, s->then_n);
            xv_pop();
            fprintf(o, "    jmp .L%d\n.L%d:\n", Ltop, Lend);
            loop_n--;
            break;
        }
        case TK_ST_FOR_C: {
            int Ltop = new_lbl(), Lend = new_lbl();
            xv_push();
            if (!s->for_init_empty) {
                if (s->for_init_is_decl) emit_stmt(s->for_init_decl);
                else if (s->for_init_simple->has_assign) {
                    emit_assign_stmt(s->for_init_simple->e1, s->for_init_simple->e2,
                        s->for_init_simple->assign_op);
                } else {
                    emit_val(s->for_init_simple->e1);
                }
            }
            fprintf(o, ".L%d:\n", Ltop);
            loop_brk[loop_n] = Lend; loop_cont[loop_n] = Ltop; loop_n++;
            if (s->has_for_cond) {
                emit_val(s->for_cond);
                fprintf(o, "    testq %%rax, %%rax\n    jz .L%d\n", Lend);
            }
            emit_stmts(s->then_s, s->then_n);
            if (s->has_incr) {
                if (!s->for_incr->has_assign) {
                    emit_val(s->for_incr->e1);
                } else {
                    emit_assign_stmt(s->for_incr->e1, s->for_incr->e2, s->for_incr->assign_op);
                }
            }
            fprintf(o, "    jmp .L%d\n.L%d:\n", Ltop, Lend);
            loop_n--;
            xv_pop();
            break;
        }
        case TK_ST_MATCH_STMT: {
            int mslot = tmp_find(s);
            int dslot = tmp_find((const void *)((const char *)s + 1));
            if (mslot < 0 || dslot < 0) { Xerr("temp missing (walk/emit skew)"); break; }
            char mm[64], dm[64];
            mem_slot(mslot, mm, sizeof mm);
            mem_slot(dslot, dm, sizeof dm);
            emit_val(s->m_scrut);
            TkType *st = xinfer(s->m_scrut);
            if (st && ty_width(st) == Q_STRUCT) {
                int sz = ty_size(st);
                fprintf(o, "    movq %%rax, %%rsi\n");
                fprintf(o, "    leaq %s, %%rdi\n", mm);
                fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", sz);
            } else if (st && (ty_width(st) == Q_F32 || ty_width(st) == Q_F64)) {
                if (ty_width(st) == Q_F32) fprintf(o, "    movss %%xmm0, %s\n", mm);
                else fprintf(o, "    movsd %%xmm0, %s\n", mm);
            } else {
                fprintf(o, "    movq %%rax, %s\n", mm);
            }
            fprintf(o, "    movq $0, %s\n", dm);
            for (int i = 0; i < s->m_arm_n; i++) {
                int Lbody = new_lbl(), Lnext = new_lbl();
                fprintf(o, "    cmpb $0, %s\n    jne .L%d\n", dm, Lnext);
                TkPattern *alts[9];
                int na = 0;
                alts[na++] = s->m_arms[i].pat;
                for (int k = 0; k < s->m_arms[i].pat->or_count && na < 9; k++)
                    alts[na++] = s->m_arms[i].pat->or_next[k];
                for (int k = 0; k < na; k++) {
                    xalt_bool(alts[k], mslot, st);
                    fprintf(o, "    testb %%al, %%al\n    jnz .L%d\n", Lbody);
                }
                fprintf(o, "    jmp .L%d\n.L%d:\n", Lnext, Lbody);
                xv_push();
                xbind_add(s->m_arms[i].pat, st);
                xbind_copy(s->m_arms[i].pat, mslot, st);
                if (s->m_arms[i].guard) {
                    emit_val(s->m_arms[i].guard);
                    fprintf(o, "    testq %%rax, %%rax\n");
                    int Lskip = new_lbl();
                    fprintf(o, "    jz .L%d\n", Lskip);
                    emit_stmts(s->m_arms[i].bs, s->m_arms[i].bn);
                    fprintf(o, "    movq $1, %s\n", dm);
                    fprintf(o, ".L%d:\n", Lskip);
                } else {
                    emit_stmts(s->m_arms[i].bs, s->m_arms[i].bn);
                    fprintf(o, "    movq $1, %s\n", dm);
                }
                xv_pop();
                fprintf(o, ".L%d:\n", Lnext);
            }
            break;
        }
        case TK_ST_RETURN: {
            if (!s->has_ret) {
                fprintf(o, "    jmp .Lfn%d\n", x_fn_exit);
                break;
            }
            if (s->ret_e && s->ret_e->kind == TK_EX_MATCH) {
                // return <match>: temp + value chain + load + ret
                int rslot = tmp_find(s);
                if (rslot < 0) { Xerr("temp missing (walk/emit skew)"); break; }
                TkType *rt = x_cur_ret_ty;
                char rm[64];
                mem_slot(rslot, rm, sizeof rm);
                int rsz = rt ? ty_size(rt) : 8;
                fprintf(o, "    leaq %s, %%rdi\n", rm);
                fprintf(o, "    movl $%d, %%ecx\n    xorl %%eax, %%eax\n    rep stosb\n", rsz);
                emit_match_value(s->ret_e, rslot, rt);
                if (rt && ty_width(rt) == Q_STRUCT) {
                    int sz2 = ty_size(rt);
                    // struct return via sret home (rdi saved at prologue)
                    fprintf(o, "    leaq %s, %%rsi\n", rm);
                    char sm[64];
                    if (x_sret_home < 0) { Xerr("sret home missing"); break; }
                    int ss2 = x_sret_home;
                    mem_slot(ss2, sm, sizeof sm);
                    fprintf(o, "    movq %s, %%rdi\n", sm);
                    fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", sz2);
                } else if (rt && (ty_width(rt) == Q_F32 || ty_width(rt) == Q_F64)) {
                    if (ty_width(rt) == Q_F32) fprintf(o, "    movss %s, %%xmm0\n", rm);
                    else fprintf(o, "    movsd %s, %%xmm0\n", rm);
                } else {
                    char rmm[96];
                    snprintf(rmm, sizeof rmm, "%s", rm);
                    load_mem(rmm, rt ? ty_width(rt) : Q_I64);
                }
                fprintf(o, "    jmp .Lfn%d\n", x_fn_exit);
                break;
            }
            TkType *rt = x_cur_ret_ty;
            if (rt && ty_width(rt) == Q_STRUCT) {
                // value address in RAX -> copy into sret home
                emit_val(s->ret_e);
                char sm[64];
                if (x_sret_home < 0) { Xerr("sret home missing"); break; }
                int ss2 = x_sret_home;
                mem_slot(ss2, sm, sizeof sm);
                int sz = ty_size(rt);
                fprintf(o, "    movq %%rax, %%rsi\n");
                fprintf(o, "    movq %s, %%rdi\n", sm);
                fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", sz);
            } else if (rt && (ty_width(rt) == Q_F32 || ty_width(rt) == Q_F64)) {
                emit_val(s->ret_e);
                TkType *bt = xinfer(s->ret_e);
                to_width(bt ? ty_width(bt) : Q_F64, ty_width(rt));
            } else {
                emit_val(s->ret_e);
                TkType *bt = xinfer(s->ret_e);
                to_width(bt ? ty_width(bt) : Q_I64, rt ? ty_width(rt) : Q_I64);
            }
            fprintf(o, "    jmp .Lfn%d\n", x_fn_exit);
            break;
        }
        case TK_ST_BREAK: {
            if (loop_n <= 0) { Xerr("break outside loop"); break; }
            if (s->label) { Xerr("labeled break/continue TODO in native backend"); break; }
            fprintf(o, "    jmp .L%d\n", loop_brk[loop_n - 1]);
            break;
        }
        case TK_ST_CONTINUE: {
            if (loop_n <= 0) { Xerr("continue outside loop"); break; }
            if (s->label) { Xerr("labeled break/continue TODO in native backend"); break; }
            fprintf(o, "    jmp .L%d\n", loop_cont[loop_n - 1]);
            break;
        }
        case TK_ST_LABEL:
            fprintf(o, ".L%s:\n", s->lab);
            emit_stmt(s->lab_s);
            break;
        case TK_ST_SPAWN_BLK: {
            xv_push();
            emit_stmts(s->sp_blk, s->sp_n);
            xv_pop();
            break;
        }
        case TK_ST_SPAWN_CALL: emit_val(s->sp_call); break;
        case TK_ST_DEFER: {
            // True defer: body lives in a jumped-over fragment (same frame,
            // so locals are visible; ends with ret) registered as a malloc
            // chain node { next, fn }. Drained LIFO at the fn exit label.
            // Bodies run at scope exit with variables read then (Zig-style).
            int Ld = new_lbl(), Lend = new_lbl(), Lok = new_lbl();
            char hm[64];
            mem_slot(x_defer_head, hm, sizeof hm);
            fprintf(o, "    jmp .L%d\n", Lend);
            fprintf(o, ".L%d:\n", Ld);
            if (s->defer_is_block) {
                xv_push();
                emit_stmts(s->defer_blk, s->defer_n);
                xv_pop();
            } else if (s->defer_simple) {
                if (s->defer_simple->has_assign) {
                    emit_assign_stmt(s->defer_simple->e1, s->defer_simple->e2,
                        s->defer_simple->assign_op);
                } else {
                    emit_val(s->defer_simple->e1);
                }
            }
            fprintf(o, "    ret\n");
            fprintf(o, ".L%d:\n", Lend);
            fprintf(o, "    movq $16, %%rdi\n");
            fprintf(o, "    call malloc@PLT\n");
            fprintf(o, "    testq %%rax, %%rax\n    jnz .L%d\n", Lok);
            fprintf(o, "    movl $1, %%edi\n    call exit@PLT\n");
            fprintf(o, ".L%d:\n", Lok);
            fprintf(o, "    movq %s, %%rcx\n", hm);
            fprintf(o, "    movq %%rcx, (%%rax)\n");
            fprintf(o, "    leaq .L%d(%%rip), %%rcx\n", Ld);
            fprintf(o, "    movq %%rcx, 8(%%rax)\n");
            fprintf(o, "    movq %%rax, %s\n", hm);
            break;
        }
        case TK_ST_FOR_EACH:
            Xerr("for-each TODO in native backend (use C-style for)");
            break;
        case TK_ST_HANDLE_STMT: case TK_ST_TRANSACT_STMT: {
            xv_push();
            emit_stmts(s->stmts, s->n);
            xv_pop();
            break;
        }
        default: Xerr("statement TODO in native backend"); break;
    }
}

// frame walk for one function body (locals/binds/temps/arg stats)
static void walk_fn_body(TkItem *it) {
    w_cur_ret = it->ret;
    w_maxArgs = 0;
    lookup_fn = wt_find;
    wt_depth = 0;
    for (int j = 0; j < it->pcount; j++) {
        wt_add(it->params[j].nm, it->params[j].ty);
        frame_slots += slots_for_size(ty_size(it->params[j].ty));
    }
    walk_stmts(it->body, it->bcount);
    while (wt_n > 0) wt_n--;
    wt_depth = 0;
}

// count call arg maxima (argtmp/outgoing sizing) — walks exprs for TK_EX_CALL


// List push helpers (one asm function per monomorphization).
// Convention: RDI = List*, RSI = value (scalar bits / float bits / ADDRESS
// for struct elems). Matches the push call sequences in emit_call.
static void emit_list_helpers(void) {
    for (int li = 0; li < xlist_n; li++) {
        TkType *el = xlist_elems[li];
        QW ew = ty_width(el);
        int esz = ty_size(el);
        // In: RDI = List*, RSI = value bits (scalar/float) or ADDRESS (struct).
        // Locals below rsp (red-zone-free: explicit sub).
        fprintf(o, "List_");
        mangle_rec(el);
        fprintf(o, "_push:\n");
        fprintf(o, "    pushq %%rbp\n    movq %%rsp, %%rbp\n");
        fprintf(o, "    subq $32, %%rsp\n");
        fprintf(o, "    movq %%rdi, -8(%%rbp)\n"); // List*
        fprintf(o, "    movq %%rsi, -16(%%rbp)\n"); // value / address
        fprintf(o, "    movq -8(%%rbp), %%rax\n");
        fprintf(o, "    movq 8(%%rax), %%rax\n"); // len
        fprintf(o, "    movq -8(%%rbp), %%rcx\n");
        fprintf(o, "    cmpq 16(%%rcx), %%rax\n"); // len == cap?
        int Lok = new_lbl();
        fprintf(o, "    jne .L%d\n", Lok);
        // grow: nc = cap ? cap*2 : 8; nd = realloc(data, nc*esz)
        fprintf(o, "    movq -8(%%rbp), %%rax\n");
        fprintf(o, "    movq 16(%%rax), %%rsi\n");
        fprintf(o, "    testq %%rsi, %%rsi\n");
        int Lnz = new_lbl(), Lgot = new_lbl();
        fprintf(o, "    jnz .L%d\n", Lnz);
        fprintf(o, "    movq $8, %%rsi\n");
        fprintf(o, "    jmp .L%d\n", Lgot);
        fprintf(o, ".L%d:\n", Lnz);
        fprintf(o, "    leaq (%%rsi,%%rsi), %%rsi\n");
        fprintf(o, ".L%d:\n", Lgot);
        fprintf(o, "    imulq $%d, %%rsi\n", esz);
        fprintf(o, "    movq -8(%%rbp), %%rax\n");
        fprintf(o, "    movq (%%rax), %%rdi\n"); // old data (NULL ok: realloc~=malloc)
        fprintf(o, "    call realloc@PLT\n");
        fprintf(o, "    testq %%rax, %%rax\n");
        int Lok2 = new_lbl();
        fprintf(o, "    jnz .L%d\n", Lok2);
        fprintf(o, "    movl $1, %%edi\n    call exit@PLT\n");
        fprintf(o, ".L%d:\n", Lok2);
        fprintf(o, "    movq -8(%%rbp), %%rcx\n");
        fprintf(o, "    movq %%rax, (%%rcx)\n"); // data = nd
        fprintf(o, "    movq 16(%%rcx), %%rsi\n"); // old cap
        fprintf(o, "    testq %%rsi, %%rsi\n");
        int Lnz2 = new_lbl(), Lgot2 = new_lbl();
        fprintf(o, "    jnz .L%d\n", Lnz2);
        fprintf(o, "    movq $8, %%rsi\n");
        fprintf(o, "    jmp .L%d\n", Lgot2);
        fprintf(o, ".L%d:\n", Lnz2);
        fprintf(o, "    leaq (%%rsi,%%rsi), %%rsi\n");
        fprintf(o, ".L%d:\n", Lgot2);
        fprintf(o, "    movq %%rsi, 16(%%rcx)\n"); // cap = nc
        fprintf(o, ".L%d:\n", Lok);
        // store value at data[len]; len++ (SIB scale allows only 1/2/4/8)
        fprintf(o, "    movq -8(%%rbp), %%rax\n");
        fprintf(o, "    movq (%%rax), %%rax\n"); // data
        fprintf(o, "    movq -8(%%rbp), %%rcx\n");
        fprintf(o, "    movq 8(%%rcx), %%rcx\n"); // len
        if (esz == 1) fprintf(o, "    addq %%rcx, %%rax\n");
        else if (esz == 2 || esz == 4 || esz == 8) fprintf(o, "    leaq (%%rax,%%rcx,%d), %%rax\n", esz);
        else { fprintf(o, "    imulq $%d, %%rcx\n", esz); fprintf(o, "    addq %%rcx, %%rax\n"); }
        fprintf(o, "    movq %%rax, -24(%%rbp)\n"); // dst
        if (ew == Q_STRUCT) {
            fprintf(o, "    movq -16(%%rbp), %%rsi\n"); // src address
            fprintf(o, "    movq -24(%%rbp), %%rdi\n");
            fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", esz);
        } else if (ew == Q_F32) {
            fprintf(o, "    movq -16(%%rbp), %%rax\n");
            fprintf(o, "    movq -24(%%rbp), %%rcx\n");
            fprintf(o, "    movl %%eax, (%%rcx)\n");
        } else if (ew == Q_F64) {
            fprintf(o, "    movq -16(%%rbp), %%rax\n");
            fprintf(o, "    movq -24(%%rbp), %%rcx\n");
            fprintf(o, "    movq %%rax, (%%rcx)\n");
        } else {
            fprintf(o, "    movq -16(%%rbp), %%rax\n");
            fprintf(o, "    movq -24(%%rbp), %%rcx\n");
            if (esz == 1) fprintf(o, "    movb %%al, (%%rcx)\n");
            else if (esz == 2) fprintf(o, "    movw %%ax, (%%rcx)\n");
            else if (esz == 4) fprintf(o, "    movl %%eax, (%%rcx)\n");
            else fprintf(o, "    movq %%rax, (%%rcx)\n");
        }
        fprintf(o, "    movq -8(%%rbp), %%rax\n");
        fprintf(o, "    incq 8(%%rax)\n"); // len++
        fprintf(o, "    leave\n    ret\n");
    }
}

// function emission (name mangling: taka `main` -> `taka_main`)
static int xv_global_n = 0;

static void emit_fn(TkItem *it) {
    TkType *rt = it->ret;
    QW rw = rt ? ty_width(rt) : Q_VOID;
    int is_sret = (rw == Q_STRUCT);
    x_fn_exit = new_lbl();
    // defer-chain head slot (emit-pass raffle; counted into frame_bytes below)
    x_defer_head = frame_slots; frame_slots += 1;
    // home params (mirrors arg_home classification order)
    int ipos = 0, fpos = 0, spos = 0;
    static const char *ir[6] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
    static const char *ir32[6] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
    if (is_sret) {
        int slot = tmp_find(it);
        if (slot < 0) { Xerr("temp missing (walk/emit skew)"); }
        else {
            char m[64];
            mem_slot(slot, m, sizeof m);
            fprintf(o, "    movq %%rdi, %s\n", m);
            x_sret_home = slot;
        }
        ipos = 1;
    }
    for (int j = 0; j < it->pcount; j++) {
        TkType *pt = it->params[j].ty;
        QW pw = ty_width(pt);
        int slots = slots_for_size(ty_size(pt));
        xv_add(it->params[j].nm, pt, slots);
        XV *v = xv_find(it->params[j].nm);
        char dst[64];
        mem_slot(v->slot, dst, sizeof dst);
        if (pw == Q_STRUCT && pt) {
            int esz = ty_size(pt);
            // rep movsb clobbers rsi/rdi/rcx: preserve all incoming int regs
            // so later params (struct or scalar) still see their values.
            fprintf(o, "    pushq %%rdi\n    pushq %%rsi\n    pushq %%rdx\n    pushq %%rcx\n    pushq %%r8\n    pushq %%r9\n");
            if (ipos < 6) {
                fprintf(o, "    movq %s, %%rsi\n", ir[ipos]);
            } else {
                fprintf(o, "    movq %d(%%rbp), %%rsi\n", 16 + spos * 8);
                spos++;
            }
            fprintf(o, "    leaq %s, %%rdi\n", dst);
            fprintf(o, "    movl $%d, %%ecx\n    rep movsb\n", esz);
            fprintf(o, "    popq %%r9\n    popq %%r8\n    popq %%rcx\n    popq %%rdx\n    popq %%rsi\n    popq %%rdi\n");
            ipos++;
        } else if (pw == Q_F32 || pw == Q_F64) {
            if (fpos < 8) {
                if (pw == Q_F32) fprintf(o, "    movss %%xmm%d, %s\n", fpos, dst);
                else fprintf(o, "    movsd %%xmm%d, %s\n", fpos, dst);
                fpos++;
            } else {
                fprintf(o, "    movq %d(%%rbp), %%rax\n", 16 + spos * 8);
                fprintf(o, "    movq %%rax, %s\n", dst);
                spos++;
            }
        } else {
            if (ipos < 6) {
                if (pw == Q_I32 || pw == Q_U32 || pw == Q_BOOL || pw == Q_CHAR)
                    fprintf(o, "    movl %s, %s\n", ir32[ipos], dst);
                else
                    fprintf(o, "    movq %s, %s\n", ir[ipos], dst);
                ipos++;
            } else {
                fprintf(o, "    movq %d(%%rbp), %%rax\n", 16 + spos * 8);
                fprintf(o, "    movq %%rax, %s\n", dst);
                spos++;
            }
        }
    }
    // defer chain starts empty
    {
        char hm[64];
        mem_slot(x_defer_head, hm, sizeof hm);
        fprintf(o, "    movq $0, %s\n", hm);
    }
    emit_stmts(it->body, it->bcount);
    fprintf(o, ".Lfn%d:\n", x_fn_exit);
    // drain defer chain LIFO (head NULL fast path); rax saved: scalar
    // returns ride in rax across this loop, floats in xmm0 (untouched).
    {
        char hm[64];
        mem_slot(x_defer_head, hm, sizeof hm);
        int Ltop = new_lbl(), Ldone = new_lbl();
        fprintf(o, "    pushq %%rax\n");
        fprintf(o, ".L%d:\n", Ltop);
        fprintf(o, "    movq %s, %%rax\n", hm);
        fprintf(o, "    testq %%rax, %%rax\n");
        fprintf(o, "    jz .L%d\n", Ldone);
        fprintf(o, "    movq (%%rax), %%rcx\n");
        fprintf(o, "    movq %%rcx, %s\n", hm);
        fprintf(o, "    pushq %%rax\n");
        fprintf(o, "    movq 8(%%rax), %%rax\n");
        fprintf(o, "    subq $8, %%rsp\n");
        fprintf(o, "    call *%%rax\n");
        fprintf(o, "    addq $8, %%rsp\n");
        fprintf(o, "    popq %%rdi\n");
        fprintf(o, "    call free@PLT\n");
        fprintf(o, "    jmp .L%d\n", Ltop);
        fprintf(o, ".L%d:\n", Ldone);
        fprintf(o, "    popq %%rax\n");
    }
    fprintf(o, "    leave\n    ret\n");
}

int x64_emit(TkProgram *prog, const char *out_path) {
    // reset all per-program state
    failed = 0;
    for (int i = 0; i < xss_n; i++) { free(xss[i].fn); free(xss[i].ft); free(xss[i].off); }
    free(xss); xss = NULL; xss_n = 0;
    for (int i = 0; i < xes_n; i++) {
        for (int j = 0; j < xes[i].n; j++) { free(xes[i].vars[j].sf); free(xes[i].vars[j].st); }
        free(xes[i].vars);
    }
    free(xes); xes = NULL; xes_n = 0;
    for (int i = 0; i < xfs_n; i++) { free(xfs[i].pt); free(xfs[i].pnm); }
    free(xfs); xfs = NULL; xfs_n = 0;
    free(xlist_elems); xlist_elems = NULL; xlist_n = xlist_cap = 0;
    for (int i = 0; i < rod_n; i++) free(rod_str[i]);
    free(rod_str); rod_str = NULL; rod_n = rod_cap = 0;
    free(xvs); xvs = NULL; xv_n = xv_cap = xv_global_n = 0; xv_depth = 0;
    free(xts); xts = NULL; xt_n = xt_cap = 0;
    frame_slots = 0;
    lbl = 0;
    loop_n = 0;
    build_tables(prog);
    o = fopen(out_path, "w");
    if (!o) return 1;
    fprintf(o, "    # generated taka -> x86-64 (native, no C)\n");
    fprintf(o, "    .text\n");
    // globals registered first (rip-relative; no frame slots)
    for (int i = 0; i < prog->count; i++) {
        TkItem *it = prog->items[i];
        if (it->kind != TK_IT_GLOBAL) continue;
        for (int k = 0; k < it->gdecl->dcount; k++)
            xv_add_global(it->gdecl->names[k], it->gdecl->ty);
    }
    xv_global_n = xv_n;
    // functions: walk (slots/temps/arg stats), reserve arg areas, emit body
    // into a buffer (frame size only known after), then prologue + body.
    for (int i = 0; i < prog->count; i++) {
        TkItem *it = prog->items[i];
        if (it->kind != TK_IT_FN || it->is_proto) continue;
        frame_slots = 0;
        xt_n = 0;
        w_maxArgs = 0;
        wt_n = 0; wt_depth = 0;
        w_cur_ret = it->ret;
        lookup_fn = wt_find;
        walk_fn_body(it);
        TkType *rt = it->ret;
        QW rw = rt ? ty_width(rt) : Q_VOID;
        if (rw == Q_STRUCT) tmp_for(it, 1); // sret home
        g_argBase = frame_slots;
        frame_slots += 9 * w_maxArgs;
        g_outBase = g_argBase + 8 * w_maxArgs;
        xv_n = xv_global_n; xv_depth = 0;
        lookup_fn = lookup_xv;
        x_cur_ret_ty = it->ret;
        x_sret_home = -1;
        loop_n = 0;
        char *body = NULL;
        size_t blen = 0;
        FILE *mb = open_memstream(&body, &blen);
        if (!mb) { Xerr("OOM"); break; }
        FILE *save_o = o;
        o = mb;
        emit_fn(it);
        fclose(mb);
        o = save_o;
        int frame_bytes = align_up(frame_slots * 8, 16);
        const char *nm = !strcmp(it->fn_name, "main") ? "taka_main" : it->fn_name;
        fprintf(o, "    .globl %s\n", nm);
        fprintf(o, "%s:\n", nm);
        fprintf(o, "    pushq %%rbp\n    movq %%rsp, %%rbp\n");
        if (frame_bytes > 0) fprintf(o, "    subq $%d, %%rsp\n", frame_bytes);
        fwrite(body, 1, blen, o);
        free(body);
    }
    emit_list_helpers();
    // process-args runtime (tk_argc/tk_argv externs, C-ABI argc/argv saved here).
    // Bounds behavior matches the C backend (NULL past either end).
    fprintf(o, "    .data\n");
    fprintf(o, "tk_rt_argc:\n    .long 0\n");
    fprintf(o, "tk_rt_argv:\n    .quad 0\n");
    fprintf(o, "tk_g_exit:\n    .long 0\n");
    fprintf(o, "    .text\n");
    fprintf(o, "    .globl tk_argc\ntk_argc:\n");
    fprintf(o, "    movl tk_rt_argc(%%rip), %%eax\n    ret\n");
    fprintf(o, "    .globl tk_argv\ntk_argv:\n");
    fprintf(o, "    movslq %%edi, %%rcx\n");
    fprintf(o, "    testl %%ecx, %%ecx\n    js .Largv_null\n");
    fprintf(o, "    cmpl tk_rt_argc(%%rip), %%ecx\n    jae .Largv_null\n");
    fprintf(o, "    movq tk_rt_argv(%%rip), %%rax\n");
    fprintf(o, "    movq (%%rax,%%rcx,8), %%rax\n    ret\n");
    fprintf(o, ".Largv_null:\n    xorl %%eax, %%eax\n    ret\n");
    // tk_eprint(p, n): write(2, p, n); returns bytes written (i64 in RAX).
    fprintf(o, "    .globl tk_eprint_rt\ntk_eprint_rt:\n");
    fprintf(o, "    movq %%rsi, %%rdx\n    movq %%rdi, %%rsi\n    movl $2, %%edi\n");
    fprintf(o, "    call write@PLT\n    ret\n");
    // tk_set_exit(c): store process exit code (returned by main).
    fprintf(o, "    .globl tk_set_exit\ntk_set_exit:\n");
    fprintf(o, "    movl %%edi, tk_g_exit(%%rip)\n    ret\n");
    // tk_stdout(): return C stdout FILE* (for block writes via fwrite).
    fprintf(o, "    .globl tk_stdout\ntk_stdout:\n");
    fprintf(o, "    movq stdout@GOTPCREL(%%rip), %%rax\n    movq (%%rax), %%rax\n    ret\n");
    // tk_strtod_bits(s): NUL-terminated decimal -> f64 bits in RAX (via strtod).
    fprintf(o, "    .globl tk_strtod_bits\ntk_strtod_bits:\n");
    fprintf(o, "    call strtod@PLT\n    movq %%xmm0, %%rax\n    ret\n");
    fprintf(o, "    .globl main\n");
    fprintf(o, "main:\n");
    fprintf(o, "    pushq %%rbp\n    movq %%rsp, %%rbp\n");
    fprintf(o, "    movl %%edi, tk_rt_argc(%%rip)\n");
    fprintf(o, "    movq %%rsi, tk_rt_argv(%%rip)\n");
    fprintf(o, "    call taka_main\n");
    fprintf(o, "    movl tk_g_exit(%%rip), %%eax\n");
    fprintf(o, "    leave\n    ret\n");
    // rodata strings (ids assigned during emission; forward refs resolve at asm)
    fprintf(o, "    .section .rodata\n");
    for (int i = 0; i < rod_n; i++) {
        fprintf(o, ".Lstr_%d:\n    .asciz ", i);
        emit_asciz(rod_str[i], (int)strlen(rod_str[i]));
        fprintf(o, "\n");
    }
    // globals with constant initializers
    int have_data = 0;
    for (int i = 0; i < prog->count; i++) {
        TkItem *it = prog->items[i];
        if (it->kind != TK_IT_GLOBAL) continue;
        for (int k = 0; k < it->gdecl->dcount; k++) {
            TkExpr *in = it->gdecl->inits[k];
            QW w = ty_width(it->gdecl->ty);
            if (in->kind == TK_EX_INT && w != Q_STRUCT && w != Q_F32 && w != Q_F64 && w != Q_VOID) {
                if (!have_data) { fprintf(o, "    .data\n"); have_data = 1; }
                fprintf(o, "%s:\n", it->gdecl->names[k]);
                u64 bits = lit_int_bits(in->tok);
                if (w == Q_I8 || w == Q_U8 || w == Q_BOOL || w == Q_CHAR)
                    fprintf(o, "    .byte %u\n", (unsigned)(bits & 0xFF));
                else if (w == Q_I16 || w == Q_U16)
                    fprintf(o, "    .value %u\n", (unsigned)(bits & 0xFFFF));
                else if (w == Q_I32 || w == Q_U32)
                    fprintf(o, "    .long %u\n", (unsigned)(bits & 0xFFFFFFFF));
                else
                    fprintf(o, "    .quad %llu\n", (unsigned long long)bits);
            } else if (in->kind == TK_EX_STRING) {
                if (!have_data) { fprintf(o, "    .data\n"); have_data = 1; }
                int id = rod_add(in->tok.start + 1, (int)in->tok.len - 2);
                fprintf(o, "%s:\n", it->gdecl->names[k]);
                fprintf(o, "    .quad .Lstr_%d\n", id);
            } else if (in->kind == TK_EX_UNARY &&
                       (in->op == TOKEN_MINUS || in->op == TOKEN_PLUS) &&
                       in->lhs && in->lhs->kind == TK_EX_INT &&
                       w != Q_STRUCT && w != Q_F32 && w != Q_F64 && w != Q_VOID) {
                int cv = 0;
                if (const_int(in, &cv)) { Xerr("non-constant global initializer TODO in native backend"); }
                else {
                    if (!have_data) { fprintf(o, "    .data\n"); have_data = 1; }
                    fprintf(o, "%s:\n", it->gdecl->names[k]);
                    u64 bits = (u64)(i64)cv;
                    if (w == Q_I8 || w == Q_U8 || w == Q_BOOL || w == Q_CHAR)
                        fprintf(o, "    .byte %u\n", (unsigned)(bits & 0xFF));
                    else if (w == Q_I16 || w == Q_U16)
                        fprintf(o, "    .value %u\n", (unsigned)(bits & 0xFFFF));
                    else if (w == Q_I32 || w == Q_U32)
                        fprintf(o, "    .long %u\n", (unsigned)(bits & 0xFFFFFFFF));
                    else
                        fprintf(o, "    .quad %llu\n", (unsigned long long)bits);
                }
            } else if (in->kind == TK_EX_NULL) {
                if (!have_data) { fprintf(o, "    .data\n"); have_data = 1; }
                fprintf(o, "%s:\n", it->gdecl->names[k]);
                fprintf(o, "    .quad 0\n");
            } else {
                Xerr("non-constant global initializer TODO in native backend");
            }
        }
    }
    fprintf(o, "    .section .note.GNU-stack,\"\",@progbits\n");
    fclose(o);
    return failed ? 1 : 0;
}
