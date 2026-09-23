#include "targets/x86.h"
#include "backend.h"
#include "targets/x86_64.h"
#include "targets/arm.h"
#include "targets/arm64.h"
#include "targets/riscv.h"
#include <string.h>

void opt_run(IRModule *m);
void ir_print(IRModule *m, FILE *o);
const TargetDesc *backend_lookup(const char*n){
  if(!n)return NULL;
  if(!strcmp(n,"x86"))return &target_x86;
  if(!strcmp(n,"x86_64"))return &target_x86_64;
  if(!strcmp(n,"arm"))return &target_arm;
  if(!strcmp(n,"arm64"))return &target_arm64;
  if(!strcmp(n,"riscv"))return &target_riscv;
  return NULL;
}
int target_emit_dispatch(IRModule*m,FILE*o,const TargetDesc*t){
    opt_run(m);
    if(getenv("CC_DUMP_IR")) ir_print(m, stderr);
  if(!t)return 1;
  if(!strcmp(t->name,"x86"))return x86_emit(m,o,t);
  if(!strcmp(t->name,"x86_64"))return x86_64_emit(m,o,t);
  if(!strcmp(t->name,"arm"))return arm_emit(m,o,t);
  if(!strcmp(t->name,"arm64"))return arm64_emit(m,o,t);
  if(!strcmp(t->name,"riscv"))return riscv_emit(m,o,t);
  return 1;
}
