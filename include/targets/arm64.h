#ifndef TARGET_ARM64_H
#define TARGET_ARM64_H
#include <stdio.h>
#include "ir.h"
#include "backend.h"
extern const TargetDesc target_arm64;
int arm64_emit(IRModule*,FILE*,const TargetDesc*);
#endif
