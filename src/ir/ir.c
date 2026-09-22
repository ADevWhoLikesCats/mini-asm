#include "ir.h"
#include "ir_parse.h"
#include <stdlib.h>
#include <string.h>
IRModule*ir_module_new(void){IRModule*m=calloc(1,sizeof*m);m->cap=8;m->funcs=calloc(8,sizeof(IRFunc));return m;}
IRFunc*ir_func_new(IRModule*m,const char*n,IRType*r){
  if(m->nfuncs==m->cap){m->cap*=2;m->funcs=realloc(m->funcs,m->cap*sizeof(IRFunc));}
  IRFunc*f=&m->funcs[m->nfuncs++];
  memset(f,0,sizeof*f);f->name=n;f->ret=r;f->cap=4;f->blocks=calloc(4,sizeof(IRBlock));
  return f;
}
IRBlock*ir_block_new(IRFunc*f,const char*n){
  if(f->nblocks==f->cap){f->cap*=2;f->blocks=realloc(f->blocks,f->cap*sizeof(IRBlock));}
  IRBlock*b=&f->blocks[f->nblocks++];
  memset(b,0,sizeof*b);b->name=n;b->cap=8;b->instrs=calloc(8,sizeof(IRInstr));
  return b;
}
int ir_emit(IRBlock*b,IROpcode op,IRType*t,int a0,int a1,int a2,int a3,int n){
  if(b->ninstrs==b->cap){b->cap*=2;b->instrs=realloc(b->instrs,b->cap*sizeof(IRInstr));}
  IRInstr*i=&b->instrs[b->ninstrs++];
  memset(i,0,sizeof*i);
  i->op=op;i->type=t;i->dst=-1;i->args[0]=a0;i->args[1]=a1;i->args[2]=a2;i->args[3]=a3;i->nargs=n;
  return (int)(b->ninstrs-1);
}
