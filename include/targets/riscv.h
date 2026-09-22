#ifndef TARGET_RISCV_H
#define TARGET_RISCV_H
#include <stdio.h>
#include "ir.h"
#include "backend.h"
extern const TargetDesc target_riscv;
int riscv_emit(IRModule*,FILE*,const TargetDesc*);
#endif
