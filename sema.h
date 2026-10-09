#pragma once
#include "ast.h"
#include "arena.h"
// Returns 0 if ok, 1 on error (errors printed to stderr).
int sema_check(TkProgram *prog, mem_arena *arena);
