#include "ir.h"
#include <stdio.h>
void emit_line(FILE*o,const char*f){fputs(f,o);fputc(10,o);}
