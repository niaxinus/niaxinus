#pragma once
#include "arena.h"
#include "parser.h"

/* Compile AST to a native binary at out_path.
   Generates a C source, then invokes gcc.
   Returns 0 on success, non-zero on error. */
int codegen_emit(Arena *arena, const AST *ast, const char *out_path);
