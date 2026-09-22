#ifndef BACKEND_H
#define BACKEND_H
#include <stdio.h>
#include "ir.h"
typedef struct TargetDesc TargetDesc;
struct TargetDesc { const char *name,*triple,*assembler,*obj_ext; uint32_t ptr_size,stack_align; bool little; };
int target_emit_dispatch(IRModule*,FILE*,const TargetDesc*);
int emit_icmp(IRInstr*,FILE*,const TargetDesc*);
int emit_fcmp(IRInstr*,FILE*,const TargetDesc*);
int emit_int_math(IRInstr*,FILE*,const TargetDesc*);
int emit_fp_math(IRInstr*,FILE*,const TargetDesc*);
#endif
