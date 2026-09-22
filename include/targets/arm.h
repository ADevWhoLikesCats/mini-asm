#ifndef TARGET_ARM_H
#define TARGET_ARM_H
#include <stdio.h>
#include "ir.h"
#include "backend.h"
extern const TargetDesc target_arm;
int arm_emit(IRModule*,FILE*,const TargetDesc*);
#endif
