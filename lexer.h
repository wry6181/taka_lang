#ifndef LEXER_H
#define LEXER_H

#include "types.h"

typedef enum {
    TOKEN_IDENT,
    TOKEN_INT,
    TOKEN_FN,
    TOKEN_MUT,
    TOKEN_CONST,
    TOKEN_AUTO,
    TOKEN_I8,
    TOKEN_I32,
    TOKEN_LPAREN,   // (
    TOKEN_RPAREN,   // )
    TOKEN_LBRACE,   // {
    TOKEN_RBRACE,   // }
    TOKEN_SEMI,     // ;
    TOKEN_EQ,       // =
    TOKEN_PLUS_EQ,  // +=
    TOKEN_STAR,     // *
    TOKEN_AMP,      // &
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
