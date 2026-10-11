#include "cgen.h"
#include "arena.h"
#include <string.h>
#include <stdlib.h>

static FILE *out;
static int tmp_n = 0;

// enum-table lookup used by expression emission (defined below)
typedef struct { char *vn; int kind; int tuple_n; char **sfields; int sfn; TkType **tuple_types; TkType **sfield_types; } CgVar;
typedef struct { char *ename; int payload; CgVar *vars; int vcount; } CgEnum;
static CgEnum *cg_find_enum(const char *name);
static int cg_find_variant(CgEnum *en, const char *vn);
static void cg_err(const char *msg);
static void cg_free_enums(void);
typedef struct { char *name; TkType *ret; } CgFn;
typedef struct { char *name; char **fnames; TkType **ftypes; int fn; } CgStruct;
static CgStruct *cg_find_struct(const char *name);
// List<T> monomorphization (onto ds_list growth policy: init 8, 2x, malloc/realloc)
static int cg_is_list(TkType *t);
static void emit_elem_mangle(TkType *t);
static void emit_list_name(TkType *elem);
// var-type table for List lowering (flat with append-order shadowing; do not shadow names in one fn)
typedef struct { char *name; TkType *ty; int depth; } CgVarEntry;
static int cg_depth = 0;
static void cg_var_add(char *name, TkType *ty);
static TkType *cg_var_find(const char *name);
static void cg_vars_fn_reset(void);
static TkType *infer_expr_type(TkExpr *e);

// ---- type mapping ----
static const char *c_prim(TokenKind k) {
    switch (k) {
        case TOKEN_I8: return "int8_t"; case TOKEN_I16: return "int16_t";
        case TOKEN_I32: return "int32_t"; case TOKEN_I64: return "int64_t";
        case TOKEN_U8: return "uint8_t"; case TOKEN_U16: return "uint16_t";
        case TOKEN_U32: return "uint32_t"; case TOKEN_U64: return "uint64_t";
        case TOKEN_F32: return "float"; case TOKEN_F64: return "double";
        case TOKEN_CHAR_TY: return "char"; case TOKEN_BOOL: return "bool";
        case TOKEN_SVIEW: return "TkSView"; case TOKEN_NEVER: return "void";
        default: return "int";
    }
}
static void emit_path(TkPath *p) {
    for (int i=0;i<p->count;i++) { if(i) fprintf(out,"_"); fprintf(out,"%s",p->segs[i].seg); }
}
static void emit_type(TkType *t) {
    if (!t) { fprintf(out,"void"); return; }
    if (t->kind==TK_TY_PRIM) {
        fprintf(out,"%s", c_prim(t->prim));
    } else if (t->kind==TK_TY_AUTO) {
        fprintf(out,"int"); // auto infers int in core demo (sema could refine; C needs concrete)
    } else if (t->kind==TK_TY_PATH) {
        if (t->path.count == 1 && strcmp(t->path.segs[0].seg, "List") == 0) {
            if (t->gcount != 1) { cg_err("List takes exactly one type arg"); emit_path(&t->path); }
            else emit_list_name(t->gargs[0]);
        }
        else {
            if (t->gcount) cg_err("only List<T> generics supported in core backend");
            emit_path(&t->path);
        }
    } else if (t->kind==TK_TY_TUPLE) {
        fprintf(out,"/*tuple*/ struct { ");
        for(int i=0;i<t->ecount;i++){ emit_type(t->elems[i]); fprintf(out," _%d; ",i); }
        fprintf(out,"}");
    } else if (t->kind==TK_TY_ARRAY) {
        emit_type(t->elem); // fixed arrays emit as pointer in core (decay); length in decl
    } else if (t->kind==TK_TY_FN) {
        // fn type as function pointer: ret (*)(params)
        if (t->ret) emit_type(t->ret); else fprintf(out,"void");
        fprintf(out," (*)(");
        for(int i=0;i<t->pcount;i++){ if(i)fprintf(out,", "); emit_type(t->params[i]); }
        fprintf(out,")");
        return;
    } else if (t->kind==TK_TY_DYN) {
        fprintf(out,"/*dyn*/ void*");
        return;
    } else {
        fprintf(out,"int");
    }
    for(int i=0;i<t->stars;i++) fprintf(out,"*");
    if(t->is_ref) fprintf(out,"*"); // T& -> T* (auto-deref at use)
    if(t->is_const) { /* const elided for core mutability demo */ }
}

// ---- expr/stmt forward ----
static void emit_expr(TkExpr *e);
static void emit_stmt(TkStmt *s, int indent);
static void emit_block_body(TkStmt **ss,int n,TkExpr *trail,int indent);

static void ind(int n){ for(int i=0;i<n;i++) fprintf(out,"    "); }

static void emit_path_expr(TkPath *p) {
    for(int i=0;i<p->count;i++){ if(i) fprintf(out,"_"); fprintf(out,"%s",p->segs[i].seg); }
}

static void emit_expr(TkExpr *e) {
    if(!e){ fprintf(out,"0"); return; }
    switch(e->kind){
        case TK_EX_INT: case TK_EX_FLOAT: case TK_EX_STRING: case TK_EX_CHAR:
            fprintf(out,"%.*s",(int)e->tok.len,e->tok.start); break;
        case TK_EX_BOOL: fprintf(out,"%s",e->tok.kind==TOKEN_TRUE?"true":"false"); break;
        case TK_EX_NULL: fprintf(out,"NULL"); break;
        case TK_EX_UNDEF: fprintf(out,"0"); break;
        case TK_EX_VAR: {
            // payload-enum unit variant as a value: `Opt::None` -> tag-only literal
            if (e->path.count == 2) {
                CgEnum *en = cg_find_enum(e->path.segs[0].seg);
                if (en && en->payload) {
                    int vi = cg_find_variant(en, e->path.segs[1].seg);
                    if (vi >= 0 && en->vars[vi].kind == 0) {
                        fprintf(out,"(%s){.tag=%s_%s}",en->ename,en->ename,en->vars[vi].vn);
                        break;
                    }
                }
            }
            emit_path_expr(&e->path); break;
        }
        case TK_EX_PAREN: fprintf(out,"("); emit_expr(e->lhs); fprintf(out,")"); break;
        case TK_EX_STRUCT_LIT: {
            // enum struct-variant construction: `E::V { f: e }` -> nested literal
            if (e->path.count == 2) {
                CgEnum *en = cg_find_enum(e->path.segs[0].seg);
                if (en && en->payload) {
                    int vi = cg_find_variant(en, e->path.segs[1].seg);
                    if (vi >= 0 && en->vars[vi].kind == 2) {
                        fprintf(out,"(%s){.tag=%s_%s,.u={.%s={",en->ename,en->ename,en->vars[vi].vn,en->vars[vi].vn);
                        for(int i=0;i<e->scount;i++){ if(i)fprintf(out,","); fprintf(out,".%s=",e->sfields[i].field); emit_expr(e->sfields[i].e); }
                        fprintf(out,"}}}"); break;
                    }
                    cg_err("struct literal on non-struct variant");
                    fprintf(out,"0"); break;
                }
            }
            fprintf(out,"("); emit_path_expr(&e->path); fprintf(out,"){");
            for(int i=0;i<e->scount;i++){ if(i)fprintf(out,", "); fprintf(out,".%s=",e->sfields[i].field); emit_expr(e->sfields[i].e); }
            fprintf(out,"}"); break;
        }
        case TK_EX_ARRAY_LIST:
            fprintf(out,"(int[]){");
            for(int i=0;i<e->ecount;i++){ if(i)fprintf(out,","); emit_expr(e->elems[i]); }
            fprintf(out,"}"); break;
        case TK_EX_ARRAY_FILL: fprintf(out,"0/*fill-stub*/"); break;
        case TK_EX_BRACE: case TK_EX_BLOCK:
            fprintf(out,"({ "); // GNU statement-expression (gcc)
            for(int i=0;i<e->scount2;i++){ emit_stmt(e->stmts[i],0); fprintf(out," "); }
            if(e->trailing) emit_expr(e->trailing); else fprintf(out,"0");
            fprintf(out,"; })"); break;
        case TK_EX_LAMBDA: fprintf(out,"0/*lambda-stub: non-escaping only*/"); break;
        case TK_EX_SIZEOF:
            fprintf(out,"sizeof(");
            if(e->sizeof_is_type) emit_type(e->sizeof_ty); else emit_expr(e->sizeof_ex);
            fprintf(out,")"); break;
        case TK_EX_OFFSETOF:
            if(e->off_bare) { emit_path_expr(&e->path); }
            else { fprintf(out,"offsetof("); emit_type(e->off_ty); fprintf(out,", %s)",e->off_field); }
            break;
        case TK_EX_IF:
            fprintf(out,"({ int __t%d=0; ",tmp_n++);
            // simplified: if as expression via ternary chain? For core demo avoid if-expr; stub
            fprintf(out,"0; })"); break;
        case TK_EX_MATCH: cg_err("match expressions only in decl-init/return positions in core"); fprintf(out,"0"); break;
        case TK_EX_PERFORM: fprintf(out,"tk_effect_stub()"); break;
        case TK_EX_HANDLE: emit_expr(e->h_scrut); break; // stub: ignore handler, eval scrutinee
        case TK_EX_SPAWN: if(e->callee) emit_expr(e->callee); else fprintf(out,"0"); break; // stub: synchronous
        case TK_EX_RESUME: if(e->has_resume_e) emit_expr(e->resume_e); else fprintf(out,"0"); break;
        case TK_EX_TRANSACT: fprintf(out,"({ "); for(int i=0;i<e->t_n;i++) emit_stmt(e->t_blk[i],0); fprintf(out,"1; })"); break; // stub: no rollback, yields true
        case TK_EX_UNARY:
            if(e->op==TOKEN_AMP){fprintf(out,"&");emit_expr(e->lhs);}
            else if(e->op==TOKEN_STAR){fprintf(out,"*");emit_expr(e->lhs);}
            else if(e->op==TOKEN_PLUSPLUS){emit_expr(e->lhs);fprintf(out,"++");}
            else if(e->op==TOKEN_MINUSMINUS){emit_expr(e->lhs);fprintf(out,"--");}
            else {fprintf(out,"(");fputs(e->op==TOKEN_PLUS?"+":e->op==TOKEN_MINUS?"-":e->op==TOKEN_BANG?"!":"~",out);emit_expr(e->lhs);fprintf(out,")");}
            break;
        case TK_EX_BINARY: {
            const char *op="?";
            switch(e->op){case TOKEN_PLUS:op="+";break;case TOKEN_MINUS:op="-";break;case TOKEN_STAR:op="*";break;case TOKEN_SLASH:op="/";break;case TOKEN_PERCENT:op="%";break;case TOKEN_SHL:op="<<";break;case TOKEN_SHR:op=">>";break;case TOKEN_AMP:op="&";break;case TOKEN_CARET:op="^";break;case TOKEN_PIPE:op="|";break;case TOKEN_EQEQ:op="==";break;case TOKEN_BANGEQ:op="!=";break;case TOKEN_LT:op="<";break;case TOKEN_GT:op=">";break;case TOKEN_LTEQ:op="<=";break;case TOKEN_GTEQ:op=">=";break;case TOKEN_AMP2:op="&&";break;case TOKEN_PIPE2:op="||";break;default:break;}
            fprintf(out,"(");emit_expr(e->lhs);fprintf(out,"%s",op);emit_expr(e->rhs);fprintf(out,")");break;
        }
        case TK_EX_CAST: {
            // `as` is a real conversion/reinterpret: emit an explicit C cast,
            // except to struct/enum/List/composite types (not castable in C).
            TkType *t = e->cast_to;
            int bare = 0;
            if (t && t->kind == TK_TY_PATH && t->path.count == 1 && t->stars == 0 && !t->is_ref) {
                const char *nm = t->path.segs[0].seg;
                if (cg_find_struct(nm) || cg_find_enum(nm)) bare = 1;
                else if (t->gcount) bare = 1; // List<..> etc: not C-castable
            } else if (t && (t->kind == TK_TY_TUPLE || t->kind == TK_TY_ARRAY || t->kind == TK_TY_FN || t->kind == TK_TY_DYN)) bare = 1;
            if (bare) { emit_expr(e->lhs); break; }
            fprintf(out, "(");
            if (t) emit_type(t); else fprintf(out, "int");
            fprintf(out, ")("); emit_expr(e->lhs); fprintf(out, ")");
            break;
        }
        case TK_EX_TERNARY: fprintf(out,"(");emit_expr(e->cond);fprintf(out,"?");emit_expr(e->then_e);fprintf(out,":");emit_expr(e->else_e);fprintf(out,")"); break;
        case TK_EX_CALL: {
            // payload-enum tuple construction: `Opt::Some(v, ...)` -> tag+union literal
            if (e->callee && e->callee->kind == TK_EX_VAR && e->callee->path.count == 2) {
                CgEnum *en = cg_find_enum(e->callee->path.segs[0].seg);
                if (en && en->payload) {
                    int vi = cg_find_variant(en, e->callee->path.segs[1].seg);
                    if (vi >= 0 && en->vars[vi].kind == 1) {
                        fprintf(out,"(%s){.tag=%s_%s,.u={.%s={",en->ename,en->ename,en->vars[vi].vn,en->vars[vi].vn);
                        for(int i=0;i<e->nargs;i++){ if(i)fprintf(out,","); fprintf(out,"._%d=",i); emit_expr(e->args[i]); }
                        fprintf(out,"}}}"); break;
                    }
                    cg_err("call on non-tuple variant");
                    fprintf(out,"0"); break;
                }
            }
            // List push: `xs.push(v)` / `m.slots.push(v)` -> `List_T_push(&base, v)`
            if (e->callee && e->callee->kind == TK_EX_FIELD && strcmp(e->callee->field, "push") == 0 &&
                e->callee->callee) {
                TkType *bt = infer_expr_type(e->callee->callee);
                if (cg_is_list(bt)) {
                    if (e->nargs != 1) { cg_err("push takes exactly one arg"); fprintf(out, "0"); break; }
                    emit_list_name(bt->gargs[0]); fprintf(out, "_push(&(");
                    emit_expr(e->callee->callee); fprintf(out, "), ");
                    emit_expr(e->args[0]); fprintf(out, ")"); break;
                }
            }
            emit_expr(e->callee); fprintf(out,"("); for(int i=0;i<e->nargs;i++){if(i)fprintf(out,",");emit_expr(e->args[i]);} fprintf(out,")"); break;
        }
        case TK_EX_INDEX: {
            // List indexing: `xs[i]` -> `xs.data[i]` when the base infers to List
            // (covers locals/params/globals, match bindings, and `m.slots`-style fields)
            if (e->callee) {
                TkType *bt = infer_expr_type(e->callee);
                if (cg_is_list(bt)) {
                    emit_expr(e->callee); fprintf(out,".data["); emit_expr(e->index); fprintf(out,"]"); break;
                }
            }
            emit_expr(e->callee);fprintf(out,"[");emit_expr(e->index);fprintf(out,"]"); break;
        }
        case TK_EX_FIELD: {
            // one-level auto-deref (grammar 15): base of pointer/ref type uses `->`
            TkType *bt = infer_expr_type(e->callee);
            if (bt && (bt->stars > 0 || bt->is_ref)) {
                emit_expr(e->callee); fprintf(out,"->%s",e->field); break;
            }
            emit_expr(e->callee);fprintf(out,".%s",e->field); break;
        }
        case TK_EX_POST_INC: emit_expr(e->lhs);fprintf(out,"++"); break;
        case TK_EX_POST_DEC: emit_expr(e->lhs);fprintf(out,"--"); break;
        default: fprintf(out,"0"); break;
    }
}

static const char *assign_op_str(TokenKind k){
    switch(k){case TOKEN_EQ:return "=";case TOKEN_PLUS_EQ:return "+=";case TOKEN_MINUS_EQ:return "-=";case TOKEN_STAR_EQ:return "*=";case TOKEN_SLASH_EQ:return "/=";case TOKEN_PERCENT_EQ:return "%=";case TOKEN_SHL_EQ:return "<<=";case TOKEN_SHR_EQ:return ">>=";case TOKEN_AMP_EQ:return "&=";case TOKEN_PIPE_EQ:return "|=";case TOKEN_CARET_EQ:return "^=";default:return "=";}
}

// ---- match support: enum table + pattern conditions/bindings ----
static int cg_failed = 0;
static int cg_used_defer = 0; // set when a nested __defer_ fn is emitted (needs execstack)
int cgen_needs_execstack(void) { return cg_used_defer; }
static void cg_err(const char *msg) { fprintf(stderr, "codegen error: %s\n", msg); cg_failed = 1; }

static CgEnum *cg_enums = NULL; static int cg_enum_n = 0;
// fn/struct tables for auto inference + field types
static CgFn *cg_fns = NULL; static int cg_fn_n = 0;
static CgStruct *cg_structs = NULL; static int cg_struct_n = 0;
static CgFn *cg_find_fn(const char *name) {
    for (int i = 0; i < cg_fn_n; i++) if (strcmp(cg_fns[i].name, name) == 0) return &cg_fns[i];
    return NULL;
}
static CgStruct *cg_find_struct(const char *name) {
    for (int i = 0; i < cg_struct_n; i++) if (strcmp(cg_structs[i].name, name) == 0) return &cg_structs[i];
    return NULL;
}
// collected List<T> instantiations (dedup by structural equality)
static TkType **cg_list_elems = NULL; static int cg_list_n = 0, cg_list_cap = 0;
static CgVarEntry *cg_vars = NULL; static int cg_var_n = 0, cg_var_cap = 0, cg_var_fn_base = 0;
// Per-emit side tables (fn/struct/List index): explicitly owned here, dropped
// at emit entry (stale state never survives) and emit end (no leaks).
static void cg_free_tables(void) {
    for (int i = 0; i < cg_struct_n; i++) {
        free(cg_structs[i].fnames);
        free(cg_structs[i].ftypes);
    }
    free(cg_list_elems); cg_list_elems = NULL; cg_list_n = 0; cg_list_cap = 0;
    free(cg_fns); cg_fns = NULL; cg_fn_n = 0;
    free(cg_structs); cg_structs = NULL; cg_struct_n = 0;
}
static CgEnum *cg_find_enum(const char *name) {
    for (int i = 0; i < cg_enum_n; i++) if (strcmp(cg_enums[i].ename, name) == 0) return &cg_enums[i];
    return NULL;
}
static int cg_find_variant(CgEnum *en, const char *vn) {
    for (int i = 0; i < en->vcount; i++) if (strcmp(en->vars[i].vn, vn) == 0) return i;
    return -1;
}
static void cg_build_enums(TkProgram *prog) {
    cg_free_enums(); // drop any previous emit's table: no leaks, no stale state
    int n = 0; for (int i = 0; i < prog->count; i++) if (prog->items[i]->kind == TK_IT_ENUM) n++;
    cg_enums = n ? (CgEnum*)calloc((size_t)n, sizeof(CgEnum)) : NULL; cg_enum_n = n;
    int k = 0;
    for (int i = 0; i < prog->count; i++) {
        TkItem *it = prog->items[i]; if (it->kind != TK_IT_ENUM) continue;
        CgEnum *en = &cg_enums[k++];
        en->ename = it->ename; en->vcount = it->vcount;
        en->vars = (CgVar*)calloc((size_t)(it->vcount ? it->vcount : 1), sizeof(CgVar));
        en->payload = 0;
        for (int j = 0; j < it->vcount; j++) {
            en->vars[j].vn = it->vars[j].vn;
            if (it->vars[j].has_tuple) {
                en->vars[j].kind = 1; en->vars[j].tuple_n = it->vars[j].tn; en->payload = 1;
                en->vars[j].tuple_types = it->vars[j].ts;
            }
            else if (it->vars[j].has_struct) {
                en->vars[j].kind = 2; en->payload = 1;
                en->vars[j].sfn = it->vars[j].fn2;
                en->vars[j].sfields = (char**)calloc((size_t)(it->vars[j].fn2 ? it->vars[j].fn2 : 1), sizeof(char*));
                en->vars[j].sfield_types = (TkType**)calloc((size_t)(it->vars[j].fn2 ? it->vars[j].fn2 : 1), sizeof(TkType*));
                for (int f = 0; f < it->vars[j].fn2; f++) {
                    en->vars[j].sfields[f] = it->vars[j].fs[f].nm;
                    en->vars[j].sfield_types[f] = it->vars[j].fs[f].ty;
                }
            } else en->vars[j].kind = 0;
        }
    }
}
static void cg_free_enums(void) {
    for (int i = 0; i < cg_enum_n; i++) {
        for (int j = 0; j < cg_enums[i].vcount; j++) {
            free(cg_enums[i].vars[j].sfields);
            free(cg_enums[i].vars[j].sfield_types);
        }
        free(cg_enums[i].vars);
    }
    free(cg_enums); cg_enums = NULL; cg_enum_n = 0;
}

// ---- List<T> monomorphization (ds_list growth policy: init 8, 2x) ----
static int cg_is_list(TkType *t) {
    return t && t->kind == TK_TY_PATH && t->path.count == 1 &&
           strcmp(t->path.segs[0].seg, "List") == 0 && t->gcount == 1 &&
           t->stars == 0 && !t->is_ref;
}
static const char *prim_spelling(TokenKind k) {
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
static void emit_elem_mangle(TkType *t) {
    if (!t) { fprintf(out, "void"); return; }
    if (t->kind == TK_TY_PRIM) { fprintf(out, "%s", prim_spelling(t->prim)); }
    else if (t->kind == TK_TY_PATH) {
        if (t->path.count == 1 && strcmp(t->path.segs[0].seg, "List") == 0 && t->gcount == 1) {
            fprintf(out, "List_"); emit_elem_mangle(t->gargs[0]);
        } else {
            if (t->gcount) { cg_err("only List<T> generics supported in core backend"); }
            for (int i = 0; i < t->path.count; i++) { if (i) fprintf(out, "_"); fprintf(out, "%s", t->path.segs[i].seg); }
        }
    } else { cg_err("List elem must be prim/path/List in core"); fprintf(out, "int"); }
    for (int i = 0; i < t->stars; i++) fprintf(out, "P");
    if (t->is_ref) { cg_err("ref List elems TODO in core"); }
}
static void emit_list_name(TkType *elem) { fprintf(out, "List_"); emit_elem_mangle(elem); }
static int type_eq(TkType *a, TkType *b) {
    if (a == b) return 1;
    if (!a || !b || a->kind != b->kind) return 0;
    if (a->stars != b->stars || a->is_ref != b->is_ref) return 0;
    if (a->kind == TK_TY_PRIM) return a->prim == b->prim;
    if (a->kind == TK_TY_PATH) {
        if (a->path.count != b->path.count || a->gcount != b->gcount) return 0;
        for (int i = 0; i < a->path.count; i++) if (strcmp(a->path.segs[i].seg, b->path.segs[i].seg)) return 0;
        for (int i = 0; i < a->gcount; i++) if (!type_eq(a->gargs[i], b->gargs[i])) return 0;
        return 1;
    }
    return 0; // other kinds unsupported as List elems in core
}
static void collect_ty(TkType *t);
static void collect_stmts(TkStmt **ss, int n);
static void collect_ty(TkType *t) {
    if (!t) return;
    if (cg_is_list(t)) {
        for (int i = 0; i < cg_list_n; i++) if (type_eq(cg_list_elems[i], t->gargs[0])) { collect_ty(t->gargs[0]); return; }
        if (cg_list_n == cg_list_cap) {
            cg_list_cap = cg_list_cap ? cg_list_cap * 2 : 8;
            cg_list_elems = (TkType**)realloc(cg_list_elems, sizeof(TkType*) * (size_t)cg_list_cap);
            if (!cg_list_elems) { cg_err("OOM"); exit(1); }
        }
        cg_list_elems[cg_list_n++] = t->gargs[0];
        collect_ty(t->gargs[0]); // nested List<List<..>>
        return;
    }
    if (t->kind == TK_TY_PATH) { for (int i = 0; i < t->gcount; i++) collect_ty(t->gargs[i]); }
    else if (t->kind == TK_TY_FN) { for (int i = 0; i < t->pcount; i++) collect_ty(t->params[i]); collect_ty(t->ret); }
    else if (t->kind == TK_TY_TUPLE) { for (int i = 0; i < t->ecount; i++) collect_ty(t->elems[i]); }
    else if (t->kind == TK_TY_ARRAY) { collect_ty(t->elem); }
}
static void collect_stmts(TkStmt **ss, int n) {
    for (int i = 0; i < n; i++) {
        TkStmt *s = ss[i]; if (!s) continue;
        switch (s->kind) {
            case TK_ST_DECL: collect_ty(s->ty); break;
            case TK_ST_BLOCK: collect_stmts(s->stmts, s->n); break;
            case TK_ST_IF:
                collect_stmts(s->then_s, s->then_n);
                for (int k = 0; k < s->eif_n; k++) collect_stmts(s->eifs[k].s, s->eifs[k].n);
                if (s->has_els) collect_stmts(s->els, s->els_n);
                break;
            case TK_ST_WHILE: collect_stmts(s->then_s, s->then_n); break;
            case TK_ST_FOR_C:
                if (!s->for_init_empty && s->for_init_is_decl) collect_ty(s->for_init_decl->ty);
                collect_stmts(s->then_s, s->then_n); break;
            case TK_ST_MATCH_STMT:
                for (int k = 0; k < s->m_arm_n; k++) collect_stmts(s->m_arms[k].bs, s->m_arms[k].bn);
                break;
            case TK_ST_LABEL: { TkStmt *inner[1] = {s->lab_s}; collect_stmts(inner, 1); break; }
            case TK_ST_SPAWN_BLK: collect_stmts(s->sp_blk, s->sp_n); break;
            case TK_ST_DEFER:
                if (s->defer_is_block) collect_stmts(s->defer_blk, s->defer_n);
                break;
            default: break;
        }
    }
}
static void cg_var_add(char *name, TkType *ty) {
    if (cg_var_n == cg_var_cap) {
        cg_var_cap = cg_var_cap ? cg_var_cap * 2 : 64;
        cg_vars = (CgVarEntry*)realloc(cg_vars, sizeof(*cg_vars) * (size_t)cg_var_cap);
        if (!cg_vars) { cg_err("OOM"); exit(1); }
    }
    cg_vars[cg_var_n].name = name; cg_vars[cg_var_n].ty = ty; cg_vars[cg_var_n].depth = cg_depth; cg_var_n++;
}
static TkType *cg_var_find(const char *name) {
    for (int i = cg_var_n - 1; i >= 0; i--) if (strcmp(cg_vars[i].name, name) == 0) return cg_vars[i].ty;
    return NULL;
}
static void cg_vars_fn_reset(void) { cg_var_n = cg_var_fn_base; cg_depth = 0; }
// Per-emit scratch arena ("bump and drop") for inferred types: O(1) bump
// alloc, zero per-node frees, whole arena dropped at emit end. Returned
// types are valid until cgen_emit returns; callers never free them. NULL
// (exhaustion) propagates through the existing unknown-type paths + cg_err.
static mem_arena *cg_scratch = NULL;
static void *cg_push(u64 sz) {
    void *p = cg_scratch ? arena_push(cg_scratch, sz, 1) : NULL;
    if (!p) cg_err("OOM in scratch arena");
    return p;
}
static TkType *new_ty_prim(TokenKind p) {
    TkType *t = (TkType*)cg_push(sizeof *t);
    if (t) { t->kind = TK_TY_PRIM; t->prim = p; }
    return t;
}
static TkType *new_ty_path1(const char *seg) {
    TkType *t = (TkType*)cg_push(sizeof *t);
    if (!t) return NULL;
    t->kind = TK_TY_PATH; t->path.count = 1;
    t->path.segs = (TkPathSeg*)cg_push(sizeof(TkPathSeg));
    if (!t->path.segs) return NULL;
    t->path.segs[0].seg = (char*)seg;
    return t;
}
static TkType *copy_ty_star(TkType *t, int dstar) {
    if (!t) return NULL;
    TkType *n = (TkType*)cg_push(sizeof *n);
    if (!n) return NULL;
    *n = *t; n->stars += dstar;
    return n;
}
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
// Best-effort type of an expression for `auto`. NULL = needs explicit annotation.
static TkType *infer_expr_type(TkExpr *e) {
    if (!e) return NULL;
    switch (e->kind) {
        case TK_EX_INT: return new_ty_prim(TOKEN_I32);
        case TK_EX_FLOAT: return new_ty_prim(TOKEN_F64);
        case TK_EX_STRING: { TkType *t = new_ty_prim(TOKEN_CHAR_TY); if (t) t->stars = 1; return t; }
        case TK_EX_CHAR: return new_ty_prim(TOKEN_CHAR_TY);
        case TK_EX_BOOL: return new_ty_prim(TOKEN_BOOL);
        case TK_EX_NULL: { TkType *t = new_ty_prim(TOKEN_U8); if (t) t->stars = 1; return t; }
        case TK_EX_VAR:
            if (e->path.count == 1) return cg_var_find(e->path.segs[0].seg);
            if (e->path.count == 2 && cg_find_enum(e->path.segs[0].seg)) return new_ty_path1(e->path.segs[0].seg);
            return NULL;
        case TK_EX_PAREN: return infer_expr_type(e->lhs);
        case TK_EX_CAST: return e->cast_to;
        case TK_EX_STRUCT_LIT:
            if (e->path.count == 1) return new_ty_path1(e->path.segs[0].seg);
            if (e->path.count == 2 && cg_find_enum(e->path.segs[0].seg)) return new_ty_path1(e->path.segs[0].seg);
            return NULL;
        case TK_EX_CALL: {
            if (e->callee && e->callee->kind == TK_EX_VAR && e->callee->path.count == 2 &&
                cg_find_enum(e->callee->path.segs[0].seg))
                return new_ty_path1(e->callee->path.segs[0].seg);
            if (e->callee && e->callee->kind == TK_EX_VAR && e->callee->path.count == 1) {
                CgFn *f = cg_find_fn(e->callee->path.segs[0].seg);
                if (f) return f->ret; // NULL (void) -> caller errors
            }
            return NULL;
        }
        case TK_EX_FIELD: {
            TkType *bt = infer_expr_type(e->callee);
            if (!bt) return NULL;
            if (bt->kind == TK_TY_PRIM && bt->prim == TOKEN_SVIEW && bt->stars == 0 && !bt->is_ref) {
                // builtin sview layout { ptr, len } (see taka_runtime.h TkSView)
                if (strcmp(e->field, "ptr") == 0) { TkType *t = new_ty_prim(TOKEN_CHAR_TY); if (t) t->stars = 1; return t; }
                if (strcmp(e->field, "len") == 0) return new_ty_prim(TOKEN_I64);
                return NULL;
            }
            if (bt->kind != TK_TY_PATH || bt->path.count != 1) return NULL;
            CgStruct *st = cg_find_struct(bt->path.segs[0].seg);
            if (!st) return NULL;
            for (int i = 0; i < st->fn; i++) if (strcmp(st->fnames[i], e->field) == 0) return st->ftypes[i];
            return NULL;
        }
        case TK_EX_INDEX: {
            TkType *bt = infer_expr_type(e->callee);
            if (!bt) return NULL;
            if (cg_is_list(bt)) return bt->gargs[0];
            if (bt->kind == TK_TY_ARRAY) return bt->elem;
            if (bt->stars > 0) return copy_ty_star(bt, -1);
            return NULL;
        }
        case TK_EX_UNARY:
            if (e->op == TOKEN_AMP) { TkType *t = infer_expr_type(e->lhs); return copy_ty_star(t, 1); }
            if (e->op == TOKEN_STAR) { TkType *t = infer_expr_type(e->lhs); if (t && t->stars > 0) return copy_ty_star(t, -1); return NULL; }
            if (e->op == TOKEN_BANG) return new_ty_prim(TOKEN_BOOL);
            return infer_expr_type(e->lhs);
        case TK_EX_POST_INC: case TK_EX_POST_DEC: return infer_expr_type(e->lhs);
        case TK_EX_BINARY: {
            TkType *l = infer_expr_type(e->lhs), *r = infer_expr_type(e->rhs);
            if (!l || !r) return NULL;
            int lp = (l->stars > 0 || l->is_ref), rp = (r->stars > 0 || r->is_ref);
            if (lp && !rp) return l;
            if (rp && !lp) return r;
            if (e->op == TOKEN_EQEQ || e->op == TOKEN_BANGEQ || e->op == TOKEN_LT || e->op == TOKEN_GT ||
                e->op == TOKEN_LTEQ || e->op == TOKEN_GTEQ || e->op == TOKEN_AMP2 || e->op == TOKEN_PIPE2)
                return new_ty_prim(TOKEN_BOOL);
            if (l->kind == TK_TY_PRIM && r->kind == TK_TY_PRIM) {
                int rl = prim_rank(l->prim), rr = prim_rank(r->prim);
                return new_ty_prim(rl >= rr ? l->prim : r->prim);
            }
            return NULL;
        }
        case TK_EX_TERNARY: return infer_expr_type(e->then_e);
        default: return NULL; // MATCH/IF/PERFORM/... need explicit annotation in core
    }
}
static void cg_scope_push(void) { cg_depth++; }
static void cg_scope_pop(void) {
    cg_depth--;
    while (cg_var_n > cg_var_fn_base && cg_vars[cg_var_n - 1].depth > cg_depth) cg_var_n--;
}
static void emit_pat_lit(TkPattern *p) {
    if (p->tok.kind == TOKEN_TRUE) { fprintf(out, "true"); return; }
    if (p->tok.kind == TOKEN_FALSE) { fprintf(out, "false"); return; }
    if (p->tok.kind == TOKEN_STRING) { cg_err("string patterns TODO in core backend"); fprintf(out, "0"); return; }
    fprintf(out, "%.*s", (int)p->tok.len, p->tok.start);
}
// Boolean condition for one alternative (no or/guards); references __m<id>.
static void emit_alt_cond(TkPattern *p, int id) {
    switch (p->kind) {
        case TK_PAT_WILD: case TK_PAT_BIND: fprintf(out, "1"); break;
        case TK_PAT_REF: fprintf(out, "1"); break;
        case TK_PAT_LIT: fprintf(out, "(__m%d == ", id); emit_pat_lit(p); fprintf(out, ")"); break;
        case TK_PAT_PATH: {
            if (p->path.count != 2) { cg_err("qualified Enum::Variant needed in core patterns"); fprintf(out, "0"); break; }
            CgEnum *en = cg_find_enum(p->path.segs[0].seg);
            if (!en) { cg_err("unknown enum in pattern"); fprintf(out, "0"); break; }
            int vi = cg_find_variant(en, p->path.segs[1].seg);
            if (vi < 0) { cg_err("unknown variant in pattern"); fprintf(out, "0"); break; }
            if (!en->payload) {
                fprintf(out, "(__m%d == %s_%s)", id, en->ename, en->vars[vi].vn);
            } else {
                fprintf(out, "(__m%d.tag == %s_%s", id, en->ename, en->vars[vi].vn);
                if (en->vars[vi].kind == 1) {
                    for (int i = 0; i < p->acount; i++) {
                        TkPattern *a = p->args[i];
                        if (a->kind == TK_PAT_LIT) { fprintf(out, " && (__m%d.u.%s._%d == ", id, en->vars[vi].vn, i); emit_pat_lit(a); fprintf(out, ")"); }
                        else if (a->kind != TK_PAT_BIND && a->kind != TK_PAT_WILD && a->kind != TK_PAT_REF) cg_err("nested patterns TODO in core backend");
                    }
                } else if (en->vars[vi].kind == 2) {
                    for (int i = 0; i < p->fcount; i++) {
                        TkPattern *sp = p->fields[i].pat;
                        if (sp && sp->kind == TK_PAT_LIT) { fprintf(out, " && (__m%d.u.%s.%s == ", id, en->vars[vi].vn, p->fields[i].field); emit_pat_lit(sp); fprintf(out, ")"); }
                        else if (!sp || (sp->kind != TK_PAT_BIND && sp->kind != TK_PAT_WILD && sp->kind != TK_PAT_REF)) cg_err("nested patterns TODO in core backend");
                    }
                }
                fprintf(out, ")");
            }
            break;
        }
    }
}
// Local bindings of the primary pattern (or-branches are binding-free by sema).
// Bindings are also recorded with their field types so later List lowering sees them.
static void emit_pat_binds(TkPattern *p, int id, int indent) {
    if (p->kind == TK_PAT_BIND) {
        ind(indent); fprintf(out, "__auto_type %s = __m%d;\n", p->name, id);
        cg_var_add(p->name, NULL); // whole-scrutinee type unknown in core; no List lowering
        return;
    }
    if (p->kind == TK_PAT_REF) {
        ind(indent); fprintf(out, "__auto_type %s = &__m%d;\n", p->name, id);
        cg_var_add(p->name, NULL);
        return;
    }
    if (p->kind != TK_PAT_PATH || p->path.count != 2) return;
    CgEnum *en = cg_find_enum(p->path.segs[0].seg);
    if (!en || !en->payload) return;
    int vi = cg_find_variant(en, p->path.segs[1].seg);
    if (vi < 0) return;
    if (en->vars[vi].kind == 1) {
        for (int i = 0; i < p->acount; i++)
            if (p->args[i]->kind == TK_PAT_BIND) {
                ind(indent); fprintf(out, "__auto_type %s = __m%d.u.%s._%d;\n", p->args[i]->name, id, en->vars[vi].vn, i);
                cg_var_add(p->args[i]->name, en->vars[vi].tuple_types ? en->vars[vi].tuple_types[i] : NULL);
            } else if (p->args[i]->kind == TK_PAT_REF) {
                ind(indent); fprintf(out, "__auto_type %s = &__m%d.u.%s._%d;\n", p->args[i]->name, id, en->vars[vi].vn, i);
                TkType *ft = en->vars[vi].tuple_types ? en->vars[vi].tuple_types[i] : NULL;
                cg_var_add(p->args[i]->name, copy_ty_star(ft, 1));
            }
    } else if (en->vars[vi].kind == 2) {
        for (int i = 0; i < p->fcount; i++)
            if (p->fields[i].pat && p->fields[i].pat->kind == TK_PAT_BIND) {
                ind(indent); fprintf(out, "__auto_type %s = __m%d.u.%s.%s;\n", p->fields[i].pat->name, id, en->vars[vi].vn, p->fields[i].field);
                TkType *ft = NULL;
                for (int j = 0; j < en->vars[vi].sfn; j++)
                    if (strcmp(en->vars[vi].sfields[j], p->fields[i].field) == 0) { ft = en->vars[vi].sfield_types[j]; break; }
                cg_var_add(p->fields[i].pat->name, ft);
            } else if (p->fields[i].pat && p->fields[i].pat->kind == TK_PAT_REF) {
                ind(indent); fprintf(out, "__auto_type %s = &__m%d.u.%s.%s;\n", p->fields[i].pat->name, id, en->vars[vi].vn, p->fields[i].field);
                TkType *ft = NULL;
                for (int j = 0; j < en->vars[vi].sfn; j++)
                    if (strcmp(en->vars[vi].sfields[j], p->fields[i].field) == 0) { ft = en->vars[vi].sfield_types[j]; break; }
                cg_var_add(p->fields[i].pat->name, copy_ty_star(ft, 1));
            }
    }
}

static TkType *cg_cur_ret = NULL;
// Value match shared by `T x = match ...` and `return match ...`.
// Target is an lvalue name already declared (zero-initialized by the caller,
// so non-exhaustive matches yield zero instead of UB).
static void emit_match_value(TkExpr *m, const char *tgt, int indent) {
    int id = tmp_n++;
    ind(indent);fprintf(out,"{\n");
    ind(indent+1);fprintf(out,"__auto_type __m%d = (",id); emit_expr(m->scrut); fprintf(out,");\n");
    ind(indent+1);fprintf(out,"int __done%d = 0;\n",id);
    for (int i = 0; i < m->arm_n; i++) {
        ind(indent+1);fprintf(out,"if (!__done%d && (",id);
        emit_alt_cond(m->arms[i].pat, id);
        for (int o = 0; o < m->arms[i].pat->or_count; o++) { fprintf(out," || "); emit_alt_cond(m->arms[i].pat->or_next[o], id); }
        fprintf(out,")) {\n");
        cg_scope_push();
        emit_pat_binds(m->arms[i].pat, id, indent+2);
        if (m->arms[i].guard) {
            ind(indent+2);fprintf(out,"if ("); emit_expr(m->arms[i].guard); fprintf(out,") {\n");
            ind(indent+3);fprintf(out,"%s = (",tgt); emit_expr(m->arms[i].body); fprintf(out,");\n");
            ind(indent+3);fprintf(out,"__done%d = 1;\n",id);
            ind(indent+2);fprintf(out,"}\n");
        } else {
            ind(indent+2);fprintf(out,"%s = (",tgt); emit_expr(m->arms[i].body); fprintf(out,");\n");
            ind(indent+2);fprintf(out,"__done%d = 1;\n",id);
        }
        cg_scope_pop();
        ind(indent+1);fprintf(out,"}\n");
    }
    ind(indent);fprintf(out,"}\n");
}

static void emit_block_body(TkStmt **ss,int n,TkExpr *trail,int indent){
    cg_scope_push();
    for(int i=0;i<n;i++) emit_stmt(ss[i],indent);
    if(trail){ ind(indent); emit_expr(trail); fprintf(out,";\n"); }
    cg_scope_pop();
}

// True defer: per-function registration stack of nested GNU-C fns, drained
// LIFO at every return and at function end. Slots are emitted unconditionally
// (unused attribute keeps -Wall clean) so defers nested in block values work.
static void emit_defer_drain(int indent) {
    ind(indent); fprintf(out, "while (__dlen) ((void(*)())__dbuf[--__dlen])();\n");
    ind(indent); fprintf(out, "free(__dbuf);\n");
}

static void emit_stmt(TkStmt *s,int indent){
    if(!s) return;
    switch(s->kind){
        case TK_ST_DECL:
            for(int i=0;i<s->dcount;i++){
                TkType *dt = s->ty;
                if (dt && dt->kind == TK_TY_AUTO) {
                    dt = infer_expr_type(s->inits[i]);
                    if (!dt) { cg_err("auto needs explicit type here in core (annotate the decl)"); dt = new_ty_prim(TOKEN_I32); }
                }
                // value match: `T x = match ...` (arms are exprs; zero-init covers non-exhaustive)
                if (s->inits[i] && s->inits[i]->kind == TK_EX_MATCH) {
                    ind(indent); emit_type(dt); fprintf(out, " %s = {0};\n", s->names[i]);
                    emit_match_value(s->inits[i], s->names[i], indent);
                    cg_var_add(s->names[i], dt);
                    break;
                }
                // List from array literal: heap-allocate + copy (ds_list policy)
                if (cg_is_list(dt) && s->inits[i] && s->inits[i]->kind == TK_EX_ARRAY_LIST) {
                    TkExpr *al = s->inits[i];
                    ind(indent); emit_type(dt); fprintf(out, " %s = {0};\n", s->names[i]);
                    if (al->ecount > 0) {
                        ind(indent); fprintf(out, "{\n");
                        ind(indent+1); fprintf(out, "%s.len = %d;\n", s->names[i], al->ecount);
                        ind(indent+1); fprintf(out, "%s.cap = %d;\n", s->names[i], al->ecount);
                        ind(indent+1); fprintf(out, "%s.data = (", s->names[i]);
                        emit_type(dt->gargs[0]); fprintf(out, "*)malloc(sizeof(");
                        emit_type(dt->gargs[0]); fprintf(out, ") * %d);\n", al->ecount);
                        ind(indent+1); fprintf(out, "if (!%s.data) { fprintf(stderr, \"OOM in List init\\n\"); exit(1); }\n", s->names[i]);
                        for (int k = 0; k < al->ecount; k++) {
                            ind(indent+1); fprintf(out, "%s.data[%d] = (", s->names[i], k);
                            emit_expr(al->elems[k]); fprintf(out, ");\n");
                        }
                        ind(indent); fprintf(out, "}\n");
                    }
                    cg_var_add(s->names[i], dt);
                    break;
                }
                ind(indent);
                if (s->inits[i] && s->inits[i]->kind == TK_EX_ARRAY_LIST) {
                    cg_err("array literal needs explicit type (List<T> or [T; N]) in core");
                    fprintf(out, "int %s = 0;\n", s->names[i]);
                } else if(dt && dt->kind==TK_TY_ARRAY){
                    emit_type(dt->elem); fprintf(out," %s[",s->names[i]);
                    // array length: try int literal from arr_n if int
                    if(dt->arr_n && dt->arr_n->kind==TK_EX_INT) fprintf(out,"%.*s",(int)dt->arr_n->tok.len,dt->arr_n->tok.start);
                    else fprintf(out,"4");
                    fprintf(out,"] = ");
                    emit_expr(s->inits[i]); fprintf(out,";\n");
                } else {
                    emit_type(dt); fprintf(out," %s = ",s->names[i]);
                    emit_expr(s->inits[i]); fprintf(out,";\n");
                }
                cg_var_add(s->names[i], dt);
            }
            break;
        case TK_ST_SIMPLE:
            ind(indent);
            emit_expr(s->e1);
            if(s->has_assign){ fprintf(out,"%s",assign_op_str(s->assign_op)); emit_expr(s->e2); }
            fprintf(out,";\n"); break;
        case TK_ST_BLOCK: ind(indent);fprintf(out,"{\n"); emit_block_body(s->stmts,s->n,s->trailing,indent+1); ind(indent);fprintf(out,"}\n"); break;
        case TK_ST_IF: {
            ind(indent);fprintf(out,"if("); emit_expr(s->cond_ex?s->cond_ex:s->cond); fprintf(out,"){\n");
            emit_block_body(s->then_s,s->then_n,s->then_t,indent+1); ind(indent);fprintf(out,"}");
            for(int i=0;i<s->eif_n;i++){ fprintf(out," else if("); emit_expr(s->eifs[i].e?s->eifs[i].e:s->eifs[i].c); fprintf(out,"){\n"); emit_block_body(s->eifs[i].s,s->eifs[i].n,s->eifs[i].t,indent+1); ind(indent);fprintf(out,"}"); }
            if(s->has_els){ fprintf(out," else {\n"); emit_block_body(s->els,s->els_n,s->els_t,indent+1); ind(indent);fprintf(out,"}"); }
            fprintf(out,"\n"); break;
        }
        case TK_ST_WHILE:
            ind(indent);fprintf(out,"while(");emit_expr(s->cond_ex?s->cond_ex:s->cond);fprintf(out,"){\n");
            emit_block_body(s->then_s,s->then_n,s->then_t,indent+1);ind(indent);fprintf(out,"}\n");break;
        case TK_ST_FOR_C: {
            cg_scope_push();
            ind(indent);fprintf(out,"for(");
            if(!s->for_init_empty){ if(s->for_init_is_decl){ TkType *ft = s->for_init_decl->ty; if (ft && ft->kind == TK_TY_AUTO) { ft = infer_expr_type(s->for_init_decl->inits[0]); if (!ft) { cg_err("auto needs explicit type here in core (annotate the decl)"); ft = new_ty_prim(TOKEN_I32); } } emit_type(ft); fprintf(out," %s=",s->for_init_decl->names[0]); emit_expr(s->for_init_decl->inits[0]); cg_var_add(s->for_init_decl->names[0], ft); } else { emit_expr(s->for_init_simple->e1); if(s->for_init_simple->has_assign){fprintf(out,"%s",assign_op_str(s->for_init_simple->assign_op));emit_expr(s->for_init_simple->e2);} } }
            fprintf(out,";");
            if(s->has_for_cond) emit_expr(s->for_cond);
            fprintf(out,";");
            if(s->has_incr){ emit_expr(s->for_incr->e1); if(s->for_incr->has_assign){fprintf(out,"%s",assign_op_str(s->for_incr->assign_op));emit_expr(s->for_incr->e2);} }
            fprintf(out,"){\n"); emit_block_body(s->then_s,s->then_n,s->then_t,indent+1); ind(indent);fprintf(out,"}\n"); cg_scope_pop(); break;
        }
        case TK_ST_FOR_EACH:
            ind(indent);fprintf(out,"/*for-each stub*/{\n"); emit_block_body(s->then_s,s->then_n,s->then_t,indent+1); ind(indent);fprintf(out,"}\n"); break;
        case TK_ST_MATCH_STMT: {
            int id = tmp_n++;
            ind(indent);fprintf(out,"{\n");
            ind(indent+1);fprintf(out,"__auto_type __m%d = (",id); emit_expr(s->m_scrut); fprintf(out,");\n");
            ind(indent+1);fprintf(out,"int __done%d = 0;\n",id);
            for (int i = 0; i < s->m_arm_n; i++) {
                ind(indent+1);fprintf(out,"if (!__done%d && (",id);
                emit_alt_cond(s->m_arms[i].pat, id);
                for (int o = 0; o < s->m_arms[i].pat->or_count; o++) { fprintf(out," || "); emit_alt_cond(s->m_arms[i].pat->or_next[o], id); }
                fprintf(out,")) {\n");
                cg_scope_push();
                emit_pat_binds(s->m_arms[i].pat, id, indent+2);
                if (s->m_arms[i].guard) {
                    ind(indent+2);fprintf(out,"if ("); emit_expr(s->m_arms[i].guard); fprintf(out,") {\n");
                    emit_block_body(s->m_arms[i].bs, s->m_arms[i].bn, NULL, indent+3);
                    ind(indent+3);fprintf(out,"__done%d = 1;\n",id);
                    ind(indent+2);fprintf(out,"}\n");
                } else {
                    emit_block_body(s->m_arms[i].bs, s->m_arms[i].bn, NULL, indent+2);
                    ind(indent+2);fprintf(out,"__done%d = 1;\n",id);
                }
                cg_scope_pop();
                ind(indent+1);fprintf(out,"}\n");
            }
            ind(indent);fprintf(out,"}\n"); break;
        }
        case TK_ST_DEFER: {
            int id = tmp_n++;
            cg_used_defer = 1;            ind(indent); fprintf(out, "void __defer_%d(void) {\n", id);
            if (s->defer_is_block) emit_block_body(s->defer_blk, s->defer_n, s->defer_t, indent+1);
            else { ind(indent+1); emit_expr(s->defer_simple->e1); if (s->defer_simple->has_assign) { fprintf(out, "%s", assign_op_str(s->defer_simple->assign_op)); emit_expr(s->defer_simple->e2); } fprintf(out, ";\n"); }
            ind(indent); fprintf(out, "}\n");
            ind(indent); fprintf(out, "{ if (__dlen == __dcap) { __dcap = __dcap ? __dcap * 2 : 4; __dbuf = realloc(__dbuf, __dcap * sizeof(void*)); if (!__dbuf) { fprintf(stderr, \"OOM in defer\\n\"); exit(1); } } __dbuf[__dlen++] = (void*)__defer_%d; }\n", id);
            break;
        }
        case TK_ST_BREAK: ind(indent);fprintf(out,"break;\n"); break;
        case TK_ST_CONTINUE: ind(indent);fprintf(out,"continue;\n"); break;
        case TK_ST_RETURN:
            if (s->has_ret && s->ret_e && s->ret_e->kind == TK_EX_MATCH) {
                if (!cg_cur_ret) { cg_err("return match in void function"); break; }
                int rid = tmp_n++;
                ind(indent); emit_type(cg_cur_ret); fprintf(out, " __ret%d = {0};\n", rid);
                char tgt[64]; snprintf(tgt, sizeof tgt, "__ret%d", rid);
                emit_match_value(s->ret_e, tgt, indent);
                emit_defer_drain(indent);
                ind(indent); fprintf(out, "return __ret%d;\n", rid);
                break;
            }
            if (s->has_ret) {
                ind(indent); fprintf(out, "{ __auto_type __rv = ("); emit_expr(s->ret_e); fprintf(out, "); ");
                fprintf(out, "while (__dlen) ((void(*)())__dbuf[--__dlen])(); free(__dbuf); return __rv; }\n");
            } else {
                ind(indent); fprintf(out, "{ while (__dlen) ((void(*)())__dbuf[--__dlen])(); free(__dbuf); return; }\n");
            }
            break;
        case TK_ST_LABEL: ind(indent);fprintf(out,"%s: ",s->lab); emit_stmt(s->lab_s,indent); break;
        case TK_ST_SPAWN_BLK: ind(indent);fprintf(out,"{\n"); emit_block_body(s->sp_blk,s->sp_n,s->sp_t,indent+1); ind(indent);fprintf(out,"}\n"); break;
        case TK_ST_SPAWN_CALL: ind(indent);emit_expr(s->sp_call);fprintf(out,";\n"); break;
        case TK_ST_HANDLE_STMT: case TK_ST_TRANSACT_STMT:
            ind(indent);fprintf(out,"{\n"); emit_block_body(s->stmts,s->n,s->trailing,indent+1); ind(indent);fprintf(out,"}\n"); break;
        default: break;
    }
}

static int list_depth(TkType *t) {
    if (cg_is_list(t)) return 1 + list_depth(t->gargs[0]);
    return 0;
}
// A type needs a List monomorphization defined before it if List appears inside.
static int has_list_type(TkType *t) {
    if (!t) return 0;
    if (cg_is_list(t)) return 1;
    if (t->kind == TK_TY_PATH) { for (int i = 0; i < t->gcount; i++) if (has_list_type(t->gargs[i])) return 1; return 0; }
    if (t->kind == TK_TY_FN) {
        for (int i = 0; i < t->pcount; i++) if (has_list_type(t->params[i])) return 1;
        return has_list_type(t->ret);
    }
    if (t->kind == TK_TY_TUPLE) { for (int i = 0; i < t->ecount; i++) if (has_list_type(t->elems[i])) return 1; return 0; }
    if (t->kind == TK_TY_ARRAY) return has_list_type(t->elem);
    return 0;
}
static int item_needs_list_first(TkItem *it) {
    if (it->kind == TK_IT_STRUCT) { for (int j = 0; j < it->fcount; j++) if (has_list_type(it->fields[j].ty)) return 1; return 0; }
    if (it->kind == TK_IT_ENUM) {
        for (int j = 0; j < it->vcount; j++) {
            if (it->vars[j].has_tuple) { for (int k = 0; k < it->vars[j].tn; k++) if (has_list_type(it->vars[j].ts[k])) return 1; }
            if (it->vars[j].has_struct) { for (int k = 0; k < it->vars[j].fn2; k++) if (has_list_type(it->vars[j].fs[k].ty)) return 1; }
        }
        return 0;
    }
    return 0;
}
int cgen_emit(TkProgram *prog, const char *out_path) {
    cg_failed = 0;
    cg_used_defer = 0;
    tmp_n = 0; // per-emit ids: same input -> same output, no history dependence
    arena_destroy(cg_scratch);
    cg_scratch = arena_create(MByte(16));
    if (!cg_scratch) return 1;
    cg_build_enums(prog);
    cg_free_tables();
    cg_var_n = 0; cg_var_fn_base = 0; cg_depth = 0;
    // fn + struct tables for auto inference / field types
    for (int i = 0; i < prog->count; i++) {
        TkItem *it = prog->items[i];
        if (it->kind == TK_IT_FN || it->kind == TK_IT_EXTERN) {
            cg_fns = (CgFn*)realloc(cg_fns, sizeof(*cg_fns) * (size_t)(cg_fn_n + 1));
            if (!cg_fns) { cg_err("OOM"); exit(1); }
            cg_fns[cg_fn_n].name = it->fn_name; cg_fns[cg_fn_n].ret = it->ret; cg_fn_n++;
        } else if (it->kind == TK_IT_STRUCT) {
            cg_structs = (CgStruct*)realloc(cg_structs, sizeof(*cg_structs) * (size_t)(cg_struct_n + 1));
            if (!cg_structs) { cg_err("OOM"); exit(1); }
            CgStruct *st = &cg_structs[cg_struct_n++];
            st->name = it->sname; st->fn = it->fcount;
            st->fnames = (char**)calloc((size_t)(it->fcount ? it->fcount : 1), sizeof(char*));
            st->ftypes = (TkType**)calloc((size_t)(it->fcount ? it->fcount : 1), sizeof(TkType*));
            for (int j = 0; j < it->fcount; j++) { st->fnames[j] = it->fields[j].nm; st->ftypes[j] = it->fields[j].ty; }
        }
    }
    // collect List<T> instantiations from all type positions
    for (int i = 0; i < prog->count; i++) {
        TkItem *it = prog->items[i];
        if (it->kind == TK_IT_STRUCT) { for (int j = 0; j < it->fcount; j++) collect_ty(it->fields[j].ty); }
        else if (it->kind == TK_IT_FN) {
            for (int j = 0; j < it->pcount; j++) collect_ty(it->params[j].ty);
            collect_ty(it->ret);
            if (!it->is_proto) collect_stmts(it->body, it->bcount);
        }
        else if (it->kind == TK_IT_GLOBAL) collect_ty(it->gdecl->ty);
        else if (it->kind == TK_IT_ALIAS) collect_ty(it->aty);
        else if (it->kind == TK_IT_EXTERN) { for (int j = 0; j < it->pcount; j++) collect_ty(it->params[j].ty); collect_ty(it->ret); }
    }
    // order by nesting depth so inner structs are defined first
    for (int i = 1; i < cg_list_n; i++) {
        TkType *t = cg_list_elems[i]; int d = list_depth(cg_list_elems[i]), j = i;
        while (j > 0 && list_depth(cg_list_elems[j-1]) > d) { cg_list_elems[j] = cg_list_elems[j-1]; j--; }
        cg_list_elems[j] = t;
    }
    out = fopen(out_path,"w");
    if(!out) { cg_free_enums(); arena_destroy(cg_scratch); cg_scratch = NULL; return 1; }
    fprintf(out,"// generated taka -> C (core runnable + stubs for dyn/effects). C stdlib only.\n");
    fprintf(out,"#include <stdint.h>\n#include <stdio.h>\n#include <stdbool.h>\n#include <stddef.h>\n#include <stdlib.h>\n");
    fprintf(out,"#include \"taka_runtime.h\"\n\n");
    // predeclare fns/structs/enums
    for(int i=0;i<prog->count;i++){
        TkItem *it=prog->items[i];
        if(it->kind==TK_IT_STRUCT) fprintf(out,"typedef struct %s %s;\n",it->sname,it->sname);
        if(it->kind==TK_IT_ENUM){
            int payload=0; for(int j=0;j<it->vcount;j++) if(it->vars[j].has_tuple||it->vars[j].has_struct) payload=1;
            if(!payload) fprintf(out,"typedef enum %s %s;\n",it->ename,it->ename);
            else fprintf(out,"typedef struct %s %s;\n",it->ename,it->ename);
        }
    }
    fprintf(out,"\n");
    for(int i=0;i<prog->count;i++){
        TkItem *it=prog->items[i];
        if(it->kind==TK_IT_STRUCT && item_needs_list_first(it)) continue; // deferred to pass 2
        if(it->kind==TK_IT_ENUM && item_needs_list_first(it)) continue;
        if(it->kind==TK_IT_STRUCT){
            fprintf(out,"struct %s {\n",it->sname);
            for(int j=0;j<it->fcount;j++){ fprintf(out,"    "); emit_type(it->fields[j].ty); fprintf(out," %s;\n",it->fields[j].nm); }
            fprintf(out,"};\n");
        } else if(it->kind==TK_IT_ENUM){
            int payload=0; for(int j=0;j<it->vcount;j++) if(it->vars[j].has_tuple||it->vars[j].has_struct) payload=1;
            if(!payload){
                fprintf(out,"enum %s {\n",it->ename);
                for(int j=0;j<it->vcount;j++){ fprintf(out,"    %s_%s",it->ename,it->vars[j].vn); if(it->vars[j].has_disc){fprintf(out," = ");emit_expr(it->vars[j].disc);} if(j+1<it->vcount)fprintf(out,","); fprintf(out,"\n"); }
                fprintf(out,"};\n");
            } else {
                fprintf(out,"struct %s {\n    int tag;\n    union {\n",it->ename);
                for(int j=0;j<it->vcount;j++) if(it->vars[j].has_tuple||it->vars[j].has_struct){
                    fprintf(out,"        struct { ");
                    if(it->vars[j].has_tuple){ for(int k=0;k<it->vars[j].tn;k++){emit_type(it->vars[j].ts[k]);fprintf(out," _%d; ",k);} }
                    else { for(int k=0;k<it->vars[j].fn2;k++){emit_type(it->vars[j].fs[k].ty);fprintf(out," %s; ",it->vars[j].fs[k].nm);} }
                    fprintf(out,"} %s;\n",it->vars[j].vn);
                }
                fprintf(out,"    } u;\n};\n");
                for(int j=0;j<it->vcount;j++) fprintf(out,"#define %s_%s %d\n",it->ename,it->vars[j].vn,j);
            }
        } else if(it->kind==TK_IT_ALIAS){
            fprintf(out,"typedef "); emit_type(it->aty); fprintf(out," %s;\n",it->aname);
        }
    }
    fprintf(out,"\n");
    // List<T> monomorphizations after elem types are defined
    // (same growth policy as ds_list: init 8, 2x; nested inners first via depth sort)
    for (int li = 0; li < cg_list_n; li++) {
        TkType *el = cg_list_elems[li];
        fprintf(out, "typedef struct { ");
        emit_type(el); fprintf(out, " *data; uint64_t len, cap; } ");
        emit_list_name(el); fprintf(out, ";\n");
        fprintf(out, "static void "); emit_list_name(el);
        fprintf(out, "_push("); emit_list_name(el); fprintf(out, " *xs, ");
        emit_type(el); fprintf(out, " v) {\n");
        fprintf(out, "    if (xs->len == xs->cap) { uint64_t nc = xs->cap ? xs->cap * 2 : 8; ");
        emit_type(el); fprintf(out, " *nd = (");
        emit_type(el); fprintf(out, "*)realloc(xs->data, nc * sizeof(");
        emit_type(el); fprintf(out, "));\n");
        fprintf(out, "        if (!nd) { fprintf(stderr, \"OOM in List push\\n\"); exit(1); } xs->data = nd; xs->cap = nc; }\n");
        fprintf(out, "    xs->data[xs->len++] = v;\n}\n");
    }
    fprintf(out, "\n");
    // pass 2: structs/enums that contain Lists (elems + monos now defined)
    for(int i=0;i<prog->count;i++){
        TkItem *it=prog->items[i];
        if(it->kind==TK_IT_STRUCT && item_needs_list_first(it)){
            fprintf(out,"struct %s {\n",it->sname);
            for(int j=0;j<it->fcount;j++){ fprintf(out,"    "); emit_type(it->fields[j].ty); fprintf(out," %s;\n",it->fields[j].nm); }
            fprintf(out,"};\n");
        } else if(it->kind==TK_IT_ENUM && item_needs_list_first(it)){
            fprintf(out,"struct %s {\n    int tag;\n    union {\n",it->ename);
            for(int j=0;j<it->vcount;j++) if(it->vars[j].has_tuple||it->vars[j].has_struct){
                fprintf(out,"        struct { ");
                if(it->vars[j].has_tuple){ for(int k=0;k<it->vars[j].tn;k++){emit_type(it->vars[j].ts[k]);fprintf(out," _%d; ",k);} }
                else { for(int k=0;k<it->vars[j].fn2;k++){emit_type(it->vars[j].fs[k].ty);fprintf(out," %s; ",it->vars[j].fs[k].nm);} }
                fprintf(out,"} %s;\n",it->vars[j].vn);
            }
            fprintf(out,"    } u;\n};\n");
            for(int j=0;j<it->vcount;j++) fprintf(out,"#define %s_%s %d\n",it->ename,it->vars[j].vn,j);
        }
    }
    fprintf(out, "\n");
    for(int i=0;i<prog->count;i++){
        TkItem *it=prog->items[i];
        if(it->kind==TK_IT_GLOBAL){
            for(int j=0;j<it->gdecl->dcount;j++){ emit_type(it->gdecl->ty); fprintf(out," %s = ",it->gdecl->names[j]); emit_expr(it->gdecl->inits[j]); fprintf(out,";\n"); cg_var_add(it->gdecl->names[j], it->gdecl->ty); }
        } else if(it->kind==TK_IT_EXTERN){
            fprintf(out,"extern "); if(it->ret) emit_type(it->ret); else fprintf(out,"void");
            fprintf(out," %s(",it->fn_name);
            for(int j=0;j<it->pcount;j++){if(j)fprintf(out,",");emit_type(it->params[j].ty);}
            fprintf(out,");\n");
        }
    }
    fprintf(out,"\n");
    // fn protos
    for(int i=0;i<prog->count;i++){
        TkItem *it=prog->items[i];
        if(it->kind==TK_IT_FN){ if(it->ret) emit_type(it->ret); else fprintf(out,"void"); fprintf(out," %s(",strcmp(it->fn_name,"main")==0?"taka_main":it->fn_name);
            for(int j=0;j<it->pcount;j++){if(j)fprintf(out,",");emit_type(it->params[j].ty);fprintf(out," %s",it->params[j].nm);} 
            if(it->pcount==0) fprintf(out,"void");
            fprintf(out,");\n"); }
    }
    // fn bodies (var table: globals below, fn params/locals above fn_base)
    cg_var_fn_base = cg_var_n;
    for(int i=0;i<prog->count;i++){
        TkItem *it=prog->items[i];
        if(it->kind==TK_IT_FN && !it->is_proto){
            cg_vars_fn_reset();
            cg_cur_ret = it->ret;
            for(int j=0;j<it->pcount;j++) cg_var_add(it->params[j].nm, it->params[j].ty);
            fprintf(out,"\n"); if(it->ret) emit_type(it->ret); else fprintf(out,"void");
            fprintf(out," %s(",strcmp(it->fn_name,"main")==0?"taka_main":it->fn_name);
            for(int j=0;j<it->pcount;j++){if(j)fprintf(out,",");emit_type(it->params[j].ty);fprintf(out," %s",it->params[j].nm);}
            if(it->pcount==0) fprintf(out,"void");
            fprintf(out,"){\n");
            ind(1); fprintf(out, "__attribute__((unused)) void **__dbuf = NULL;\n");
            ind(1); fprintf(out, "__attribute__((unused)) size_t __dlen = 0, __dcap = 0;\n");
            emit_block_body(it->body,it->bcount,it->trail,1);
            // drain defers on fallthrough (explicit returns drain at their own sites)
            emit_defer_drain(1);
            // void fallthrough: C requires return for non-void; core demo uses explicit returns
            fprintf(out,"}\n");
        } else if(it->kind==TK_IT_TRAIT||it->kind==TK_IT_IMPL||it->kind==TK_IT_EFFECT||it->kind==TK_IT_HANDLER){
            fprintf(out,"\n/* %s stub (vtable/handler runtime TODO) */\n",
                it->kind==TK_IT_TRAIT?"trait":it->kind==TK_IT_IMPL?"impl":it->kind==TK_IT_EFFECT?"effect":"handler");
        }
    }
    fprintf(out,"\nint main(int argc, char **argv){ tk_g_argc = argc; tk_g_argv = argv; taka_main(); return tk_g_exit; }\n");
    // main mapping: taka `fn main()` has no args and may return void/i32; wrap.
    fclose(out);
    int rc = cg_failed ? 1 : 0;
    cg_free_enums();
    cg_free_tables();
    free(cg_vars); cg_vars = NULL; cg_var_n = cg_var_cap = cg_var_fn_base = 0; cg_depth = 0;
    // drop the whole scratch working set in one free (no per-node bookkeeping)
    arena_destroy(cg_scratch); cg_scratch = NULL;
    return rc;
}
