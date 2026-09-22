#include "targets/arm64.h"
#include "backend.h"
#include <stdio.h>
#include <string.h>
int arm64_emit(IRModule*m,FILE*o,const TargetDesc*t){
  (void)t;
  fputs(".text\n",o);
  for(uint32_t i=0;i<m->nfuncs;i++){
    IRFunc*f=&m->funcs[i];
    fprintf(o,".globl %s\n.type %s, %%function\n%s:\n",f->name,f->name,f->name);
    fputs("  stp x29, x30, [sp, #-16]!\n  mov x29, sp\n",o);
    for(uint32_t j=0;j<f->nblocks;j++){
      IRBlock*b=&f->blocks[j];
      for(uint32_t k=0;k<b->ninstrs;k++){
        IRInstr*in=&b->instrs[k];
        if(in->op==OP_RET){
          if(in->nargs>=1)fprintf(o,"  mov x0, #%d\n",in->args[0]);
          else fputs("  mov x0, #0\n",o);
          fputs("  ldp x29, x30, [sp], #16\n  ret\n",o);
        }
      }
    }
  }
  fputs(".section .note.GNU-stack,\"\",%progbits\n",o);
  return 0;
}
