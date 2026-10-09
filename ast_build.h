#pragma once
#include "ast.h"
#include "lexer.h"
#include "arena.h"
// Build AST for runnable core subset (demo.tk). Validation (parse_full_*)
// already ensured full syntax; this builds nodes for codegen.
// Advanced features (generics/effects/traits/dyn/lambda/match-expr/etc.)
// return NULL with error message (backend TODO, transparent).
TkProgram *ast_build_core(Token *toks, int count, mem_arena *arena, const char **err_out);
