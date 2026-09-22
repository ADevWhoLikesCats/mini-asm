#ifndef TARGET_X86_H
#define TARGET_X86_H
#include <stdio.h>
#include "ir.h"
#include "backend.h"
extern const TargetDesc target_x86;
int x86_emit(IRModule*,FILE*,const TargetDesc*);
#endif
