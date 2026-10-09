#include "sema.h"
#include "ds_list.h"
#include "ds_map.h"
#include <stdio.h>
#include <string.h>

// Minimal checker for runnable core + mut semantics.
// Full rules in grammar.ebnf; advanced effect/trait/lifetime checks are
// conservative stubs (accept + note) so core programs can run.
//
// Scopes use growable List (ds_list, arena-backed) + linear search.
// Linear beats hashing for small scopes (<~32 bindings, the common case:
// function locals/params); globals additionally carry a StrMap index so
// large programs (self-hosted compiler tables) resolve in O(1).

typedef struct { char *name; b32 is_mut; b32 is_ref; } ScopeVar;
VEC_DECLARE(ScopeVarVec, ScopeVar);

static int errors = 0;
static void err(const char *msg, const char *detail) {
    fprintf(stderr, "sema error: %s%s%s%s\n", msg, detail?" '":"", detail?detail:"", detail?"'":"");
    errors++;
}

typedef struct Scope Scope;
struct Scope {
    ScopeVarVec vars;   // growable, arena-backed (no cap-8 limit)
    StrMap index;       // name -> u64 idx; built for global scope (self-host scale)
    b32 use_index;
    Scope *parent;
    mem_arena *arena;
};

static Scope *scope_new(mem_arena *a, Scope *par) {
    Scope *s = (Scope*)arena_push(a, sizeof(Scope), 1);
    vec_ScopeVarVec_init(&s->vars);
    s->use_index = 0;
    s->parent = par; s->arena = a;
    return s;
}
static void scope_use_index(Scope *s) {
    if (s->use_index) return;
    strmap_init_arena(&s->index, s->arena);
    for (u64 i = 0; i < vec_ScopeVarVec_len(&s->vars); i++)
        strmap_put(&s->index, vec_ScopeVarVec_get(&s->vars, i)->name, (void*)(u64)(i + 1));
    s->use_index = 1;
}
static void scope_add(Scope *s, const char *name, b32 is_mut, b32 is_ref) {
    // dup check in current scope (fatal: counts toward check failure)
    for (u64 i = 0; i < vec_ScopeVarVec_len(&s->vars); i++)
        if (strcmp(vec_ScopeVarVec_get(&s->vars, i)->name, name)==0) {
            err("duplicate binding", name);
            return;
        }
    ScopeVar v = {(char*)name, is_mut, is_ref};
    if (vec_ScopeVarVec_push_arena(&s->vars, v, s->arena) != 0) {
        err("out of memory", NULL);
        return;
    }
    if (s->use_index)
        strmap_put(&s->index, vec_ScopeVarVec_get(&s->vars, vec_ScopeVarVec_len(&s->vars)-1)->name,
            (void*)(u64)vec_ScopeVarVec_len(&s->vars)); // 1-based idx
}
static int scope_find(Scope *s, const char *name, b32 *is_mut) {
    for (Scope *p = s; p; p = p->parent) {
        if (p->use_index) {
            void *v = NULL;
            if (strmap_get(&p->index, name, &v)) {
                u64 idx = (u64)v - 1;
                if (is_mut) *is_mut = vec_ScopeVarVec_get(&p->vars, idx)->is_mut;
                return 1;
            }
            continue;
        }
        for (u64 i = 0; i < vec_ScopeVarVec_len(&p->vars); i++)
            if (strcmp(vec_ScopeVarVec_get(&p->vars, i)->name, name)==0) {
                if (is_mut) *is_mut = vec_ScopeVarVec_get(&p->vars, i)->is_mut;
                return 1;
            }
    }
    return 0;
}

// forward
static void check_expr(Scope *s, TkExpr *e);
static void check_stmt(Scope *s, TkStmt *st);

static void check_pattern(Scope *s, TkPattern *p) {
    if (!p) return;
    if (p->kind == TK_PAT_BIND) {
        if (p->is_mut && p->path.count==0) {
            // `mut x` binding is fine
        }
        scope_add(s, p->name, p->is_mut, 0);
    } else if (p->kind == TK_PAT_REF) {
        scope_add(s, p->name, 0, 1);
    } else if (p->kind == TK_PAT_PATH) {
        // variant pattern args bind
        for (int i = 0; i < p->acount; i++) check_pattern(s, p->args[i]);
        for (int i = 0; i < p->fcount; i++) if (p->fields[i].pat) check_pattern(s, p->fields[i].pat);
    }
    for (int i = 0; i < p->or_count; i++) check_pattern(s, p->or_next[i]);
}
static void check_pat_variant(TkPattern *p);
static void check_stmts(Scope *s, TkStmt **ss, int n, TkExpr *trail);

static void check_mut_assign(Scope *s, TkExpr *lhs) {
    // Direct var mutation through non-mut is an error (mut.tk: `a += 8` where a non-mut).
    // Through `*p`/field/index we allow (pointer/reference semantics, checked conservatively).
    TkExpr *e = lhs;
    // strip parens
    while (e && e->kind == TK_EX_PAREN) e = e->lhs;
    if (!e) return;
    if (e->kind == TK_EX_VAR && e->path.count == 1) {
        b32 is_mut = 0;
        if (scope_find(s, e->path.segs[0].seg, &is_mut)) {
            if (!is_mut) err("mutation of non-mut binding", e->path.segs[0].seg);
        } else {
            err("assignment to undefined variable", e->path.segs[0].seg);
        }
    } else if (e->kind == TK_EX_VAR) {
        // `obj.field` etc: check base defined (best effort)
        if (e->path.count > 0 && !scope_find(s, e->path.segs[0].seg, NULL))
            err("use of undefined variable", e->path.segs[0].seg);
        // field/index through var: no mut check (permissive core)
    } else if (e->kind == TK_EX_UNARY || e->kind == TK_EX_FIELD || e->kind == TK_EX_INDEX) {
        // *p, xs[0], obj.field on LHS: recurse to base for definedness
        check_expr(s, e);
    }
}

static int expr_has_effect(TkExpr *e) {
    if (!e) return 0;
    switch (e->kind) {
        case TK_EX_CALL: case TK_EX_PERFORM: case TK_EX_HANDLE:
        case TK_EX_RESUME: case TK_EX_SPAWN: case TK_EX_TRANSACT:
        case TK_EX_POST_INC: case TK_EX_POST_DEC: return 1;
        case TK_EX_UNARY: return e->op==TOKEN_PLUSPLUS||e->op==TOKEN_MINUSMINUS;
        case TK_EX_BINARY: return 0;
        default: return 0;
    }
}

static void check_expr(Scope *s, TkExpr *e) {
    if (!e) return;
    switch (e->kind) {
        case TK_EX_VAR:
            if (e->path.count>0 && !scope_find(s, e->path.segs[0].seg, NULL)) {
                // allow function names / globals: only check locals? conservative: if single seg and not found, check globals later?
                // For core, require defined unless it looks like a global/fn (we pre-populated globals).
                err("use of undefined variable", e->path.segs[0].seg);
            }
            break;
        case TK_EX_CALL:
            check_expr(s, e->callee);
            for (int i=0;i<e->nargs;i++) check_expr(s, e->args[i]);
            break;
        case TK_EX_UNARY:
            if ((e->op==TOKEN_PLUSPLUS||e->op==TOKEN_MINUSMINUS)) check_mut_assign(s, e->lhs);
            else check_expr(s, e->lhs);
            break;
        case TK_EX_BINARY:
            check_expr(s, e->lhs); check_expr(s, e->rhs);
            break;
        case TK_EX_CAST: check_expr(s, e->lhs); break;
        case TK_EX_TERNARY: check_expr(s, e->cond); check_expr(s, e->then_e); check_expr(s, e->else_e); break;
        case TK_EX_PAREN: check_expr(s, e->lhs); break;
        case TK_EX_INDEX: check_expr(s, e->callee); check_expr(s, e->index); break;
        case TK_EX_FIELD: check_expr(s, e->callee); break;
        case TK_EX_POST_INC: case TK_EX_POST_DEC: check_mut_assign(s, e->lhs); break;
        case TK_EX_STRUCT_LIT: for(int i=0;i<e->scount;i++) check_expr(s, e->sfields[i].e); break;
        case TK_EX_ARRAY_LIST: for(int i=0;i<e->ecount;i++) check_expr(s, e->elems[i]); break;
        case TK_EX_ARRAY_FILL: check_expr(s, e->fill_elem); check_expr(s, e->fill_n); break;
        case TK_EX_IF:
            check_expr(s, e->if_cond); break;
        case TK_EX_MATCH: {
            check_expr(s, e->scrut);
            for (int i = 0; i < e->arm_n; i++) {
                Scope *arm = scope_new(s->arena, s);
                check_pat_variant(e->arms[i].pat);
                check_pattern(arm, e->arms[i].pat);
                if (e->arms[i].guard) check_expr(arm, e->arms[i].guard);
                if (e->arms[i].body_is_block) {
                    check_stmts(arm, e->arms[i].bs, e->arms[i].bn, e->arms[i].bt);
                } else if (e->arms[i].body) {
                    check_expr(arm, e->arms[i].body);
                }
            }
            break;
        }
        case TK_EX_PERFORM: for(int i=0;i<e->pargc;i++) check_expr(s, e->pargs[i]); break;
        case TK_EX_HANDLE: check_expr(s, e->h_scrut); break;
        case TK_EX_SPAWN: if(e->callee) check_expr(s, e->callee); break;
        case TK_EX_RESUME: if(e->has_resume_e) check_expr(s, e->resume_e); break;
        case TK_EX_TRANSACT: break;
        case TK_EX_LAMBDA: break; // captures checked at backend (non-escaping stub)
        case TK_EX_SIZEOF: if(!e->sizeof_is_type) check_expr(s, e->sizeof_ex); break;
        case TK_EX_OFFSETOF: break;
        case TK_EX_BRACE: case TK_EX_BLOCK: break;
        default: break;
    }
}

static void check_stmts(Scope *s, TkStmt **ss, int n, TkExpr *trail) {
    for (int i=0;i<n;i++) check_stmt(s, ss[i]);
    if (trail) {
        // trailing value must be pure-ish? allow any expr
        check_expr(s, trail);
    }
}

static const TkProgram *sema_prog = NULL;
static TkItem *sema_find_enum(const char *name) {
    if (!sema_prog) return NULL;
    for (int i = 0; i < sema_prog->count; i++)
        if (sema_prog->items[i]->kind == TK_IT_ENUM && strcmp(sema_prog->items[i]->ename, name) == 0)
            return sema_prog->items[i];
    return NULL;
}
// MVP structural rule for nested patterns: only plain bindings, wildcards and
// literals (variant paths without args would need type-directed resolution).
static int pat_arg_simple(TkPattern *p) {
    if (!p) return 1;
    if (p->kind == TK_PAT_BIND || p->kind == TK_PAT_WILD || p->kind == TK_PAT_LIT) return 1;
    return 0;
}
static int pat_binds(TkPattern *p) {
    if (!p) return 0;
    if (p->kind == TK_PAT_BIND || p->kind == TK_PAT_REF) return 1;
    if (p->kind == TK_PAT_PATH) {
        for (int i = 0; i < p->acount; i++) if (pat_binds(p->args[i])) return 1;
        for (int i = 0; i < p->fcount; i++) if (pat_binds(p->fields[i].pat)) return 1;
    }
    return 0;
}
static void check_pat_alt(TkPattern *p) {
    if (!p) return;
    if (p->kind == TK_PAT_WILD || p->kind == TK_PAT_LIT ||
        p->kind == TK_PAT_BIND || p->kind == TK_PAT_REF) return;
    if (p->kind == TK_PAT_PATH) {
        if (p->path.count != 2) { err("qualified Enum::Variant needed in core patterns", NULL); return; }
        TkItem *en = sema_find_enum(p->path.segs[0].seg);
        if (!en) { err("unknown enum in pattern", p->path.segs[0].seg); return; }
        TkItem *e2 = en; int vi = -1;
        for (int i = 0; i < e2->vcount; i++)
            if (strcmp(e2->vars[i].vn, p->path.segs[1].seg) == 0) { vi = i; break; }
        if (vi < 0) { err("unknown variant in pattern", p->path.segs[1].seg); return; }
        if (e2->vars[vi].has_tuple) {
            if (p->acount != e2->vars[vi].tn) { err("wrong tuple arity in pattern", p->path.segs[1].seg); return; }
            for (int i = 0; i < p->acount; i++)
                if (!pat_arg_simple(p->args[i])) { err("nested patterns TODO in core (use plain bindings)", NULL); return; }
        } else if (e2->vars[vi].has_struct) {
            for (int i = 0; i < p->fcount; i++) {
                int found = 0;
                for (int j = 0; j < e2->vars[vi].fn2; j++)
                    if (strcmp(e2->vars[vi].fs[j].nm, p->fields[i].field) == 0) { found = 1; break; }
                if (!found && !p->has_dotdot) { err("unknown field in pattern", p->fields[i].field); return; }
                if (!pat_arg_simple(p->fields[i].pat)) { err("nested patterns TODO in core (use plain bindings)", NULL); return; }
            }
        } else {
            if (p->acount || p->fcount) err("unit variant takes no pattern args", p->path.segs[1].seg);
        }
        return;
    }
}
static void check_pat_variant(TkPattern *p) {
    if (!p) return;
    if (p->or_count > 0) {
        if (pat_binds(p)) { err("or-pattern bindings TODO in core (keep or-branches binding-free)", NULL); return; }
        for (int i = 0; i < p->or_count; i++) {
            if (pat_binds(p->or_next[i])) { err("or-pattern bindings TODO in core (keep or-branches binding-free)", NULL); return; }
            check_pat_alt(p->or_next[i]);
        }
    }
    check_pat_alt(p);
}

static void check_stmt(Scope *s, TkStmt *st) {
    if (!st) return;
    switch (st->kind) {
        case TK_ST_DECL: {
            if (st->ty && st->ty->kind==TK_TY_PATH && st->ty->is_ref && st->is_mut)
                err("`mut` on `&` is a checker error (14d)", st->names[0]);
            for (int i=0;i<st->dcount;i++) {
                check_expr(s, st->inits[i]);
                scope_add(s, st->names[i], st->is_mut, st->ty && st->ty->is_ref);
            }
            break;
        }
        case TK_ST_SIMPLE:
            if (!st->has_assign) {
                // rule 17: statement with no effect is an error
                if (!expr_has_effect(st->e1)) {
                    // allow calls through field/method which expr_has_effect marks via CALL
                    // `i++`/`++i` handled via POST_INC/UNARY
                    // Be conservative: only error for pure binary/var/literal
                    TkExprKind k = st->e1 ? st->e1->kind : TK_EX_NULL;
                    if (k==TK_EX_VAR||k==TK_EX_INT||k==TK_EX_FLOAT||k==TK_EX_STRING||k==TK_EX_BINARY)
                        err("statement with no effect", NULL);
                }
                check_expr(s, st->e1);
            } else {
                check_mut_assign(s, st->e1);
                check_expr(s, st->e2);
                // also check base of lhs for definedness (e.g. *p)
                if (st->e1 && (st->e1->kind==TK_EX_UNARY||st->e1->kind==TK_EX_INDEX||st->e1->kind==TK_EX_FIELD))
                    check_expr(s, st->e1);
            }
            break;
        case TK_ST_BLOCK: { Scope *inner = scope_new(s->arena, s); check_stmts(inner, st->stmts, st->n, st->trailing); break; }
        case TK_ST_IF: {
            check_expr(s, st->cond_ex ? st->cond_ex : st->cond);
            Scope *inner = scope_new(s->arena, s);
            check_stmts(inner, st->then_s, st->then_n, st->then_t);
            for (int i = 0; i < st->eif_n; i++) {
                Scope *ei = scope_new(s->arena, s);
                check_expr(s, st->eifs[i].e ? st->eifs[i].e : st->eifs[i].c);
                check_stmts(ei, st->eifs[i].s, st->eifs[i].n, st->eifs[i].t);
            }
            if (st->has_els) { Scope *el = scope_new(s->arena, s); check_stmts(el, st->els, st->els_n, st->els_t); }
            break;
        }
        case TK_ST_HANDLE_STMT: case TK_ST_TRANSACT_STMT:
            check_stmts(s, st->stmts, st->n, st->trailing); break;
        case TK_ST_MATCH_STMT: {
            check_expr(s, st->m_scrut);
            for (int i = 0; i < st->m_arm_n; i++) {
                Scope *arm = scope_new(s->arena, s);
                check_pat_variant(st->m_arms[i].pat);
                check_pattern(arm, st->m_arms[i].pat);
                if (st->m_arms[i].guard) check_expr(arm, st->m_arms[i].guard);
                check_stmts(arm, st->m_arms[i].bs, st->m_arms[i].bn, NULL);
            }
            break;
        }
        case TK_ST_WHILE: {
            check_expr(s, st->cond_ex ? st->cond_ex : st->cond);
            Scope *inner = scope_new(s->arena, s);
            check_stmts(inner, st->then_s, st->then_n, st->then_t); break;
        }
        case TK_ST_FOR_C: {
            Scope *loop = scope_new(s->arena, s);
            if (!st->for_init_empty) {
                if (st->for_init_is_decl) {
                    for (int i = 0; i < st->for_init_decl->dcount; i++) {
                        check_expr(loop, st->for_init_decl->inits[i]);
                        scope_add(loop, st->for_init_decl->names[i], st->for_init_decl->is_mut, 0);
                    }
                } else if (st->for_init_simple->has_assign) {
                    check_mut_assign(loop, st->for_init_simple->e1);
                    check_expr(loop, st->for_init_simple->e2);
                } else check_expr(loop, st->for_init_simple->e1);
            }
            if (st->has_for_cond) check_expr(loop, st->for_cond);
            if (st->has_incr) {
                if (st->for_incr->has_assign) { check_mut_assign(loop, st->for_incr->e1); check_expr(loop, st->for_incr->e2); }
                else check_expr(loop, st->for_incr->e1);
            }
            Scope *body = scope_new(s->arena, loop);
            check_stmts(body, st->then_s, st->then_n, st->then_t);
            break;
        }
        case TK_ST_FOR_EACH: {
            Scope *inner = scope_new(s->arena, s);
            if (st->fe_auto) { /* auto x: fresh mut? treat as mut for loop var */ }
            check_pattern(inner, st->fe_pat);
            check_expr(s, st->fe_ex);
            check_stmts(inner, st->then_s, st->then_n, st->then_t);
            break;
        }
        case TK_ST_DEFER: break;
        case TK_ST_BREAK: case TK_ST_CONTINUE: break;
        case TK_ST_RETURN: if (st->has_ret) check_expr(s, st->ret_e); break;
        case TK_ST_LABEL: check_stmt(s, st->lab_s); break;
        case TK_ST_SPAWN_BLK: check_stmts(s, st->sp_blk, st->sp_n, st->sp_t); break;
        case TK_ST_SPAWN_CALL: check_expr(s, st->sp_call); break;
        default: break;
    }
    // Defer bodies checked in backend phase (scope exits); simplified here.
}

int sema_check(TkProgram *prog, mem_arena *arena) {
    errors = 0;
    sema_prog = prog;
    Scope *glob = scope_new(arena, NULL);
    // pre-populate function/global names so calls resolve
    for (int i=0;i<prog->count;i++) {
        TkItem *it = prog->items[i];
        if (it->kind==TK_IT_FN) scope_add(glob, it->fn_name, 0, 0);
        else if (it->kind==TK_IT_EXTERN) scope_add(glob, it->fn_name, 0, 0);
        else if (it->kind==TK_IT_GLOBAL) {
            for (int j=0;j<it->gdecl->dcount;j++) scope_add(glob, it->gdecl->names[j], it->gdecl->is_mut, 0);
        } else if (it->kind==TK_IT_STRUCT) scope_add(glob, it->sname, 0, 0);
        else if (it->kind==TK_IT_ENUM) scope_add(glob, it->ename, 0, 0);
    }
    scope_use_index(glob); // O(1) globals for self-host-scale programs
    for (int i=0;i<prog->count;i++) {
        TkItem *it = prog->items[i];
        if (it->kind==TK_IT_FN && !it->is_proto) {
            Scope *fn = scope_new(arena, glob);
            for (int j=0;j<it->pcount;j++) scope_add(fn, it->params[j].nm, it->params[j].is_mut, it->params[j].ty && it->params[j].ty->is_ref);
            check_stmts(fn, it->body, it->bcount, it->trail);
            // effect row basics: pure alone, transacts exclusion, suspends+decides
            if (it->eff) {
                int has_pure=0, has_trans=0, has_rw_a=0, has_susp=0, has_dec=0;
                for (int k=0;k<it->eff->count;k++) {
                    if (it->eff->items[k].kind==TK_EFF_BUILTIN) {
                        TokenKind b=it->eff->items[k].builtin;
                        if(b==TOKEN_PURE)has_pure=1; if(b==TOKEN_TRANSACTS)has_trans=1;
                        if(b==TOKEN_READS||b==TOKEN_WRITES||b==TOKEN_ALLOC)has_rw_a=1;
                        if(b==TOKEN_SUSPENDS)has_susp=1; if(b==TOKEN_DECIDES)has_dec=1;
                    }
                }
                if (has_pure && it->eff->count>1) err("'pure' only alone", it->fn_name);
                if (has_trans && has_rw_a) err("'transacts' never mixes with reads/writes/alloc", it->fn_name);
                if (has_susp && has_dec) err("suspends + decides never mix", it->fn_name);
                if (it->eff->count==0 && !it->eff->tail) err("'<>' is illegal", it->fn_name);
            }
        } else if (it->kind==TK_IT_GLOBAL) {
            check_expr(glob, it->gdecl->inits[0]);
        } else if (it->kind==TK_IT_ENUM) {
            int has_pay=0, has_disc=0;
            for(int j=0;j<it->vcount;j++){ if(it->vars[j].has_tuple||it->vars[j].has_struct)has_pay=1; if(it->vars[j].has_disc)has_disc=1; }
            if(has_pay&&has_disc) err("enum payload and '= const' never coexist", it->ename);
        }
    }
    return errors ? 1 : 0;
}
