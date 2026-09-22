#ifndef TARGET_X86_64_H
#define TARGET_X86_64_H
#include <stdio.h>
#include "ir.h"
#include "backend.h"
extern const TargetDesc target_x86_64;
int x86_64_emit(IRModule*,FILE*,const TargetDesc*);
#endif
