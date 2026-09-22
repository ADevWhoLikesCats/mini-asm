#ifndef IR_PARSE_H
#define IR_PARSE_H
#include "ir.h"
#include <stdio.h>
IRModule *ir_parse_file(const char *path);
IRModule *ir_parse_stream(FILE *f);
#endif
