#include "ir.h"
#include <stdio.h>
void ir_print(IRModule *m, FILE *o){
    fprintf(o,"; module funcs=%u\n",m->nfuncs);
    for(uint32_t i=0;i<m->nfuncs;i++){
        IRFunc *f=&m->funcs[i];
        fprintf(o,"; func %s blocks=%u\n",f->name,f->nblocks);
        for(uint32_t j=0;j<f->nblocks;j++){
            IRBlock *b=&f->blocks[j];
            fprintf(o,";   block %s instrs=%u\n",b->name,b->ninstrs);
            for(uint32_t k=0;k<b->ninstrs;k++){
                IRInstr *in=&b->instrs[k];
                fprintf(o,";     op=%d dst=%d nargs=%u k0=%d k1=%d a0=%d a1=%d\n",
                        in->op,in->dst,in->nargs,
                        in->kinds[0],in->kinds[1],in->args[0],in->args[1]);
            }
        }
    }
}
