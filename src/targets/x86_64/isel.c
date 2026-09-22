#include "targets/x86_64.h"
#include "backend.h"
#include <stdio.h>
#include <string.h>

#define PARAM_BASE 1000

static int vreg_off(int v){return -8*(v+1);}

static int type_size(IRType *t){
    if(!t)return 8;
    switch(t->kind){
        case TY_I8:  return 1;
        case TY_I16: return 2;
        case TY_I32: return 4;
        case TY_I64: case TY_PTR: return 8;
        case TY_F32: return 4;
        case TY_F64: return 8;
        default: return 4;
    }
}

static void load_arg(FILE *o, IRInstr *in, int i, const char *reg){
    if(in->kinds[i]==ARG_IMM)
        fprintf(o,"  movq $%d, %s\n",in->args[i],reg);
    else
        fprintf(o,"  movq %d(%%rbp), %s\n",vreg_off(in->args[i]),reg);
}
static void store_dst(FILE *o, int v, const char *reg){
    if(v>=0)fprintf(o,"  movq %s, %d(%%rbp)\n",reg,vreg_off(v));
}

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

/* Count how many 8-byte slots we need, and how many bytes for allocas.
   Allocas get 16-byte aligned regions carved out of the frame. */
static int count_maxv(IRFunc *f){
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
    /* params occupy PARAM_BASE..PARAM_BASE+nparams */
    if(f->nparams>0){
        int pv=PARAM_BASE+(int)f->nparams-1;
        if(pv>maxv)maxv=pv;
    }
    return maxv;
}

/* Collect alloca instructions. Return total bytes needed, and assign each alloca's dst
   a byte offset from rbp (negative). We store offset in the alloca's args[1] as a
   side-channel (hack, but works until we add a proper field). */
static int assign_allocas(IRFunc *f, int base_slot_bytes){
    int cursor=base_slot_bytes; /* grows downward; alloca offsets are below the vreg slots */
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b=&f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in=&b->instrs[k];
            if(in->op!=OP_ALLOCA)continue;
            int sz=in->args[0]; if(sz<=0)sz=4;
            sz=(sz+7)&~7;
            cursor+=sz;
            /* store negative offset in high bits of pred field temporarily */
            in->pred=(uint32_t)cursor;
        }
    }
    return cursor;
}

int x86_64_emit(IRModule *m, FILE *o, const TargetDesc *t){
    (void)t;
    fputs(".text\n",o);
    for(uint32_t i=0;i<m->nfuncs;i++){
        IRFunc *f=&m->funcs[i];
        int maxv=count_maxv(f);
        int vreg_bytes=(maxv+1)*8;
        int total=assign_allocas(f,vreg_bytes);
        int framesize=(total+15)&~15;

        fprintf(o,".globl %s\n.type %s, @function\n%s:\n",f->name,f->name,f->name);
        fputs("  pushq %rbp\n  movq %rsp, %rbp\n",o);
        if(framesize)fprintf(o,"  subq $%d, %%rsp\n",framesize);

        /* Save incoming params into their vreg slots */
        static const char *areg[]={"%rdi","%rsi","%rdx","%rcx","%r8","%r9"};
        for(uint32_t p=0;p<f->nparams&&p<6;p++)
            fprintf(o,"  movq %s, %d(%%rbp)\n",areg[p],vreg_off(PARAM_BASE+(int)p));

        for(uint32_t j=0;j<f->nblocks;j++){
            IRBlock *b=&f->blocks[j];
            fprintf(o,".L%s_%s:\n",f->name,b->name);
            for(uint32_t k=0;k<b->ninstrs;k++){
                IRInstr *in=&b->instrs[k];

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

                case OP_ALLOCA: {
                    /* in->pred holds the byte offset from rbp (positive). Address = rbp - offset. */
                    if(in->dst>=0){
                        fprintf(o,"  leaq -%u(%%rbp), %%rax\n",in->pred);
                        fprintf(o,"  movq %%rax, %d(%%rbp)\n",vreg_off(in->dst));
                    }
                    break;
                }

                case OP_LOAD: {
                    /* args[0] = pointer vreg (or imm). Load size bytes from that address. */
                    load_arg(o,in,0,"%rax");
                    int sz=type_size(in->type);
                    if(sz==1)fputs("  movsbq (%rax), %rax\n",o);
                    else if(sz==2)fputs("  movswq (%rax), %rax\n",o);
                    else if(sz==4)fputs("  movslq (%rax), %rax\n",o);
                    else fputs("  movq (%rax), %rax\n",o);
                    store_dst(o,in->dst,"%rax");
                    break;
                }

                case OP_STORE: {
                    /* args[0] = value, args[1] = pointer */
                    load_arg(o,in,0,"%rax");
                    load_arg(o,in,1,"%rcx");
                    int sz=type_size(in->type);
                    if(sz==1)fputs("  movb %al, (%rcx)\n",o);
                    else if(sz==2)fputs("  movw %ax, (%rcx)\n",o);
                    else if(sz==4)fputs("  movl %eax, (%rcx)\n",o);
                    else fputs("  movq %rax, (%rcx)\n",o);
                    break;
                }

                case OP_CALL: {
                    static const char *areg2[]={"%rdi","%rsi","%rdx","%rcx","%r8","%r9"};
                    for(uint32_t a=0;a<in->nargs&&a<6;a++)
                        load_arg(o,in,a,areg2[a]);
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
