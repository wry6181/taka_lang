#pragma once
// AST for taka (grammar.ebnf rev 4). Arena-allocated. Covers full grammar;
// backend may initially support a runnable core subset.
#include "types.h"
#include "lexer.h"

typedef struct TkType TkType;
typedef struct TkExpr TkExpr;
typedef struct TkStmt TkStmt;
typedef struct TkPattern TkPattern;
typedef struct TkItem TkItem;

typedef struct {
    char *name; // arena string, NULL for anonymous
    TkType *bound; // not used; bounds are paths
    char **plus_paths; // trait bound paths as strings, simplified
    i32 plus_count;
} TkBoundSeg;

typedef struct {
    char *seg; // single path segment
} TkPathSeg;

typedef struct {
    TkPathSeg *segs;
    i32 count;
} TkPath;

typedef enum {
    TK_EFF_BUILTIN, // io/alloc/reads/...
    TK_EFF_PATH,    // custom effect path + generic args
} TkEffItemKind;

typedef struct {
    TkEffItemKind kind;
    TokenKind builtin; // if BUILTIN
    TkPath path;       // if PATH
    TkType **gargs;
    i32 gcount;
} TkEffItem;

typedef struct {
    TkEffItem *items;
    i32 count;
    char *tail; // `| e` row variable, NULL if none
} TkEffectRow;

typedef enum {
    TK_TY_PRIM,   // i8..never
    TK_TY_PATH,   // List<T>, Circle, Self::X
    TK_TY_FN,     // fn(T) <eff> -> U, extern fn
    TK_TY_DYN,    // dyn Trait
    TK_TY_TUPLE,  // (A, B)
    TK_TY_ARRAY,  // [T; N]
    TK_TY_AUTO,   // auto / const auto
} TkTypeKind;

struct TkType {
    TkTypeKind kind;
    b32 is_const;
    i32 stars;
    b32 is_ref; // trailing &
    TokenKind prim; // if PRIM
    TkPath path;    // if PATH/DYN
    TkType **gargs; i32 gcount; // PATH generic args
    // FN:
    b32 is_extern_fn;
    TkType **params; i32 pcount;
    TkEffectRow *eff; // may be NULL
    TkType *ret;      // may be NULL (void)
    // TUPLE:
    TkType **elems; i32 ecount;
    // ARRAY:
    TkType *elem;
    TkExpr *arr_n; // const_expr
};

typedef struct {
    char *name; // generic param name, or effect var
    b32 is_effect_var;
    char *bound_str; // raw bound text (sema resolves); simplified
} TkGenericParam;

typedef struct {
    TkGenericParam *params;
    i32 count;
} TkGenerics;

typedef struct {
    char *param;
    char *bound_str;
} TkWhereEntry;

typedef struct {
    TkWhereEntry *entries;
    i32 count;
} TkWhere;

// ---------- patterns ----------
typedef enum {
    TK_PAT_WILD,
    TK_PAT_REF,     // &x
    TK_PAT_LIT,     // literal token copy
    TK_PAT_BIND,    // [mut] name
    TK_PAT_PATH,    // path [(pats)] [{field pats}] | [= const]
} TkPatKind;

struct TkPattern {
    TkPatKind kind;
    Token tok; // for LIT (original token)
    b32 is_mut; // BIND
    char *name; // REF (bound name), BIND, PATH base name via path
    TkPath path; // PATH
    TkPattern **args; i32 acount; // tuple variant
    struct { char *field; TkPattern *pat; } *fields; i32 fcount;
    b32 has_dotdot;
    TkPattern **or_next; // `a | b` chain: store as list
    i32 or_count;
};

// ---------- expressions ----------
typedef enum {
    TK_EX_INT, TK_EX_FLOAT, TK_EX_STRING, TK_EX_CHAR,
    TK_EX_BOOL, TK_EX_NULL, TK_EX_UNDEF,
    TK_EX_VAR,      // path [+ turbofish types]
    TK_EX_PAREN,
    TK_EX_STRUCT_LIT, // path { field: expr }
    TK_EX_ARRAY_LIST, TK_EX_ARRAY_FILL,
    TK_EX_BRACE,    // bare { ... } compat (stmts+trailing)
    TK_EX_LAMBDA,
    TK_EX_SIZEOF, TK_EX_OFFSETOF,
    TK_EX_IF, TK_EX_MATCH,
    TK_EX_PERFORM, TK_EX_HANDLE, TK_EX_SPAWN, TK_EX_RESUME, TK_EX_TRANSACT,
    TK_EX_UNARY, TK_EX_BINARY, TK_EX_CAST, TK_EX_TERNARY,
    TK_EX_CALL, TK_EX_INDEX, TK_EX_FIELD,
    TK_EX_POST_INC, TK_EX_POST_DEC,
    TK_EX_BLOCK, // block as expression (trailing value)
} TkExprKind;

struct TkExpr {
    TkExprKind kind;
    Token tok; // primary token where useful
    TkPath path; // VAR / STRUCT_LIT base / PERFORM base
    TkType **targs; i32 tcount; // turbofish
    TkExpr *lhs, *rhs, *cond, *then_e, *else_e;
    TokenKind op; // UNARY/BINARY/assign-ish
    TkType *cast_to; // CAST
    // call/index/field
    TkExpr *callee, *index;
    char *field;
    TkExpr **args; i32 nargs;
    // struct lit
    struct { char *field; TkExpr *e; } *sfields; i32 scount;
    // array
    TkExpr **elems; i32 ecount;
    TkExpr *fill_elem, *fill_n;
    // brace/block
    TkStmt **stmts; i32 scount2;
    TkExpr *trailing;
    // lambda
    struct { b32 by_ref; b32 is_mut; char *name; TkExpr *init; } *caps; i32 ccount;
    struct { b32 is_mut; TkType *ty; char *name; } *lparams; i32 lpcount;
    TkEffectRow *leff;
    TkType *lret;
    TkStmt **lbody_stmts; i32 lbody_n; TkExpr *lbody_trail;
    TkExpr *lbody_expr; // for => expr
    b32 lbody_is_block;
    // sizeof/offsetof
    b32 sizeof_is_type; TkType *sizeof_ty; TkExpr *sizeof_ex;
    TkType *off_ty; char *off_field; TkExpr *off_ex_bare; b32 off_bare;
    // if
    struct { TkExpr *cond_if; TkStmt **s; i32 n; TkExpr *t; } *if_br; // simplified: use stmts below
    TkExpr *if_cond; TkStmt **if_then; i32 if_then_n; TkExpr *if_then_trail;
    struct { TkExpr *c; TkStmt **s; i32 n; TkExpr *t; } *elseifs; i32 eicount;
    TkStmt **else_s; i32 else_n; TkExpr *else_trail; b32 has_else;
    // match
    TkExpr *scrut;
    struct { TkPattern *pat; TkExpr *guard; TkExpr *body; TkStmt **bs; i32 bn; TkExpr *bt; b32 body_is_block; } *arms; i32 arm_n;
    // perform/handle/spawn/resume/transact
    TkExpr **pargs; i32 pargc;
    TkExpr *h_scrut; TkStmt **h_blk; i32 h_blk_n; TkExpr *h_blk_trail; b32 h_scrut_is_block;
    char *h_path_str; TkPath h_path; b32 h_inline; // inline body
    struct { char *op; struct { b32 is_mut; TkType *ty; char *nm; } *ps; i32 pn; TkExpr *b; TkStmt **bs2; i32 bn2; TkExpr *bt2; b32 bisblk; b32 is_return; } *hops; i32 hop_n;
    TkExpr *resume_e; b32 has_resume_e;
    TkStmt **t_blk; i32 t_n; TkExpr *t_trail;
};

// ---------- statements ----------
typedef enum {
    TK_ST_DECL, TK_ST_SIMPLE, TK_ST_BLOCK, TK_ST_MATCH_STMT, TK_ST_HANDLE_STMT, TK_ST_TRANSACT_STMT,
    TK_ST_IF, TK_ST_WHILE, TK_ST_FOR_C, TK_ST_FOR_EACH,
    TK_ST_DEFER, TK_ST_BREAK, TK_ST_CONTINUE, TK_ST_RETURN, TK_ST_LABEL,
    TK_ST_SPAWN_BLK, TK_ST_SPAWN_CALL,
} TkStmtKind;

struct TkStmt {
    TkStmtKind kind;
    // DECL: shared type for `a=.., b=..`
    b32 is_mut;
    TkType *ty;
    char **names; TkExpr **inits; i32 dcount;
    // SIMPLE: expr [assign_op expr]
    TkExpr *e1, *e2; TokenKind assign_op; b32 has_assign;
    // BLOCK
    TkStmt **stmts; i32 n; TkExpr *trailing; b32 has_trailing;
    // IF/WHILE cond
    TkExpr *cond; b32 cond_is_failable; b32 cond_f_mut; TkPattern *cond_pat; TkExpr *cond_ex;
    TkStmt **then_s; i32 then_n; TkExpr *then_t;
    struct { TkExpr *c; b32 f; b32 fm; TkPattern *p; TkExpr *e; TkStmt **s; i32 n; TkExpr *t; } *eifs; i32 eif_n;
    TkStmt **els; i32 els_n; TkExpr *els_t; b32 has_els;
    // FOR_C
    TkStmt *for_init_decl; TkStmt *for_init_simple; b32 for_init_empty; b32 for_init_is_decl;
    TkExpr *for_cond; b32 has_for_cond;
    TkStmt *for_incr; b32 has_incr;
    // FOR_EACH
    b32 fe_auto; TkPattern *fe_pat; TkExpr *fe_ex;
    // MATCH_STMT: scrutinee + arms with block bodies (bindings scoped per arm)
    TkExpr *m_scrut;
    struct { TkPattern *pat; TkExpr *guard; TkStmt **bs; i32 bn; } *m_arms; i32 m_arm_n;
    // DEFER
    TkStmt *defer_simple; TkStmt **defer_blk; i32 defer_n; TkExpr *defer_t; b32 defer_is_block;
    // BREAK/CONTINUE label
    char *label;
    // RETURN
    TkExpr *ret_e; b32 has_ret;
    // LABEL
    char *lab; TkStmt *lab_s;
    // SPAWN
    TkExpr *sp_call; TkStmt **sp_blk; i32 sp_n; TkExpr *sp_t;
};

// ---------- items ----------
typedef enum {
    TK_IT_IMPORT, TK_IT_MOD, TK_IT_FN, TK_IT_EXTERN, TK_IT_STRUCT, TK_IT_ENUM,
    TK_IT_EFFECT, TK_IT_HANDLER, TK_IT_TRAIT, TK_IT_IMPL, TK_IT_ALIAS, TK_IT_GLOBAL,
} TkItemKind;

struct TkItem {
    TkItemKind kind;
    b32 is_pub, is_priv;
    TkPath path; // import/mod
    // FN
    char *fn_name;
    TkGenerics generics;
    struct { b32 is_mut; TkType *ty; char *nm; } *params; i32 pcount;
    TkEffectRow *eff;
    TkType *ret;
    TkWhere where;
    b32 is_proto;
    TkStmt **body; i32 bcount; TkExpr *trail; // def
    char *extern_str; // extern "C"?
    // STRUCT
    char *sname;
    struct { b32 is_pub, is_priv, is_mut; TkType *ty; char *nm; } *fields; i32 fcount;
    // ENUM
    char *ename;
    struct { b32 is_pub, is_priv; char *vn; b32 has_tuple; TkType **ts; i32 tn; b32 has_struct; struct { TkType *ty; char *nm; } *fs; i32 fn2; b32 has_disc; TkExpr *disc; } *vars; i32 vcount;
    // EFFECT
    char *eff_name;
    struct { b32 is_pub, is_priv; char *op; struct { b32 is_mut; TkType *ty; char *nm; } *ps; i32 pn; TkType *rt; } *ops; i32 opcount;
    // HANDLER
    char *hname; TkPath for_path; TkEffectRow *for_eff;
    struct { b32 is_ret; char *op; struct { b32 is_mut; TkType *ty; char *nm; } *ps; i32 pn; TkType *rt_param; char *rt_nm; TkExpr *b; TkStmt **bs; i32 bn; TkExpr *bt; b32 bisblk; } *hops; i32 hopcount;
    // TRAIT
    char *tname; TkBoundSeg *sup; i32 sup_n;
    TkItem **methods; i32 mcount; // protos
    // IMPL
    TkPath impl_path; TkType **impl_gargs; i32 impl_gn; TkType *impl_ty;
    TkItem **fns; i32 fncount;
    // ALIAS
    char *aname; TkType *aty;
    // GLOBAL
    TkStmt *gdecl; // DECL
};

typedef struct {
    TkItem **items;
    i32 count;
} TkProgram;
