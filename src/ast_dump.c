// Canonical S-expression dump of the core-builder AST (stage-2 gate).
// Format: ints for all kinds (must match C enum ordinals), quoted lexemes
// with \" \\\\ \\n escapes, 0 for nulls. One line per top-level item.
#include "ast.h"
#include "lexer.h"
#include <stdio.h>
#include <string.h>

static void esc(FILE *o, const char *s, u64 n) {
    fputc('"', o);
    for (u64 i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"') fputs("\\\"", o);
        else if (c == '\\') fputs("\\\\", o);
        else if (c == '\n') fputs("\\n", o);
        else fputc(c, o);
    }
    fputc('"', o);
}

static void dump_type(FILE *o, TkType *t);
static void dump_expr(FILE *o, TkExpr *e);
static void dump_stmt(FILE *o, TkStmt *s);
static void dump_pat(FILE *o, TkPattern *p);

static void dump_type(FILE *o, TkType *t) {
    if (!t) { fputs("0", o); return; }
    fprintf(o, "(ty ");
    switch (t->kind) {
        case TK_TY_PRIM: fprintf(o, "%d", (int)t->prim); break;
        case TK_TY_AUTO: fputs("auto", o); break;
        case TK_TY_PATH: {
            fputs("(path (", o);
            for (int i = 0; i < t->path.count; i++) {
                if (i) fputc(' ', o);
                esc(o, t->path.segs[i].seg, (u64)strlen(t->path.segs[i].seg));
            }
            fputs(") (", o);
            for (int i = 0; i < t->gcount; i++) {
                if (i) fputc(' ', o);
                dump_type(o, t->gargs[i]);
            }
            fputs("))", o);
            break;
        }
        case TK_TY_TUPLE:
            fputs("(tup", o);
            for (int i = 0; i < t->ecount; i++) { fputc(' ', o); dump_type(o, t->elems[i]); }
            fputc(')', o);
            break;
        case TK_TY_ARRAY:
            fputs("(arr ", o); dump_type(o, t->elem); fputc(' ', o); dump_expr(o, t->arr_n); fputc(')', o);
            break;
        case TK_TY_FN: fputs("(fn)", o); break;
        case TK_TY_DYN: fputs("(dyn)", o); break;
        default: fputs("?", o); break;
    }
    fprintf(o, " %d %d %d)", t->stars, t->is_ref ? 1 : 0, t->is_const ? 1 : 0);
}

static void dump_toktext(FILE *o, Token *t) {
    esc(o, t->start, t->len);
}

static void dump_expr(FILE *o, TkExpr *e) {
    if (!e) { fputs("0", o); return; }
    fprintf(o, "(%d", (int)e->kind);
    switch (e->kind) {
        case TK_EX_INT: case TK_EX_FLOAT: case TK_EX_STRING:
        case TK_EX_CHAR: case TK_EX_BOOL:
            fputc(' ', o); dump_toktext(o, &e->tok); break;
        case TK_EX_NULL: case TK_EX_UNDEF: break;
        case TK_EX_VAR: {
            fputs(" (", o);
            for (int i = 0; i < e->path.count; i++) {
                if (i) fputc(' ', o);
                esc(o, e->path.segs[i].seg, (u64)strlen(e->path.segs[i].seg));
            }
            fputs(") (", o);
            for (int i = 0; i < e->tcount; i++) {
                if (i) fputc(' ', o);
                dump_type(o, e->targs[i]);
            }
            fputs(")", o);
            break;
        }
        case TK_EX_PAREN: fputc(' ', o); dump_expr(o, e->lhs); break;
        case TK_EX_STRUCT_LIT: {
            fputs(" (", o);
            for (int i = 0; i < e->path.count; i++) {
                if (i) fputc(' ', o);
                esc(o, e->path.segs[i].seg, (u64)strlen(e->path.segs[i].seg));
            }
            fputs(") (", o);
            for (int i = 0; i < e->scount; i++) {
                if (i) fputc(' ', o);
                fputc('(', o); esc(o, e->sfields[i].field, (u64)strlen(e->sfields[i].field));
                fputc(' ', o); dump_expr(o, e->sfields[i].e); fputc(')', o);
            }
            fputs(")", o);
            break;
        }
        case TK_EX_ARRAY_LIST:
            for (int i = 0; i < e->ecount; i++) { fputc(' ', o); dump_expr(o, e->elems[i]); }
            break;
        case TK_EX_ARRAY_FILL:
            fputc(' ', o); dump_expr(o, e->fill_elem); fputc(' ', o); dump_expr(o, e->fill_n);
            break;
        case TK_EX_MATCH: {
            fputc(' ', o); dump_expr(o, e->scrut); fputs(" (", o);
            for (int i = 0; i < e->arm_n; i++) {
                if (i) fputc(' ', o);
                fputc('(', o); dump_pat(o, e->arms[i].pat); fputc(' ', o);
                dump_expr(o, e->arms[i].guard); fputc(' ', o);
                dump_expr(o, e->arms[i].body); fputc(')', o);
            }
            fputs(")", o);
            break;
        }
        case TK_EX_UNARY:
            fprintf(o, " %d ", (int)e->op); dump_expr(o, e->lhs); break;
        case TK_EX_BINARY:
            fprintf(o, " %d ", (int)e->op); dump_expr(o, e->lhs); fputc(' ', o); dump_expr(o, e->rhs); break;
        case TK_EX_CAST:
            fputc(' ', o); dump_expr(o, e->lhs); fputc(' ', o); dump_type(o, e->cast_to); break;
        case TK_EX_TERNARY:
            fputc(' ', o); dump_expr(o, e->cond); fputc(' ', o);
            dump_expr(o, e->then_e); fputc(' ', o); dump_expr(o, e->else_e); break;
        case TK_EX_CALL: {
            fputc(' ', o); dump_expr(o, e->callee); fputs(" (", o);
            for (int i = 0; i < e->nargs; i++) {
                if (i) fputc(' ', o);
                dump_expr(o, e->args[i]);
            }
            fputs(")", o);
            break;
        }
        case TK_EX_INDEX:
            fputc(' ', o); dump_expr(o, e->callee); fputc(' ', o); dump_expr(o, e->index); break;
        case TK_EX_FIELD:
            fputc(' ', o); dump_expr(o, e->callee); fputc(' ', o);
            esc(o, e->field, (u64)strlen(e->field)); break;
        case TK_EX_POST_INC: case TK_EX_POST_DEC:
            fputc(' ', o); dump_expr(o, e->lhs); break;
        case TK_EX_BLOCK: {
            fputs(" (", o);
            for (int i = 0; i < e->scount2; i++) {
                if (i) fputc(' ', o);
                dump_stmt(o, e->stmts[i]);
            }
            fputs(") ", o); dump_expr(o, e->trailing);
            break;
        }
        default: break; // unreached builder kinds: kind marker only
    }
    fputc(')', o);
}

static void dump_pat(FILE *o, TkPattern *p) {
    if (!p) { fputs("0", o); return; }
    fprintf(o, "(%d", (int)p->kind);
    switch (p->kind) {
        case TK_PAT_WILD: break;
        case TK_PAT_REF: fputc(' ', o); esc(o, p->name, (u64)strlen(p->name)); break;
        case TK_PAT_LIT: fputc(' ', o); fprintf(o, "%d ", (int)p->tok.kind); dump_toktext(o, &p->tok); break;
        case TK_PAT_BIND:
            fprintf(o, " %d ", p->is_mut ? 1 : 0);
            esc(o, p->name, (u64)strlen(p->name)); break;
        case TK_PAT_PATH: {
            fputs(" (", o);
            for (int i = 0; i < p->path.count; i++) {
                if (i) fputc(' ', o);
                esc(o, p->path.segs[i].seg, (u64)strlen(p->path.segs[i].seg));
            }
            fputs(") (", o);
            for (int i = 0; i < p->acount; i++) {
                if (i) fputc(' ', o);
                dump_pat(o, p->args[i]);
            }
            fputs(") (", o);
            for (int i = 0; i < p->fcount; i++) {
                if (i) fputc(' ', o);
                fputc('(', o); esc(o, p->fields[i].field, (u64)strlen(p->fields[i].field));
                fputc(' ', o); dump_pat(o, p->fields[i].pat); fputc(')', o);
            }
            fprintf(o, ") %d (", p->has_dotdot ? 1 : 0);
            for (int i = 0; i < p->or_count; i++) {
                if (i) fputc(' ', o);
                dump_pat(o, p->or_next[i]);
            }
            fputs(")", o);
            break;
        }
        default: break;
    }
    fputc(')', o);
}

static void dump_stmt(FILE *o, TkStmt *s) {
    if (!s) { fputs("0", o); return; }
    fprintf(o, "(%d", (int)s->kind);
    switch (s->kind) {
        case TK_ST_DECL: {
            fprintf(o, " %d ", s->is_mut ? 1 : 0);
            dump_type(o, s->ty);
            fprintf(o, " %d", s->dcount);
            for (int i = 0; i < s->dcount; i++) {
                fputs(" (", o);
                esc(o, s->names[i], (u64)strlen(s->names[i]));
                fputc(' ', o); dump_expr(o, s->inits[i]); fputc(')', o);
            }
            break;
        }
        case TK_ST_SIMPLE:
            fputc(' ', o); dump_expr(o, s->e1);
            fprintf(o, " %d ", s->has_assign ? (int)s->assign_op : 0);
            dump_expr(o, s->has_assign ? s->e2 : NULL);
            break;
        case TK_ST_BLOCK: {
            fprintf(o, " %d (", s->has_trailing ? 1 : 0);
            for (int i = 0; i < s->n; i++) { if (i) fputc(' ', o); dump_stmt(o, s->stmts[i]); }
            fputs(") ", o); dump_expr(o, s->trailing);
            break;
        }
        case TK_ST_MATCH_STMT: {
            fputc(' ', o); dump_expr(o, s->m_scrut); fputs(" (", o);
            for (int i = 0; i < s->m_arm_n; i++) {
                if (i) fputc(' ', o);
                fputc('(', o); dump_pat(o, s->m_arms[i].pat); fputc(' ', o);
                dump_expr(o, s->m_arms[i].guard); fputs(" (", o);
                for (int j = 0; j < s->m_arms[i].bn; j++) {
                    if (j) fputc(' ', o);
                    dump_stmt(o, s->m_arms[i].bs[j]);
                }
                fputs("))", o);
            }
            fputs(")", o);
            break;
        }
        case TK_ST_IF: {
            int k;
            fputc(' ', o); dump_expr(o, s->cond); fputc(' ', o); dump_expr(o, s->cond_ex);
            fputs(" (", o);
            for (k = 0; k < s->then_n; k++) { if (k) fputc(' ', o); dump_stmt(o, s->then_s[k]); }
            fputs(") ", o); dump_expr(o, s->then_t); fputs(" (", o);
            for (k = 0; k < s->eif_n; k++) {
                if (k) fputc(' ', o);
                fputc('(', o); dump_expr(o, s->eifs[k].c); fputc(' ', o);
                dump_expr(o, s->eifs[k].e); fputs(" (", o);
                for (int j = 0; j < s->eifs[k].n; j++) {
                    if (j) fputc(' ', o);
                    dump_stmt(o, s->eifs[k].s[j]);
                }
                fputs(") ", o); dump_expr(o, s->eifs[k].t); fputc(')', o);
            }
            fputs(") ", o);
            if (s->has_els) {
                fputs("(1 (", o);
                for (k = 0; k < s->els_n; k++) { if (k) fputc(' ', o); dump_stmt(o, s->els[k]); }
                fputs(") ", o); dump_expr(o, s->els_t); fputc(')', o);
            } else fputs("0", o);
            break;
        }
        case TK_ST_WHILE: {
            int k;
            fputc(' ', o); dump_expr(o, s->cond); fputc(' ', o); dump_expr(o, s->cond_ex);
            fputs(" (", o);
            for (k = 0; k < s->then_n; k++) { if (k) fputc(' ', o); dump_stmt(o, s->then_s[k]); }
            fputs(") ", o); dump_expr(o, s->then_t);
            break;
        }
        case TK_ST_FOR_C: {
            int k;
            fputs(" ", o);
            if (s->for_init_is_decl) { fputs("(d ", o); dump_stmt(o, s->for_init_decl); fputc(')', o); }
            else if (s->for_init_simple) { fputs("(s ", o); dump_stmt(o, s->for_init_simple); fputc(')', o); }
            else fputs("0", o);
            fputc(' ', o); dump_expr(o, s->has_for_cond ? s->for_cond : NULL);
            fputc(' ', o); dump_stmt(o, s->has_incr ? s->for_incr : NULL);
            fputs(" (", o);
            for (k = 0; k < s->then_n; k++) { if (k) fputc(' ', o); dump_stmt(o, s->then_s[k]); }
            fputs(") ", o); dump_expr(o, s->then_t);
            break;
        }
        case TK_ST_RETURN:
            fputc(' ', o); dump_expr(o, s->has_ret ? s->ret_e : NULL);
            fprintf(o, " %d", s->has_ret ? 1 : 0);
            break;
        case TK_ST_DEFER: {
            fprintf(o, " %d (", s->defer_is_block ? 1 : 0);
            if (s->defer_is_block) {
                for (int k = 0; k < s->defer_n; k++) { if (k) fputc(' ', o); dump_stmt(o, s->defer_blk[k]); }
            }
            fputs(") ", o);
            if (s->defer_simple) dump_stmt(o, s->defer_simple);
            else fputs("0", o);
            break;
        }
        case TK_ST_BREAK: case TK_ST_CONTINUE:
            if (s->label) { fputc(' ', o); esc(o, s->label, (u64)strlen(s->label)); }
            else fputs(" 0", o);
            break;
        default: break; // unreached builder kinds
    }
    fputc(')', o);
}

static void dump_item(FILE *o, TkItem *it) {
    int i, j;
    fprintf(o, "(%d", (int)it->kind);
    switch (it->kind) {
        case TK_IT_FN: {
            fputc(' ', o); esc(o, it->fn_name, (u64)strlen(it->fn_name)); fputs(" (", o);
            for (i = 0; i < it->pcount; i++) {
                if (i) fputc(' ', o);
                fprintf(o, "(%d ", it->params[i].is_mut ? 1 : 0);
                dump_type(o, it->params[i].ty);
                fputc(' ', o); esc(o, it->params[i].nm, (u64)strlen(it->params[i].nm)); fputc(')', o);
            }
            fputs(") ", o); dump_type(o, it->ret);
            fprintf(o, " %d (", it->is_proto ? 1 : 0);
            for (i = 0; i < it->bcount; i++) { if (i) fputc(' ', o); dump_stmt(o, it->body[i]); }
            fputs(") ", o); dump_expr(o, it->trail);
            break;
        }
        case TK_IT_EXTERN: {
            fputs(" 0 ", o); esc(o, it->fn_name, (u64)strlen(it->fn_name)); fputs(" (", o);
            for (i = 0; i < it->pcount; i++) {
                if (i) fputc(' ', o);
                fprintf(o, "(%d ", it->params[i].is_mut ? 1 : 0);
                dump_type(o, it->params[i].ty);
                fputc(' ', o); esc(o, it->params[i].nm, (u64)strlen(it->params[i].nm)); fputc(')', o);
            }
            fputs(") ", o); dump_type(o, it->ret);
            break;
        }
        case TK_IT_STRUCT: {
            fputc(' ', o); esc(o, it->sname, (u64)strlen(it->sname)); fputs(" (", o);
            for (i = 0; i < it->fcount; i++) {
                if (i) fputc(' ', o);
                fprintf(o, "(%d %d %d ", it->fields[i].is_pub ? 1 : 0,
                    it->fields[i].is_priv ? 1 : 0, it->fields[i].is_mut ? 1 : 0);
                dump_type(o, it->fields[i].ty);
                fputc(' ', o); esc(o, it->fields[i].nm, (u64)strlen(it->fields[i].nm)); fputc(')', o);
            }
            fputs(")", o);
            break;
        }
        case TK_IT_ENUM: {
            fputc(' ', o); esc(o, it->ename, (u64)strlen(it->ename));             fputs(" (", o);
            for (i = 0; i < it->vcount; i++) {
                if (i) fputc(' ', o);
                fputc('(', o);
                esc(o, it->vars[i].vn, (u64)strlen(it->vars[i].vn));
                fprintf(o, " %d %d %d (", it->vars[i].has_tuple ? 1 : 0,
                    it->vars[i].has_struct ? 1 : 0, it->vars[i].has_disc ? 1 : 0);
                for (j = 0; j < it->vars[i].tn; j++) {
                    if (j) fputc(' ', o);
                    dump_type(o, it->vars[i].ts[j]);
                }
                fputs(") (", o);
                for (j = 0; j < it->vars[i].fn2; j++) {
                    if (j) fputc(' ', o);
                    fputc('(', o); dump_type(o, it->vars[i].fs[j].ty); fputc(' ', o);
                    esc(o, it->vars[i].fs[j].nm, (u64)strlen(it->vars[i].fs[j].nm)); fputc(')', o);
                }
                fputs(") ", o); dump_expr(o, it->vars[i].has_disc ? it->vars[i].disc : NULL);
                fputc(')', o);
            }
            fputs(")", o);
            break;
        }
        case TK_IT_GLOBAL: {
            TkStmt *d = it->gdecl;
            fprintf(o, " %d ", d->is_mut ? 1 : 0);
            dump_type(o, d->ty);
            fputc(' ', o); esc(o, d->names[0], (u64)strlen(d->names[0]));
            fputc(' ', o); dump_expr(o, d->inits[0]);
            break;
        }
        default: break; // unreached builder kinds
    }
    fputs(")\n", o);
}

void dump_sexp(TkProgram *prog) {
    puts("(program");
    for (int i = 0; i < prog->count; i++) dump_item(stdout, prog->items[i]);
    puts(")");
}
