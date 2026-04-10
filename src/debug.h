#pragma once

#include "lexer.h"
#include "parser.h"
#include <stdio.h>

void dump_tokens(FILE *out, const TokenList *tl);
void dump_ast(FILE *out, const AST *ast);

