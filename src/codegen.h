#pragma once
#include "arena.h"
#include "parser.h"

/* Generate a native x86-64 ELF64 binary and write it to `out_path`.
   Returns 0 on success, non-zero on error. */
int codegen_emit_elf(Arena *arena, const AST *ast, const char *out_path);
