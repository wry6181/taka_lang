#pragma once
#include "ast.h"
// Native x86-64 backend: emits GAS assembly text (AT&T) from the core AST.
// Assembled+linked with cc (used as assembler driver only — no C involved).
// SysV AMD64 ABI for extern/libc calls; internal taka calls use sret for
// struct returns. Returns 0 on success.
int x64_emit(TkProgram *prog, const char *out_path);
