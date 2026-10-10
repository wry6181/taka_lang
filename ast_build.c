#include "ast_build.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { Token *t; int n, pos; mem_arena *a; const char *err; int pend_gt; } B;

static Token *peek(B*b){ return &b->t[b->pos]; }
// Growable append for builder arrays (fixed caps silently corrupt on overflow;
// a self-hosted compiler has 100s of items/stmts). Arrays start at cap 8.
#define BAPPEND(b, arr, n, cap, val) do { \
    if ((n) >= (cap)) { \
        u64 ncap_ = (cap) ? (u64)(cap) * 2 : 8; \
        typeof(arr) narr_ = (typeof(arr))arena_push((b)->a, ncap_ * sizeof(*(arr)), 1); \
        if (!narr_) { (b)->err = "OOM"; break; } \
        if ((n)) memcpy(narr_, (arr), (size_t)(n) * sizeof(*(arr))); \
        (arr) = narr_; (cap) = (int)ncap_; \
    } \
    (arr)[(n)++] = (val); \
} while (0)
static TokenKind pk(B*b){ if(b->pend_gt>0) return TOKEN_GT; return b->t[b->pos].kind; }
// consume one '>' (splits lexer `>>` for nested List<List<..>> closes)
static int match_gt(B*b){
    if(b->pend_gt>0){ b->pend_gt--; return 1; }
    if(b->t[b->pos].kind==TOKEN_GT){ b->pos++; return 1; }
    if(b->t[b->pos].kind==TOKEN_SHR){ b->pos++; b->pend_gt=1; return 1; }
    return 0;
}
static int match(B*b,TokenKind k){ if(pk(b)==k){b->pos++;return 1;} return 0; }
static Token *expect(B*b,TokenKind k,const char *w){
    if(!match(b,k)){ static char buf[256]; snprintf(buf,sizeof buf,"expected %s at %d:%d",w,peek(b)->line,peek(b)->col); b->err=buf; return NULL; }
    return &b->t[b->pos-1];
}
static char *dupstr(B*b,const char*s,int n){
    char*o=(char*)arena_push(b->a,(u64)n+1,0); memcpy(o,s,(size_t)n); o[n]=0; return o;
}
static char *tokstr(B*b,Token*t){ return dupstr(b,t->start,(int)t->len); }
static void *push(B*b,int sz){ void*p=arena_push(b->a,(u64)sz,1); if(!p){ fprintf(stderr,"out of memory\n"); exit(1); } return p; }

static TkType *b_type(B*b);
static TkExpr *b_expr(B*b);
static TkStmt *b_stmt(B*b);
static TkPattern *b_pattern(B*b);
static void b_block_body(B*b,TkStmt***ss,int*nn,TkExpr**tr);

static int is_prim(TokenKind k){
    return k==TOKEN_I8||k==TOKEN_I16||k==TOKEN_I32||k==TOKEN_I64||k==TOKEN_U8||k==TOKEN_U16||k==TOKEN_U32||k==TOKEN_U64||k==TOKEN_F32||k==TOKEN_F64||k==TOKEN_CHAR_TY||k==TOKEN_BOOL||k==TOKEN_SVIEW||k==TOKEN_NEVER;
}
static TkType *b_type(B*b){
    TkType*t=(TkType*)push(b,sizeof *t); if(!t) return NULL;
    if(match(b,TOKEN_CONST)) t->is_const=1;
    TokenKind k=pk(b);
    if(k==TOKEN_AUTO){ b->pos++; t->kind=TK_TY_AUTO; }
    else if(is_prim(k)){ b->pos++; t->kind=TK_TY_PRIM; t->prim=k; }
    else if(k==TOKEN_IDENT||k==TOKEN_SELF){
        t->kind=TK_TY_PATH; t->path.count=0;
        int segcap=0; t->path.segs=NULL;
        { TkPathSeg s0; s0.seg=tokstr(b,peek(b)); BAPPEND(b, t->path.segs, t->path.count, segcap, s0); b->pos++; }
        while(match(b,TOKEN_COLON2)){ Token*s=expect(b,TOKEN_IDENT,"path seg"); if(!s)return NULL;
            TkPathSeg s2; s2.seg=tokstr(b,s); BAPPEND(b, t->path.segs, t->path.count, segcap, s2); if(b->err)return NULL; }
        if(pk(b)==TOKEN_LT){
            // only builtin List<T> is monomorphized in core (single type arg)
            if(!(t->path.count==1 && strcmp(t->path.segs[0].seg,"List")==0)){
                b->err="only List<T> generics supported in core builder (validation OK)"; return NULL;
            }
            b->pos++; // <
            TkType*elem=b_type(b); if(!elem||b->err)return NULL;
            if(match(b,TOKEN_COMMA)){ b->err="List takes exactly one type arg"; return NULL; }
            if(!match_gt(b)){ b->err="expected '>' for List<T>"; return NULL; }
            t->gargs=(TkType**)arena_push(b->a,sizeof(TkType*),1);
            t->gargs[0]=elem; t->gcount=1;
        }
    } else { b->err="expected type"; return NULL; }
    while(match(b,TOKEN_STAR)) t->stars++;
    if(match(b,TOKEN_AMP)) t->is_ref=1;
    return t;
}

static TkExpr *new_expr(B*b,TkExprKind k){ TkExpr*e=(TkExpr*)push(b,sizeof *e); if(e)e->kind=k; return e; }
static TkExpr *b_primary(B*b){
    TokenKind k=pk(b);
    if(k==TOKEN_MATCH){
        // match expression: `match [ ( ] scrut [ ) ] { pat [if g] => expr [,] }`
        // (block arms belong to match statements; a bare `{` arm is an error here)
        b->pos++;
        TkExpr*scr=NULL;
        if(match(b,TOKEN_LPAREN)){ scr=b_expr(b); if(!scr||b->err)return NULL; if(!expect(b,TOKEN_RPAREN,"')'"))return NULL; }
        else { scr=b_expr(b); if(!scr||b->err)return NULL; }
        if(!expect(b,TOKEN_LBRACE,"'{'"))return NULL;
        TkExpr*m=new_expr(b,TK_EX_MATCH); m->scrut=scr;
        int armcap=0; m->arms=NULL; m->arm_n=0;
        if(pk(b)!=TOKEN_RBRACE){ for(;;){
            TkPattern*pat=b_pattern(b); if(!pat||b->err)return NULL;
            TkExpr*guard=NULL;
            if(pk(b)==TOKEN_IF){ b->pos++; guard=b_expr(b); if(!guard||b->err)return NULL; }
            if(!expect(b,TOKEN_FATARROW,"'=>'"))return NULL;
            if(pk(b)==TOKEN_LBRACE){ b->err="block arms need a match statement (=> {...}); match expressions take expr arms"; return NULL; }
            TkExpr*body=b_expr(b); if(!body||b->err)return NULL;
            typeof(*m->arms) arm; arm.pat=pat; arm.guard=guard;
            arm.body=body; arm.body_is_block=0;
            arm.bs=NULL; arm.bn=0; arm.bt=NULL;
            BAPPEND(b, m->arms, m->arm_n, armcap, arm); if(b->err)return NULL;
            if(!match(b,TOKEN_COMMA))break; if(pk(b)==TOKEN_RBRACE)break;
        } }
        if(!expect(b,TOKEN_RBRACE,"'}'"))return NULL;
        return m;
    }
    if(k==TOKEN_INT){ TkExpr*e=new_expr(b,TK_EX_INT); e->tok=*peek(b); b->pos++; return e; }
    if(k==TOKEN_FLOAT){ TkExpr*e=new_expr(b,TK_EX_FLOAT); e->tok=*peek(b); b->pos++; return e; }
    if(k==TOKEN_STRING){ TkExpr*e=new_expr(b,TK_EX_STRING); e->tok=*peek(b); b->pos++; return e; }
    if(k==TOKEN_CHAR){ TkExpr*e=new_expr(b,TK_EX_CHAR); e->tok=*peek(b); b->pos++; return e; }
    if(k==TOKEN_TRUE||k==TOKEN_FALSE){ TkExpr*e=new_expr(b,TK_EX_BOOL); e->tok=*peek(b); b->pos++; return e; }
    if(k==TOKEN_NULL){ TkExpr*e=new_expr(b,TK_EX_NULL); b->pos++; return e; }
    if(k==TOKEN_LBRACK){
        b->pos++; // [
        TkExpr*e=new_expr(b,TK_EX_ARRAY_LIST);
        int elcap=0; e->elems=NULL; e->ecount=0;
        if(pk(b)!=TOKEN_RBRACK){ for(;;){ TkExpr*a=b_expr(b); if(!a||b->err)return NULL;
            if(match(b,TOKEN_SEMI)){ b->err="array-fill [x; N] TODO in core (use [a, b, ...])"; return NULL; }
            BAPPEND(b, e->elems, e->ecount, elcap, a); if(b->err)return NULL;
            if(!match(b,TOKEN_COMMA))break; if(pk(b)==TOKEN_RBRACK)break; } }
        if(!expect(b,TOKEN_RBRACK,"']'"))return NULL;
        return e;
    }
    if(k==TOKEN_LPAREN){ b->pos++; TkExpr*e=b_expr(b); if(!e)return NULL; if(!expect(b,TOKEN_RPAREN,"')'"))return NULL; TkExpr*p=new_expr(b,TK_EX_PAREN); p->lhs=e; return p; }
    if(k==TOKEN_IDENT||k==TOKEN_SELF){
        TkPath pth; pth.count=0; int pcap=0; pth.segs=NULL;
        { TkPathSeg s0; s0.seg=tokstr(b,peek(b)); BAPPEND(b, pth.segs, pth.count, pcap, s0); if(b->err)return NULL; b->pos++; }
        while(match(b,TOKEN_COLON2)){ Token*ps=expect(b,TOKEN_IDENT,"path seg"); if(!ps)return NULL;
            TkPathSeg sq; sq.seg=tokstr(b,ps); BAPPEND(b, pth.segs, pth.count, pcap, sq); if(b->err)return NULL; }
        if(pk(b)==TOKEN_LBRACE){
            // struct lit? require IDENT ':' (else leave for caller? in expr `Point {x:..}` always lit)
            // peek: IDENT COLON? if next two are IDENT COLON or RBRACE(empty), treat as lit
            int save=b->pos;
            b->pos++; // {
            int is_lit=0;
            if(pk(b)==TOKEN_RBRACE) is_lit=1;
            else if(pk(b)==TOKEN_IDENT && b->pos+1<b->n && b->t[b->pos+1].kind==TOKEN_COLON) is_lit=1;
            b->pos=save;
            if(is_lit){
                TkExpr*e=new_expr(b,TK_EX_STRUCT_LIT); e->path=pth; b->pos++; // {
                int sfcap=0; e->sfields=NULL; e->scount=0;
                // cap 16
                e->sfields=(typeof(e->sfields))arena_push(b->a,sizeof(*e->sfields)*16,1);
                if(pk(b)!=TOKEN_RBRACE){
                    for(;;){
                        Token*fn=expect(b,TOKEN_IDENT,"field"); if(!fn)return NULL;
                        if(!expect(b,TOKEN_COLON,"':'"))return NULL;
                        TkExpr*fe=b_expr(b); if(!fe)return NULL;
                        typeof(*e->sfields) sf; sf.field=tokstr(b,fn); sf.e=fe;
                        BAPPEND(b, e->sfields, e->scount, sfcap, sf); if(b->err)return NULL;
                        if(!match(b,TOKEN_COMMA))break;
                        if(pk(b)==TOKEN_RBRACE)break;
                    }
                }
                if(!expect(b,TOKEN_RBRACE,"'}'"))return NULL;
                return e;
            }
        }
        TkExpr*e=new_expr(b,TK_EX_VAR); e->path=pth; return e;
    }
    b->err="expected expression"; return NULL;
}
static TkExpr *b_postfix(B*b){
    TkExpr*e=b_primary(b); if(!e)return NULL;
    for(;;){
        if(match(b,TOKEN_LPAREN)){
            TkExpr*c=new_expr(b,TK_EX_CALL); c->callee=e;
            int acap=0; c->args=NULL; c->nargs=0;
            if(pk(b)!=TOKEN_RPAREN){ for(;;){ TkExpr*a=b_expr(b); if(!a)return NULL; BAPPEND(b, c->args, c->nargs, acap, a); if(b->err)return NULL; if(!match(b,TOKEN_COMMA))break; if(pk(b)==TOKEN_RPAREN)break; } }
            if(!expect(b,TOKEN_RPAREN,"')'"))return NULL;
            e=c;
        } else if(match(b,TOKEN_LBRACK)){ TkExpr*ix=b_expr(b); if(!ix)return NULL; if(!expect(b,TOKEN_RBRACK,"']'"))return NULL; TkExpr*n=new_expr(b,TK_EX_INDEX); n->callee=e; n->index=ix; e=n; }
        else if(match(b,TOKEN_DOT)){ Token*f=expect(b,TOKEN_IDENT,"field"); if(!f)return NULL; TkExpr*n=new_expr(b,TK_EX_FIELD); n->callee=e; n->field=tokstr(b,f); e=n; }
        else if(match(b,TOKEN_PLUSPLUS)){ TkExpr*n=new_expr(b,TK_EX_POST_INC); n->lhs=e; e=n; }
        else if(match(b,TOKEN_MINUSMINUS)){ TkExpr*n=new_expr(b,TK_EX_POST_DEC); n->lhs=e; e=n; }
        else break;
    }
    return e;
}
static TkExpr *b_unary(B*b){
    TokenKind k=pk(b);
    if(k==TOKEN_PLUS||k==TOKEN_MINUS||k==TOKEN_BANG||k==TOKEN_TILDE||k==TOKEN_STAR||k==TOKEN_AMP||k==TOKEN_PLUSPLUS||k==TOKEN_MINUSMINUS){
        // && prefix (two &) not in demo core (no refs of refs); treat AMP2 as error
        if(k==TOKEN_AMP && b->pos+0<b->n){ /* single & */ }
        b->pos++; TkExpr*e=new_expr(b,TK_EX_UNARY); e->op=k; e->lhs=b_unary(b); if(!e->lhs)return NULL; return e;
    }
    if(k==TOKEN_AMP2){ b->err="'&&' prefix not in core builder"; return NULL; }
    return b_postfix(b);
}
static TkExpr *b_cast(B*b){ TkExpr*e=b_unary(b); if(!e)return NULL; while(match(b,TOKEN_AS)){ TkType*t=b_type(b); if(!t)return NULL; TkExpr*n=new_expr(b,TK_EX_CAST); n->lhs=e; n->cast_to=t; e=n; } return e; }
static int prec(TokenKind k){
    switch(k){case TOKEN_STAR:case TOKEN_SLASH:case TOKEN_PERCENT:return 70;case TOKEN_PLUS:case TOKEN_MINUS:return 60;case TOKEN_SHL:case TOKEN_SHR:return 50;case TOKEN_AMP:return 40;case TOKEN_CARET:return 30;case TOKEN_PIPE:return 20;case TOKEN_EQEQ:case TOKEN_BANGEQ:case TOKEN_LT:case TOKEN_GT:case TOKEN_LTEQ:case TOKEN_GTEQ:return 15;case TOKEN_AMP2:return 10;case TOKEN_PIPE2:return 5;default:return -1;}
}
static TkExpr *b_bin(B*b,int mp){
    TkExpr*e=b_cast(b); if(!e)return NULL;
    for(;;){ TokenKind k=pk(b); int p=prec(k); if(p<mp)break; b->pos++; TkExpr*r=b_bin(b,p+1); if(!r)return NULL; TkExpr*n=new_expr(b,TK_EX_BINARY); n->op=k; n->lhs=e; n->rhs=r; e=n; }
    return e;
}
static TkExpr *b_expr(B*b){
    TkExpr*e=b_bin(b,0); if(!e)return NULL;
    if(match(b,TOKEN_QMARK)){ TkExpr*t=b_expr(b); if(!t)return NULL; if(!expect(b,TOKEN_COLON,"':'"))return NULL; TkExpr*f=b_expr(b); if(!f)return NULL; TkExpr*n=new_expr(b,TK_EX_TERNARY); n->cond=e; n->then_e=t; n->else_e=f; e=n; }
    return e;
}

static int is_assign(TokenKind k){ return k==TOKEN_EQ||k==TOKEN_PLUS_EQ||k==TOKEN_MINUS_EQ||k==TOKEN_STAR_EQ||k==TOKEN_SLASH_EQ||k==TOKEN_PERCENT_EQ||k==TOKEN_SHL_EQ||k==TOKEN_SHR_EQ||k==TOKEN_AMP_EQ||k==TOKEN_PIPE_EQ||k==TOKEN_CARET_EQ; }

static TkPattern *new_pat(B*b,TkPatKind k){ TkPattern*p=(TkPattern*)push(b,sizeof *p); if(p)p->kind=k; return p; }
static TkPattern *b_pat_primary(B*b);
static TkPattern *b_pattern(B*b){
    TkPattern*p=b_pat_primary(b); if(!p||b->err)return NULL;
    if(pk(b)!=TOKEN_PIPE) return p;
    TkPattern **extra=NULL;
    int n=0, xcap=0;
    while(match(b,TOKEN_PIPE)){
        TkPattern*q=b_pat_primary(b); if(!q||b->err)return NULL;
        BAPPEND(b, extra, n, xcap, q); if(b->err)return NULL;
    }
    p->or_next=extra; p->or_count=n;
    return p;
}
static TkPattern *b_pat_primary(B*b){
    TokenKind k=pk(b);
    if(k==TOKEN_WILDCARD){ b->pos++; return new_pat(b,TK_PAT_WILD); }
    if(k==TOKEN_AMP){ b->pos++; Token*nm=expect(b,TOKEN_IDENT,"name after &"); if(!nm)return NULL;
        TkPattern*p=new_pat(b,TK_PAT_REF); p->name=tokstr(b,nm); return p; }
    if(k==TOKEN_INT||k==TOKEN_FLOAT||k==TOKEN_STRING||k==TOKEN_CHAR||
       k==TOKEN_TRUE||k==TOKEN_FALSE||k==TOKEN_NULL||k==TOKEN_UNDEF){
        TkPattern*p=new_pat(b,TK_PAT_LIT); p->tok=*peek(b); b->pos++; return p; }
    if(k==TOKEN_MUT){
        b->pos++; Token*nm=expect(b,TOKEN_IDENT,"name after mut"); if(!nm)return NULL;
        if(pk(b)==TOKEN_COLON2||pk(b)==TOKEN_LPAREN||pk(b)==TOKEN_LBRACE){ b->err="'mut' only on plain bindings"; return NULL; }
        TkPattern*p=new_pat(b,TK_PAT_BIND); p->is_mut=1; p->name=tokstr(b,nm); return p; }
    if(k==TOKEN_IDENT||k==TOKEN_SELF){
        int save=b->pos;
        b->pos++;
        int is_path=0;
        if(pk(b)==TOKEN_COLON2) is_path=1;
        else if(pk(b)==TOKEN_LPAREN||pk(b)==TOKEN_LBRACE) is_path=1;
        else {
            int i=b->pos;
            while(i<b->n && b->t[i].kind==TOKEN_COLON2 && i+1<b->n &&
                  (b->t[i+1].kind==TOKEN_IDENT||b->t[i+1].kind==TOKEN_SELF)) i+=2;
            if(i!=b->pos) is_path=1;
            else if(i<b->n && (b->t[i].kind==TOKEN_LPAREN||b->t[i].kind==TOKEN_LBRACE)) is_path=1;
        }
        b->pos=save;
        if(!is_path){
            Token*nm=peek(b); b->pos++;
            TkPattern*p=new_pat(b,TK_PAT_BIND); p->is_mut=0; p->name=tokstr(b,nm); return p;
        }
        TkPattern*p=new_pat(b,TK_PAT_PATH);
        p->path.count=0; int pcap=0; p->path.segs=NULL;
        { TkPathSeg s0; s0.seg=tokstr(b,peek(b)); BAPPEND(b, p->path.segs, p->path.count, pcap, s0); if(b->err)return NULL; b->pos++; }
        while(match(b,TOKEN_COLON2)){ Token*s=expect(b,TOKEN_IDENT,"path seg"); if(!s)return NULL;
            TkPathSeg sq; sq.seg=tokstr(b,s); BAPPEND(b, p->path.segs, p->path.count, pcap, sq); if(b->err)return NULL; }
        if(match(b,TOKEN_LPAREN)){
            int agcap=0; p->args=NULL; p->acount=0;
            if(pk(b)!=TOKEN_RPAREN){ for(;;){ TkPattern*q=b_pattern(b); if(!q||b->err)return NULL;
                BAPPEND(b, p->args, p->acount, agcap, q); if(b->err)return NULL;
                if(!match(b,TOKEN_COMMA))break; if(pk(b)==TOKEN_RPAREN)break; } }
            if(!expect(b,TOKEN_RPAREN,"')'"))return NULL;
        } else if(match(b,TOKEN_LBRACE)){
            int fcap=0; p->fields=NULL; p->fcount=0; p->has_dotdot=0;
            if(pk(b)!=TOKEN_RBRACE){ for(;;){
                if(match(b,TOKEN_DOTDOT)){ p->has_dotdot=1; match(b,TOKEN_COMMA); break; }
                Token*fn=expect(b,TOKEN_IDENT,"field"); if(!fn)return NULL;
                TkPattern*sub=NULL;
                if(match(b,TOKEN_COLON)){ sub=b_pattern(b); if(!sub||b->err)return NULL; }
                else { sub=new_pat(b,TK_PAT_BIND); sub->is_mut=0; sub->name=tokstr(b,fn); }
                typeof(*p->fields) fp; fp.field=tokstr(b,fn); fp.pat=sub;
                BAPPEND(b, p->fields, p->fcount, fcap, fp); if(b->err)return NULL;
                if(!match(b,TOKEN_COMMA))break; if(pk(b)==TOKEN_RBRACE)break;
            } }
            if(!expect(b,TOKEN_RBRACE,"'}'"))return NULL;
        }
        return p;
    }
    b->err="expected pattern"; return NULL;
}

static void b_block_body(B*b,TkStmt***ss,int*nn,TkExpr**tr){
    int cap=0; *ss=NULL; *nn=0; *tr=NULL;
    while(pk(b)!=TOKEN_RBRACE && pk(b)!=TOKEN_EOF){
        // trailing? if no ';' before '}' at depth0 and next is expr-like, treat rest as trailing? Simplified: try stmt; if next is RBRACE and last was expr without ';', handled by caller? For demo (explicit returns, no trailing), just parse stmts.
        TkStmt*s=b_stmt(b); if(!s||b->err) return;
        BAPPEND(b, *ss, *nn, cap, s); if(b->err) return;
    }
}

static TkStmt *new_stmt(B*b,TkStmtKind k){ TkStmt*s=(TkStmt*)push(b,sizeof *s); if(s)s->kind=k; return s; }

static TkStmt *b_stmt(B*b){
    TokenKind k=pk(b);
    if(k==TOKEN_LBRACE){ b->pos++; TkStmt*s=new_stmt(b,TK_ST_BLOCK); TkStmt**ss=NULL;int n=0,cap=0;TkExpr*t; // nested block
        while(pk(b)!=TOKEN_RBRACE){ TkStmt*q=b_stmt(b); if(!q||b->err)return NULL; BAPPEND(b, ss, n, cap, q); if(b->err)return NULL; }
        expect(b,TOKEN_RBRACE,"'}'"); s->stmts=ss; s->n=n; return s; }
    if(k==TOKEN_IF){ b->pos++; if(!expect(b,TOKEN_LPAREN,"'('"))return NULL; TkExpr*c=b_expr(b); if(!c||b->err)return NULL; if(!expect(b,TOKEN_RPAREN,"')'"))return NULL;
        if(!expect(b,TOKEN_LBRACE,"'{'"))return NULL; TkStmt*s=new_stmt(b,TK_ST_IF); s->cond=c; s->cond_ex=c;
        TkStmt**ss;int n;TkExpr*t; b_block_body(b,&ss,&n,&t); if(b->err)return NULL; s->then_s=ss;s->then_n=n;s->then_t=t;
        if(!expect(b,TOKEN_RBRACE,"'}'"))return NULL;
        s->eif_n=0; s->has_els=0; int eifcap=0; s->eifs=NULL;
        while(match(b,TOKEN_ELSE)){ if(pk(b)==TOKEN_IF){ b->pos++; if(!expect(b,TOKEN_LPAREN,"'('"))return NULL; TkExpr*cc=b_expr(b); if(!cc)return NULL; if(!expect(b,TOKEN_RPAREN,"')'"))return NULL; if(!expect(b,TOKEN_LBRACE,"'{'"))return NULL;
                TkStmt**ss2;int n2;TkExpr*t2; b_block_body(b,&ss2,&n2,&t2); if(b->err)return NULL; if(!expect(b,TOKEN_RBRACE,"'}'"))return NULL;
                typeof(*s->eifs) ei; ei.c=cc; ei.e=cc; ei.s=ss2; ei.n=n2; ei.t=t2; ei.f=0; ei.fm=0; ei.p=NULL;
                BAPPEND(b, s->eifs, s->eif_n, eifcap, ei); if(b->err)return NULL;
            } else { if(!expect(b,TOKEN_LBRACE,"'{'"))return NULL; TkStmt**ss3;int n3;TkExpr*t3; b_block_body(b,&ss3,&n3,&t3); if(b->err)return NULL; if(!expect(b,TOKEN_RBRACE,"'}'"))return NULL; s->els=ss3;s->els_n=n3;s->els_t=t3;s->has_els=1; break; } }
        return s; }
    if(k==TOKEN_WHILE){ b->pos++; if(!expect(b,TOKEN_LPAREN,"'('"))return NULL; TkExpr*c=b_expr(b); if(!c)return NULL; if(!expect(b,TOKEN_RPAREN,"')'"))return NULL; if(!expect(b,TOKEN_LBRACE,"'{'"))return NULL;
        TkStmt*s=new_stmt(b,TK_ST_WHILE); s->cond=c; s->cond_ex=c; TkStmt**ss;int n;TkExpr*t; b_block_body(b,&ss,&n,&t); if(b->err)return NULL; if(!expect(b,TOKEN_RBRACE,"'}'"))return NULL; s->then_s=ss;s->then_n=n;s->then_t=t; return s; }
    if(k==TOKEN_FOR){ b->pos++; if(!expect(b,TOKEN_LPAREN,"'('"))return NULL;
        // only C-style for in core builder
        TkStmt*s=new_stmt(b,TK_ST_FOR_C);
        if(pk(b)!=TOKEN_SEMI){
            // decl or simple? check mut/type/ident = pattern (simplified: if MUT or (prim/ident + ...))
            int save=b->pos; if(pk(b)==TOKEN_MUT)save++;
            // crude: if starts with MUT/AUTO/CONST/prim/IDENT and contains '=' before ';', treat as decl if two tokens before '='? Use: try decl parse? Simplified: if MUT or prim or (IDENT and not single '='), attempt decl.
            // For demo `mut i32 i = 0` -> decl; `i = 0` -> simple.
            TokenKind f=b->t[save].kind;
            int isdecl=0;
            if(f==TOKEN_MUT||f==TOKEN_AUTO||f==TOKEN_CONST||is_prim(f)) isdecl=1;
            else if(f==TOKEN_IDENT){ // `List x =`? need 2 tokens? For `i = 0`, f=IDENT and next is '=', single -> simple. For `i32 i =`, f would be prim not IDENT. So IDENT-first is simple unless `IDENT IDENT =`? Not in demo. Keep simple.
                isdecl=0;
            }
            if(isdecl){
                // variable decl no semi
                int ismut=match(b,TOKEN_MUT);
                TkType*ty=b_type(b); if(!ty||b->err)return NULL;
                Token*nm=expect(b,TOKEN_IDENT,"name"); if(!nm)return NULL;
                if(!expect(b,TOKEN_EQ,"'='"))return NULL;
                TkExpr*in=b_expr(b); if(!in||b->err)return NULL;
                TkStmt*d=new_stmt(b,TK_ST_DECL); d->is_mut=ismut; d->ty=ty; d->names=(char**)arena_push(b->a,sizeof(char*)*2,1); d->inits=(TkExpr**)arena_push(b->a,sizeof(TkExpr*)*2,1); d->names[0]=tokstr(b,nm); d->inits[0]=in; d->dcount=1;
                s->for_init_decl=d; s->for_init_is_decl=1;
            } else {
                TkExpr*e1=b_expr(b); if(!e1||b->err)return NULL; TkStmt*q=new_stmt(b,TK_ST_SIMPLE); q->e1=e1;
                if(is_assign(pk(b))){ q->assign_op=pk(b); b->pos++; q->e2=b_expr(b); if(!q->e2||b->err)return NULL; q->has_assign=1; }
                s->for_init_simple=q; s->for_init_is_decl=0;
            }
        } else s->for_init_empty=1;
        if(!expect(b,TOKEN_SEMI,"';'"))return NULL;
        s->has_for_cond=0; if(pk(b)!=TOKEN_SEMI){ s->for_cond=b_expr(b); if(!s->for_cond||b->err)return NULL; s->has_for_cond=1; }
        if(!expect(b,TOKEN_SEMI,"';'"))return NULL;
        s->has_incr=0; if(pk(b)!=TOKEN_RPAREN){ TkExpr*e1=b_expr(b); if(!e1||b->err)return NULL; TkStmt*q=new_stmt(b,TK_ST_SIMPLE); q->e1=e1; if(is_assign(pk(b))){q->assign_op=pk(b);b->pos++;q->e2=b_expr(b);if(!q->e2||b->err)return NULL;q->has_assign=1;} s->for_incr=q; s->has_incr=1; }
        if(!expect(b,TOKEN_RPAREN,"')'"))return NULL;
        if(!expect(b,TOKEN_LBRACE,"'{'"))return NULL;
        TkStmt**ss;int n;TkExpr*t; b_block_body(b,&ss,&n,&t); if(b->err)return NULL; if(!expect(b,TOKEN_RBRACE,"'}'"))return NULL;
        s->then_s=ss;s->then_n=n;s->then_t=t; return s; }
    if(k==TOKEN_MATCH){
        b->pos++;
        TkExpr*scr=NULL;
        if(match(b,TOKEN_LPAREN)){ scr=b_expr(b); if(!scr||b->err)return NULL; if(!expect(b,TOKEN_RPAREN,"')'"))return NULL; }
        else { scr=b_expr(b); if(!scr||b->err)return NULL; }
        if(!expect(b,TOKEN_LBRACE,"'{'"))return NULL;
        TkStmt*s=new_stmt(b,TK_ST_MATCH_STMT); s->m_scrut=scr;
        int macap=0; s->m_arms=NULL; s->m_arm_n=0;
        if(pk(b)!=TOKEN_RBRACE){ for(;;){
            TkPattern*pat=b_pattern(b); if(!pat||b->err)return NULL;
            TkExpr*guard=NULL;
            if(pk(b)==TOKEN_IF){ b->pos++; guard=b_expr(b); if(!guard||b->err)return NULL; }
            if(!expect(b,TOKEN_FATARROW,"'=>'"))return NULL;
            if(pk(b)!=TOKEN_LBRACE){ b->err="match arms need block bodies in core (=> {...})"; return NULL; }
            b->pos++;
            TkStmt**ss;int n;TkExpr*t; b_block_body(b,&ss,&n,&t); if(b->err)return NULL;
            if(t){ b->err="trailing block values TODO in core"; return NULL; }
            if(!expect(b,TOKEN_RBRACE,"'}'"))return NULL;
            typeof(*s->m_arms) ma; ma.pat=pat; ma.guard=guard; ma.bs=ss; ma.bn=n;
            BAPPEND(b, s->m_arms, s->m_arm_n, macap, ma); if(b->err)return NULL;
            if(!match(b,TOKEN_COMMA))break; if(pk(b)==TOKEN_RBRACE)break;
        } }
        if(!expect(b,TOKEN_RBRACE,"'}'"))return NULL;
        return s; }
    if(k==TOKEN_RETURN){ b->pos++; TkStmt*s=new_stmt(b,TK_ST_RETURN); if(pk(b)!=TOKEN_SEMI){ s->ret_e=b_expr(b); if(!s->ret_e||b->err)return NULL; s->has_ret=1; } if(!expect(b,TOKEN_SEMI,"';'"))return NULL; return s; }
    if(k==TOKEN_BREAK){ b->pos++; TkStmt*s=new_stmt(b,TK_ST_BREAK); if(!expect(b,TOKEN_SEMI,"';'"))return NULL; return s; }
    if(k==TOKEN_CONTINUE){ b->pos++; TkStmt*s=new_stmt(b,TK_ST_CONTINUE); if(!expect(b,TOKEN_SEMI,"';'"))return NULL; return s; }
    // decl? `[mut] type IDENT =` (also `auto`)
    {
        int save=b->pos; int p=save;
        if(b->t[p].kind==TOKEN_MUT) p++;
        TokenKind f=b->t[p].kind;
        if(f==TOKEN_AUTO||f==TOKEN_CONST||is_prim(f)||f==TOKEN_IDENT||f==TOKEN_SELF){
            // need IDENT '=' after type (approx: find IDENT followed by '=' before ';' with no '.'/'('/'[')
            int angle=0,par=0,br=0,found=0;
            for(int i=p;i<b->n;i++){ TokenKind kk=b->t[i].kind;
                if(kk==TOKEN_LT)angle++; else if(kk==TOKEN_GT){if(angle>0)angle--;} else if(kk==TOKEN_LPAREN)par++; else if(kk==TOKEN_RPAREN){if(par>0)par--;else break;} else if(kk==TOKEN_LBRACK)br++; else if(kk==TOKEN_RBRACK){if(br>0)br--;} else if(kk==TOKEN_SEMI||kk==TOKEN_LBRACE||kk==TOKEN_RBRACE)break;
                if(angle==0&&par==0&&br==0){ if(kk==TOKEN_DOT){found=0;break;} if(kk==TOKEN_IDENT&&i+1<b->n&&(b->t[i+1].kind==TOKEN_EQ)){ found=(i>save); break; } } }
            if(found){
                int ismut=match(b,TOKEN_MUT);
                TkType*ty=b_type(b); if(!ty||b->err)return NULL;
                Token*nm=expect(b,TOKEN_IDENT,"name"); if(!nm)return NULL;
                if(!expect(b,TOKEN_EQ,"'='"))return NULL;
                TkExpr*in=b_expr(b); if(!in||b->err)return NULL;
                TkStmt*s=new_stmt(b,TK_ST_DECL); s->is_mut=ismut; s->ty=ty; s->names=(char**)arena_push(b->a,sizeof(char*)*2,1); s->inits=(TkExpr**)arena_push(b->a,sizeof(TkExpr*)*2,1); s->names[0]=tokstr(b,nm); s->inits[0]=in; s->dcount=1;
                if(!expect(b,TOKEN_SEMI,"';'"))return NULL; return s;
            }
        }
    }
    // simple `expr [assign expr] ;`
    {
        TkExpr*e1=b_expr(b); if(!e1||b->err)return NULL;
        TkStmt*s=new_stmt(b,TK_ST_SIMPLE); s->e1=e1;
        if(is_assign(pk(b))){ s->assign_op=pk(b); b->pos++; s->e2=b_expr(b); if(!s->e2||b->err)return NULL; s->has_assign=1; }
        if(!expect(b,TOKEN_SEMI,"';'"))return NULL; return s;
    }
}

TkProgram *ast_build_core(Token *toks,int count,mem_arena *arena,const char **err_out){
    B b={toks,count,0,arena,NULL};
    TkProgram*prog=(TkProgram*)arena_push(arena,sizeof *prog,1);
    int itemcap=0; prog->items=NULL; prog->count=0;
#define EMIT_ITEM(it) do { BAPPEND(&b, prog->items, prog->count, itemcap, it); if(b.err)break; } while(0)
    while(b.t[b.pos].kind!=TOKEN_EOF){
        TokenKind k=pk(&b);
        if(k==TOKEN_IMPORT){ b.pos++; // import path ;
            if(!(pk(&b)==TOKEN_IDENT||pk(&b)==TOKEN_SELF)){b.err="expected import path";break;}
            while(pk(&b)==TOKEN_IDENT||pk(&b)==TOKEN_SELF||pk(&b)==TOKEN_COLON2){ b.pos++; if(b.t[b.pos-1].kind==TOKEN_COLON2){ if(pk(&b)!=TOKEN_IDENT){b.err="bad import";break;} } else if(pk(&b)==TOKEN_COLON2) continue; else break; }
            if(!expect(&b,TOKEN_SEMI,"';'"))break; continue; }
        if(k==TOKEN_EXTERN){
            b.pos++; if(pk(&b)==TOKEN_STRING) b.pos++;
            if(!expect(&b,TOKEN_FN,"'fn'"))break;
            Token*nm=expect(&b,TOKEN_IDENT,"name"); if(!nm)break;
            if(!expect(&b,TOKEN_LPAREN,"'('"))break;
            TkItem*it=(TkItem*)arena_push(arena,sizeof *it,1); it->kind=TK_IT_EXTERN; it->fn_name=tokstr(&b,nm);
            int epcap=0; it->params=NULL; it->pcount=0;
            if(pk(&b)!=TOKEN_RPAREN){
                for(;;){ int ism=match(&b,TOKEN_MUT); TkType*ty=b_type(&b); if(!ty||b.err)break; Token*pn=expect(&b,TOKEN_IDENT,"param"); if(!pn)break;
                    typeof(*it->params) pp; pp.is_mut=ism; pp.ty=ty; pp.nm=tokstr(&b,pn);
                    BAPPEND(&b, it->params, it->pcount, epcap, pp); if(b.err)break;
                    if(!match(&b,TOKEN_COMMA))break; if(pk(&b)==TOKEN_RPAREN)break; } if(b.err)break; }
            if(!expect(&b,TOKEN_RPAREN,"')'"))break;
            if(match(&b,TOKEN_ARROW)){ it->ret=b_type(&b); if(!it->ret||b.err)break; } else it->ret=NULL;
            if(!expect(&b,TOKEN_SEMI,"';'"))break;
            EMIT_ITEM(it); continue;
        }
        if(k==TOKEN_STRUCT){
            b.pos++; Token*nm=expect(&b,TOKEN_IDENT,"struct"); if(!nm)break;
            if(pk(&b)==TOKEN_LT){b.err="generics not in core builder";break;}
            if(!expect(&b,TOKEN_LBRACE,"'{'"))break;
            TkItem*it=(TkItem*)arena_push(arena,sizeof *it,1); it->kind=TK_IT_STRUCT; it->sname=tokstr(&b,nm);
            int sfcap=0; it->fields=NULL; it->fcount=0;
            if(pk(&b)!=TOKEN_RBRACE){ for(;;){ TkType*ty=b_type(&b); if(!ty||b.err)break; Token*fn=expect(&b,TOKEN_IDENT,"field"); if(!fn)break;
                typeof(*it->fields) ff; ff.ty=ty; ff.nm=tokstr(&b,fn); ff.is_pub=0; ff.is_priv=0; ff.is_mut=0;
                BAPPEND(&b, it->fields, it->fcount, sfcap, ff); if(b.err)break;
                if(!match(&b,TOKEN_COMMA))break; if(pk(&b)==TOKEN_RBRACE)break; } if(b.err)break; }
            if(!expect(&b,TOKEN_RBRACE,"'}'"))break;
            EMIT_ITEM(it); continue;
        }
        if(k==TOKEN_ENUM){
            b.pos++; Token*nm=expect(&b,TOKEN_IDENT,"enum"); if(!nm)break;
            if(!expect(&b,TOKEN_LBRACE,"'{'"))break;
            TkItem*it=(TkItem*)arena_push(arena,sizeof *it,1); it->kind=TK_IT_ENUM; it->ename=tokstr(&b,nm);
            int vcap=0; it->vars=NULL; it->vcount=0;
            if(pk(&b)!=TOKEN_RBRACE){ for(;;){ Token*vn=expect(&b,TOKEN_IDENT,"variant"); if(!vn)break;
                typeof(*it->vars) vv; memset(&vv, 0, sizeof vv); vv.vn=tokstr(&b,vn);
                int vslot=it->vcount, tcap=0, fcap=0;
                vv.ts=NULL; vv.tn=0; vv.fs=NULL; vv.fn2=0;
                BAPPEND(&b, it->vars, it->vcount, vcap, vv); if(b.err)break;
                if(pk(&b)==TOKEN_LPAREN){
                    // tuple payload: '(' [type {',' type} [',']] ')'
                    b.pos++;
                    it->vars[vslot].has_tuple=1;
                    if(pk(&b)!=TOKEN_RPAREN){ for(;;){ TkType*t=b_type(&b); if(!t||b.err)break;
                        BAPPEND(&b, it->vars[vslot].ts, it->vars[vslot].tn, tcap, t); if(b.err)break;
                        if(!match(&b,TOKEN_COMMA))break; if(pk(&b)==TOKEN_RPAREN)break; } if(b.err)break; }
                    if(!expect(&b,TOKEN_RPAREN,"')'"))break;
                } else if(pk(&b)==TOKEN_LBRACE){
                    // struct payload: '{' type name {',' ...} [','] '}'
                    b.pos++;
                    it->vars[vslot].has_struct=1;
                    if(pk(&b)!=TOKEN_RBRACE){ for(;;){ TkType*t=b_type(&b); if(!t||b.err)break; Token*fn=expect(&b,TOKEN_IDENT,"field"); if(!fn)break;
                        typeof(*it->vars[vslot].fs) vf; vf.ty=t; vf.nm=tokstr(&b,fn);
                        BAPPEND(&b, it->vars[vslot].fs, it->vars[vslot].fn2, fcap, vf); if(b.err)break;
                        if(!match(&b,TOKEN_COMMA))break; if(pk(&b)==TOKEN_RBRACE)break; } if(b.err)break; }
                    if(!expect(&b,TOKEN_RBRACE,"'}'"))break;
                }
                if(match(&b,TOKEN_EQ)){ TkExpr*d=b_expr(&b); if(!d||b.err)break; it->vars[vslot].has_disc=1; it->vars[vslot].disc=d; }
                if(!match(&b,TOKEN_COMMA))break; if(pk(&b)==TOKEN_RBRACE)break; } if(b.err)break; }
            if(!expect(&b,TOKEN_RBRACE,"'}'"))break;
            EMIT_ITEM(it); continue;
        }
        if(k==TOKEN_FN){
            b.pos++; Token*nm=expect(&b,TOKEN_IDENT,"fn"); if(!nm)break;
            if(pk(&b)==TOKEN_LT){b.err="generics not in core builder";break;}
            if(!expect(&b,TOKEN_LPAREN,"'('"))break;
            TkItem*it=(TkItem*)arena_push(arena,sizeof *it,1); it->kind=TK_IT_FN; it->fn_name=tokstr(&b,nm);
            int pcap=0; it->params=NULL; it->pcount=0;
            if(pk(&b)!=TOKEN_RPAREN){ for(;;){ int ism=match(&b,TOKEN_MUT); TkType*ty=b_type(&b); if(!ty||b.err)break; Token*pn=expect(&b,TOKEN_IDENT,"param"); if(!pn)break;
                typeof(*it->params) pp; pp.is_mut=ism; pp.ty=ty; pp.nm=tokstr(&b,pn);
                BAPPEND(&b, it->params, it->pcount, pcap, pp); if(b.err)break;
                if(!match(&b,TOKEN_COMMA))break; if(pk(&b)==TOKEN_RPAREN)break; } if(b.err)break; }
            if(!expect(&b,TOKEN_RPAREN,"')'"))break;
            if(pk(&b)==TOKEN_LT){b.err="effect rows not in core builder (validation OK)";break;}
            if(match(&b,TOKEN_ARROW)){ it->ret=b_type(&b); if(!it->ret||b.err)break; } else it->ret=NULL;
            if(match(&b,TOKEN_SEMI)){ it->is_proto=1; EMIT_ITEM(it); continue; }
            if(!expect(&b,TOKEN_LBRACE,"'{'"))break;
            TkStmt**ss;int n;TkExpr*t; b_block_body(&b,&ss,&n,&t); if(b.err)break;
            if(!expect(&b,TOKEN_RBRACE,"'}'"))break;
            it->is_proto=0; it->body=ss; it->bcount=n; it->trail=t; EMIT_ITEM(it); continue;
        }
        // global: type name = expr ;
        {
            int ism=match(&b,TOKEN_MUT);
            TkType*ty=b_type(&b); if(!ty||b.err){ if(!b.err) b.err="expected item"; break; }
            Token*nm=expect(&b,TOKEN_IDENT,"global"); if(!nm)break;
            if(!expect(&b,TOKEN_EQ,"'='"))break;
            TkExpr*in=b_expr(&b); if(!in||b.err)break;
            if(!expect(&b,TOKEN_SEMI,"';'"))break;
            TkItem*it=(TkItem*)arena_push(arena,sizeof *it,1); it->kind=TK_IT_GLOBAL;
            TkStmt*d=(TkStmt*)arena_push(arena,sizeof *d,1); d->kind=TK_ST_DECL; d->is_mut=ism; d->ty=ty;
            d->names=(char**)arena_push(arena,sizeof(char*)*2,1); d->inits=(TkExpr**)arena_push(arena,sizeof(TkExpr*)*2,1);
            d->names[0]=tokstr(&b,nm); d->inits[0]=in; d->dcount=1; it->gdecl=d;
            EMIT_ITEM(it); continue;
        }
    }
    if(b.err){ if(err_out)*err_out=b.err; return NULL; }
    return prog;
}
