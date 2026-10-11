#ifndef LEXER_H
#define LEXER_H

#include "types.h"

typedef enum {
    TOKEN_IDENT,
    TOKEN_WILDCARD,   // lone `_`
    TOKEN_INT,
    TOKEN_FLOAT,
    TOKEN_STRING,
    TOKEN_CHAR,
    // keywords: items / decls
    TOKEN_FN,
    TOKEN_STRUCT,
    TOKEN_ENUM,
    TOKEN_EFFECT,
    TOKEN_HANDLER,
    TOKEN_TRAIT,
    TOKEN_IMPL,
    TOKEN_DYN,
    TOKEN_TYPE,
    TOKEN_EXTERN,
    TOKEN_IMPORT,
    TOKEN_MOD,
    TOKEN_PUB,
    TOKEN_PRIVATE,
    TOKEN_CONST,
    TOKEN_MUT,
    TOKEN_AUTO,
    TOKEN_WHERE,
    TOKEN_FOR,
    TOKEN_IN,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_WHILE,
    TOKEN_MATCH,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_RETURN,
    TOKEN_DEFER,
    TOKEN_PERFORM,
    TOKEN_HANDLE,
    TOKEN_WITH,
    TOKEN_RESUME,
    TOKEN_TRANSACT,
    TOKEN_SPAWN,
    TOKEN_AS,
    TOKEN_SIZEOF,
    TOKEN_OFFSETOF,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_NULL,
    TOKEN_UNDEF,
    TOKEN_SELF,
    // primitive types
    TOKEN_I8,
    TOKEN_I16,
    TOKEN_I32,
    TOKEN_I64,
    TOKEN_U8,
    TOKEN_U16,
    TOKEN_U32,
    TOKEN_U64,
    TOKEN_F32,
    TOKEN_F64,
    TOKEN_CHAR_TY,
    TOKEN_BOOL,
    TOKEN_SVIEW,
    TOKEN_NEVER,
    // builtin effects (contextual, also usable as ident)
    TOKEN_IO,
    TOKEN_ALLOC,
    TOKEN_READS,
    TOKEN_WRITES,
    TOKEN_DECIDES,
    TOKEN_SUSPENDS,
    TOKEN_TRANSACTS,
    TOKEN_PURE,
    TOKEN_OWNERSHIP,
    TOKEN_EFFECT_KW, // `effect` as generic param prefix (same as TOKEN_EFFECT, kept alias)
    // punctuation / operators
    TOKEN_LPAREN,   // (
    TOKEN_RPAREN,   // )
    TOKEN_LBRACE,   // {
    TOKEN_RBRACE,   // }
    TOKEN_LBRACK,   // [
    TOKEN_RBRACK,   // ]
    TOKEN_SEMI,     // ;
    TOKEN_COMMA,    // ,
    TOKEN_COLON,    // :
    TOKEN_COLON2,   // ::
    TOKEN_DOT,      // .
    TOKEN_DOTDOT,   // ..
    TOKEN_ARROW,    // ->
    TOKEN_FATARROW, // =>
    TOKEN_QMARK,    // ?
    TOKEN_EQ,       // =
    TOKEN_EQEQ,     // ==
    TOKEN_BANGEQ,   // !=
    TOKEN_BANG,     // !
    TOKEN_TILDE,    // ~
    TOKEN_PLUS,     // +
    TOKEN_MINUS,    // -
    TOKEN_STAR,     // *
    TOKEN_SLASH,    // /
    TOKEN_PERCENT,  // %
    TOKEN_LT,       // <
    TOKEN_GT,       // >
    TOKEN_LTEQ,     // <=
    TOKEN_GTEQ,     // >=
    TOKEN_SHL,      // <<
    TOKEN_SHR,      // >>
    TOKEN_AMP,      // &
    TOKEN_AMP2,     // &&
    TOKEN_PIPE,     // |
    TOKEN_PIPE2,    // ||
    TOKEN_CARET,    // ^
    TOKEN_PLUS_EQ,  // +=
    TOKEN_MINUS_EQ, // -=
    TOKEN_STAR_EQ,  // *=
    TOKEN_SLASH_EQ, // /=
    TOKEN_PERCENT_EQ, // %=
    TOKEN_SHL_EQ,   // <<=
    TOKEN_SHR_EQ,   // >>=
    TOKEN_AMP_EQ,   // &=
    TOKEN_PIPE_EQ,  // |=
    TOKEN_CARET_EQ, // ^=
    TOKEN_PLUSPLUS, // ++
    TOKEN_MINUSMINUS, // --
    TOKEN_EOF,
    TOKEN_ERROR
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start;  // pointer into source
    u64 len;
    i32 line, col;
} Token;

typedef struct {
    const char *src;
    u64 pos;
    i32 line, col;
} Lexer;

void lexer_init(Lexer *lx, const char *src);
Token lexer_next(Lexer *lx);
const char *token_name(TokenKind k);

#endif
