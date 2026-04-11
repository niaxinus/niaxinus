#pragma once

#include "parser.h"
#include <stddef.h>

/* One-shot interpreter: run a pre-parsed AST */
int interp_run(const AST *ast);

/* ------------------------------------------------------------------ */
/* Stateful REPL API — preserves variables between calls               */
/* ------------------------------------------------------------------ */
typedef struct InterpHandle InterpHandle;

InterpHandle *interp_state_new(void);
void          interp_state_free(InterpHandle *h);

/* Lex + parse + execute src into h; returns last exit status */
int interp_exec_src(InterpHandle *h, const char *src, size_t len);

int interp_should_exit(const InterpHandle *h);
int interp_last_status(const InterpHandle *h);

