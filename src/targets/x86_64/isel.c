#include "targets/x86_64.h"
#include "backend.h"
#include <stdio.h>
#include <string.h>

static int vreg_off(int v){return -8*(v+1);}

static void load_arg(FILE *o, IRInstr *in, int i, const char *reg){
    if(in->kinds[i]==ARG_IMM)
        fprintf(o,"  movq $%d, %s\n",in->args[i],reg);
    else
        fprintf(o,"  movq %d(%%rbp), %s\n",vreg_off(in->args[i]),reg);
}
static void store_dst(FILE *o, int v, const char *reg){
    if(v>=0)fprintf(o,"  movq %s, %d(%%rbp)\n",reg,vreg_off(v));
}

/* For each (pred_name, phi_dst, src_vreg) copy, emit mov src_slot -> dst_slot. */
static void emit_phi_copies_for_edge(FILE *o, IRFunc *f, const char *pred_name, const char *succ_name){
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b=&f->blocks[j];
        if(strcmp(b->name,succ_name))continue;
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in=&b->instrs[k];
            if(in->op!=OP_PHI)continue;
            if(in->dst<0)continue;
            for(int s=0;s<2;s++){
                const char *from = s==0 ? in->label : in->label2;
                if(!from||strcmp(from,pred_name))continue;
                if(in->kinds[s]!=ARG_VREG)continue;
                fprintf(o,"  movq %d(%%rbp), %%rax\n",vreg_off(in->args[s]));
                fprintf(o,"  movq %%rax, %d(%%rbp)\n",vreg_off(in->dst));
            }
        }
    }
}

int x86_64_emit(IRModule *m, FILE *o, const TargetDesc *t){
    (void)t;
    fputs(".text\n",o);
    for(uint32_t i=0;i<m->nfuncs;i++){
        IRFunc *f=&m->funcs[i];
        int maxv=0;
        for(uint32_t j=0;j<f->nblocks;j++){
            IRBlock *b=&f->blocks[j];
            for(uint32_t k=0;k<b->ninstrs;k++){
                IRInstr *in=&b->instrs[k];
                if(in->dst>maxv)maxv=in->dst;
                for(uint32_t a=0;a<in->nargs;a++)
                    if(in->kinds[a]==ARG_VREG&&in->args[a]>maxv)maxv=in->args[a];
            }
        }
        int framesize=(maxv+1)*8;
        framesize=(framesize+15)&~15;

        fprintf(o,".globl %s\n.type %s, @function\n%s:\n",f->name,f->name,f->name);
        fputs("  pushq %rbp\n  movq %rsp, %rbp\n",o);
        if(framesize)fprintf(o,"  subq $%d, %%rsp\n",framesize);

        for(uint32_t j=0;j<f->nblocks;j++){
            IRBlock *b=&f->blocks[j];
            fprintf(o,".L%s_%s:\n",f->name,b->name);
            for(uint32_t k=0;k<b->ninstrs;k++){
                IRInstr *in=&b->instrs[k];

                /* Before a terminator, emit phi copies along each outgoing edge */
                if(in->op==OP_BR||in->op==OP_CBR){
                    if(in->label)
                        emit_phi_copies_for_edge(o,f,b->name,in->label);
                    if(in->op==OP_CBR&&in->label2)
                        emit_phi_copies_for_edge(o,f,b->name,in->label2);
                }

                switch(in->op){
                case OP_RET:
                    if(in->nargs>=1&&in->kinds[0]==ARG_IMM)
                        fprintf(o,"  movq $%d, %%rax\n",in->args[0]);
                    else if(in->nargs>=1)
                        fprintf(o,"  movq %d(%%rbp), %%rax\n",vreg_off(in->args[0]));
                    else
                        fputs("  xorl %eax, %eax\n",o);
                    fputs("  leave\n  ret\n",o);
                    break;

                case OP_PHI:
                    /* handled via phi copies at predecessors; nothing here */
                    break;

                case OP_ADD: case OP_SUB: case OP_MUL:
                case OP_AND: case OP_OR:  case OP_XOR: {
                    load_arg(o,in,0,"%rax");
                    load_arg(o,in,1,"%rcx");
                    const char *mn="add";
                    if(in->op==OP_SUB)mn="sub";
                    else if(in->op==OP_MUL)mn="imul";
                    else if(in->op==OP_AND)mn="and";
                    else if(in->op==OP_OR) mn="or";
                    else if(in->op==OP_XOR)mn="xor";
                    fprintf(o,"  %sq %%rcx, %%rax\n",mn);
                    store_dst(o,in->dst,"%rax");
                    break;
                }
                case OP_SDIV: case OP_UDIV: case OP_SREM: case OP_UREM: {
                    load_arg(o,in,0,"%rax");
                    load_arg(o,in,1,"%rcx");
                    if(in->op==OP_SDIV||in->op==OP_SREM)fputs("  cqto\n  idivq %rcx\n",o);
                    else fputs("  xorl %edx, %edx\n  divq %rcx\n",o);
                    if(in->op==OP_SREM||in->op==OP_UREM)store_dst(o,in->dst,"%rdx");
                    else store_dst(o,in->dst,"%rax");
                    break;
                }
                case OP_SHL: case OP_LSHR: case OP_ASHR: {
                    load_arg(o,in,0,"%rax");
                    load_arg(o,in,1,"%rcx");
                    const char *mn = in->op==OP_SHL ? "shlq" : (in->op==OP_LSHR ? "shrq" : "sarq");
                    fprintf(o,"  %s %%cl, %%rax\n",mn);
                    store_dst(o,in->dst,"%rax");
                    break;
                }
                case OP_NEG: case OP_NOT:
                    load_arg(o,in,0,"%rax");
                    fputs(in->op==OP_NEG?"  negq %rax\n":"  notq %rax\n",o);
                    store_dst(o,in->dst,"%rax");
                    break;

                case OP_ICMP: {
                    load_arg(o,in,0,"%rax");
                    load_arg(o,in,1,"%rcx");
                    static const char *cm[]={"sete","setne","setl","setle","setg","setge","setb","setbe","seta","setae"};
                    fprintf(o,"  cmpq %%rcx, %%rax\n  %s %%al\n  movzbq %%al, %%rax\n",cm[in->pred%10]);
                    store_dst(o,in->dst,"%rax");
                    break;
                }

                case OP_CALL: {
                    static const char *areg[]={"%rdi","%rsi","%rdx","%rcx","%r8","%r9"};
                    for(uint32_t a=0;a<in->nargs&&a<6;a++)
                        load_arg(o,in,a,areg[a]);
                    if(in->callee)fprintf(o,"  call %s\n",in->callee);
                    store_dst(o,in->dst,"%rax");
                    break;
                }

                case OP_CBR:
                    load_arg(o,in,0,"%rax");
                    fprintf(o,"  testq %%rax, %%rax\n  jne .L%s_%s\n  jmp .L%s_%s\n",
                            f->name,in->label,f->name,in->label2);
                    break;

                case OP_BR:
                    if(in->label)fprintf(o,"  jmp .L%s_%s\n",f->name,in->label);
                    break;

                case OP_UNREACHABLE:
                    fputs("  ud2\n",o);
                    break;

                default:
                    break;
                }
            }
        }
        fprintf(o,".size %s, .-%s\n",f->name,f->name);
    }
    fputs(".section .note.GNU-stack,\"\",@progbits\n",o);
    return 0;
}
