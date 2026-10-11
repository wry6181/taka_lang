#pragma once
#include "ast.h"
#include <stdio.h>
// Generate plain C (C stdlib only) from AST. Returns 0 ok.
// Advanced features (dyn/effects/transact/spawn/lambda-escaping) emit
// simple stubs documented in cgen.c; core (fns/structs/enums/control/match) is real.
int cgen_emit(TkProgram *prog, const char *out_path);
// Nonzero when the last cgen_emit used nested fns (defer bodies via GNU
// trampolines): the link then needs -z execstack to avoid an ld warning.
int cgen_needs_execstack(void);
