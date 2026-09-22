#include "targets/x86_64.h"
#include "backend.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
extern double ir_fpimm[];

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

/* Width suffix helpers */
static const char *acc(int sz){ return sz==1?"%al": sz==2?"%ax": sz==4?"%eax":"%rax"; }

static void load_arg(FILE *o, IRInstr *in, int i, const char *reg){
    if(in->kinds[i]==ARG_IMM)
        fprintf(o,"  movq $%d, %s\n",in->args[i],reg);
    else
        fprintf(o,"  movq %d(%%rbp), %s\n",vreg_off(in->args[i]),reg);
}
static void store_dst(FILE *o, int v, const char *reg){
    if(v>=0)fprintf(o,"  movq %s, %d(%%rbp)\n",reg,vreg_off(v));
}

/* Canonicalize a value already in %rax to 64-bit using the type's width.
   Sign-extend for signed integer types; for i1 we zero-extend. */
static void canon(FILE *o, IRType *t){
    int sz=type_size(t);
    if(sz==1)fputs("  movsbq %al, %rax\n",o);
    else if(sz==2)fputs("  movswq %ax, %rax\n",o);
    else if(sz==4)fputs("  movslq %eax, %rax\n",o);
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
                if(in->kinds[s]==ARG_VREG){
                    fprintf(o,"  movq %d(%%rbp), %%rax\n",vreg_off(in->args[s]));
                    fprintf(o,"  movq %%rax, %d(%%rbp)\n",vreg_off(in->dst));
                } else if(in->kinds[s]==ARG_IMM){
                    fprintf(o,"  movq $%d, %d(%%rbp)\n",in->args[s],vreg_off(in->dst));
                }
            }
        }
    }
}

static int count_maxv(IRFunc *f){
    int maxv=0;
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b=&f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in=&b->instrs[k];
            if(in->dst>maxv)maxv=in->dst;
            for(uint32_t a=0;a<in->nargs&&a<6;a++)
                if(in->kinds[a]==ARG_VREG&&in->args[a]>maxv)maxv=in->args[a];
        }
    }
    if(f->nparams>0){
        int pv=PARAM_BASE+(int)f->nparams-1;
        if(pv>maxv)maxv=pv;
    }
    return maxv;
}

static int assign_allocas(IRFunc *f, int base){
    int cursor=base;
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b=&f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in=&b->instrs[k];
            if(in->op!=OP_ALLOCA)continue;
            int sz=in->args[0]; if(sz<=0)sz=4;
            sz=(sz+7)&~7;
            cursor+=sz;
            in->pred=(uint32_t)cursor;
        }
    }
    return cursor;
}

static int is_fp(IRType *t){return t&&(t->kind==TY_F32||t->kind==TY_F64);}
static const char *fpsuf(IRType *t){return (t&&t->kind==TY_F32)?"ss":"sd";}

static void load_fp(FILE *o, IRInstr *in, int i, const char *xmm, IRType *ty){
    if(in->kinds[i]==ARG_FP){
        uint64_t bits; double d=ir_fpimm[in->args[i]];
        __builtin_memcpy(&bits,&d,8);
        fprintf(o,"  movabsq $%llu, %%rax\n  movq %%rax, %s\n",(unsigned long long)bits,xmm);
    } else if(in->kinds[i]==ARG_IMM){
        fprintf(o,"  movq $%d, %%rax\n  movq %%rax, %s\n",in->args[i],xmm);
    } else {
        fprintf(o,"  movq %d(%%rbp), %s\n",vreg_off(in->args[i]),xmm);
    }
    (void)ty;
}
static void store_dst_fp(FILE *o, int v, const char *xmm){
    if(v>=0)fprintf(o,"  movq %s, %d(%%rbp)\n",xmm,vreg_off(v));
}

static void emit_binop(FILE *o, IRInstr *in){
    int sz=type_size(in->type);
    const char *r0=sz==1?"%al":sz==2?"%ax":sz==4?"%eax":"%rax";
    const char *r1=sz==1?"%cl":sz==2?"%cx":sz==4?"%ecx":"%rcx";
    load_arg(o,in,0,"%rax");
    load_arg(o,in,1,"%rcx");
    const char *mn="add";
    switch(in->op){
        case OP_ADD: mn="add"; break;
        case OP_SUB: mn="sub"; break;
        case OP_MUL: mn="imul"; break;
        case OP_AND: mn="and"; break;
        case OP_OR:  mn="or";  break;
        case OP_XOR: mn="xor"; break;
        default: break;
    }
    if(in->op==OP_MUL)
        fprintf(o,"  imul%c %s, %s\n", sz==1?'b':sz==2?'w':sz==4?'l':'q', r1, r0);
    else
        fprintf(o,"  %s%c %s, %s\n", mn, sz==1?'b':sz==2?'w':sz==4?'l':'q', r1, r0);
    canon(o,in->type);
    store_dst(o,in->dst,"%rax");
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

        static const char *areg[]={"%rdi","%rsi","%rdx","%rcx","%r8","%r9"};
        for(uint32_t p=0;p<f->nparams&&p<6;p++)
            fprintf(o,"  movq %s, %d(%%rbp)\n",areg[p],vreg_off(PARAM_BASE+(int)p));

        for(uint32_t j=0;j<f->nblocks;j++){
            IRBlock *b=&f->blocks[j];
            fprintf(o,".L%s_%s:\n",f->name,b->name);
            for(uint32_t k=0;k<b->ninstrs;k++){
                IRInstr *in=&b->instrs[k];

                if(in->op==OP_BR||in->op==OP_CBR){
                    if(in->label)emit_phi_copies_for_edge(o,f,b->name,in->label);
                    if(in->op==OP_CBR&&in->label2)emit_phi_copies_for_edge(o,f,b->name,in->label2);
                }

                int sz=type_size(in->type);
                switch(in->op){
                case OP_RET: {
                    if(in->nargs>=1&&in->kinds[0]==ARG_IMM)
                        fprintf(o,"  movq $%d, %%rax\n",in->args[0]);
                    else if(in->nargs>=1){
                        int isz=type_size(in->type);
                        fprintf(o,"  movq %d(%%rbp), %%rax\n",vreg_off(in->args[0]));
                        (void)isz;
                    }
                    else fputs("  xorl %eax, %eax\n",o);
                    fputs("  leave\n  ret\n",o);
                    break;
                }
                case OP_PHI: break;

                case OP_ADD: case OP_SUB: case OP_MUL:
                case OP_AND: case OP_OR:  case OP_XOR:
                    emit_binop(o,in);
                    break;

                case OP_SDIV: case OP_UDIV: case OP_SREM: case OP_UREM: {
                    load_arg(o,in,0,"%rax");
                    load_arg(o,in,1,"%rcx");
                    if(sz==4){
                        if(in->op==OP_SDIV||in->op==OP_SREM)fputs("  cltd\n  idivl %ecx\n",o);
                        else fputs("  xorl %edx, %edx\n  divl %ecx\n",o);
                        if(in->op==OP_SREM||in->op==OP_UREM)fputs("  movslq %edx, %rax\n",o);
                        else fputs("  movslq %eax, %rax\n",o);
                    } else {
                        if(in->op==OP_SDIV||in->op==OP_SREM)fputs("  cqto\n  idivq %rcx\n",o);
                        else fputs("  xorl %edx, %edx\n  divq %rcx\n",o);
                        if(in->op==OP_SREM||in->op==OP_UREM)store_dst(o,in->dst,"%rdx");
                        else store_dst(o,in->dst,"%rax");
                        break;
                    }
                    store_dst(o,in->dst,"%rax");
                    break;
                }
                case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: {
                    const char *suf=fpsuf(in->type);
                    load_fp(o,in,0,"%xmm0",in->type);
                    load_fp(o,in,1,"%xmm1",in->type);
                    const char *mn="add";
                    if(in->op==OP_FSUB)mn="sub";
                    else if(in->op==OP_FMUL)mn="mul";
                    else if(in->op==OP_FDIV)mn="div";
                    fprintf(o,"  %s%s %%xmm1, %%xmm0\n",mn,suf);
                    store_dst_fp(o,in->dst,"%xmm0");
                    break;
                }
                case OP_FNEG:
                    load_fp(o,in,0,"%xmm0",in->type);
                    fprintf(o,"  movabsq $0x8000000000000000, %%rax\n  movq %%rax, %%xmm1\n  xorpd %%xmm1, %%xmm0\n");
                    store_dst_fp(o,in->dst,"%xmm0");
                    break;
                case OP_FCMP: {
                    const char *suf=fpsuf(in->type);
                    load_fp(o,in,0,"%xmm0",in->type);
                    load_fp(o,in,1,"%xmm1",in->type);
                    if(!strcmp(suf,"ss"))fputs("  ucomiss %xmm1, %xmm0\n",o);
                    else fputs("  ucomisd %xmm1, %xmm0\n",o);
                    /* predicate mapping: oeq,one,olt,ole,ogt,oge,ord,uno */
                    static const char *cm[]={"sete","setne","setb","setbe","seta","setae","setnp","setp"};
                    fprintf(o,"  %s %%al\n  movzbq %%al, %%rax\n",cm[in->pred%8]);
                    store_dst(o,in->dst,"%rax");
                    break;
                }
                case OP_SHL: case OP_LSHR: case OP_ASHR: {
                    load_arg(o,in,0,"%rax");
                    load_arg(o,in,1,"%rcx");
                    const char *mn = in->op==OP_SHL ? "shl" : (in->op==OP_LSHR ? "shr" : "sar");
                    if(sz==4)fprintf(o,"  %sl %%cl, %%eax\n",mn);
                    else fprintf(o,"  %sq %%cl, %%rax\n",mn);
                    canon(o,in->type);
                    store_dst(o,in->dst,"%rax");
                    break;
                }
                case OP_NEG: case OP_NOT:
                    load_arg(o,in,0,"%rax");
                    fputs(in->op==OP_NEG?"  negq %rax\n":"  notq %rax\n",o);
                    canon(o,in->type);
                    store_dst(o,in->dst,"%rax");
                    break;

                case OP_ICMP: {
                    load_arg(o,in,0,"%rax");
                    load_arg(o,in,1,"%rcx");
                    static const char *cm[]={"sete","setne","setl","setle","setg","setge","setb","setbe","seta","setae"};
                    if(sz==1)fprintf(o,"  cmpb %%cl, %%al\n");
                    else if(sz==2)fprintf(o,"  cmpw %%cx, %%ax\n");
                    else if(sz==4)fprintf(o,"  cmpl %%ecx, %%eax\n");
                    else fprintf(o,"  cmpq %%rcx, %%rax\n");
                    fprintf(o,"  %s %%al\n  movzbq %%al, %%rax\n",cm[in->pred%10]);
                    store_dst(o,in->dst,"%rax");
                    break;
                }

                case OP_ZEXT: case OP_SEXT: case OP_TRUNC: {
                    load_arg(o,in,0,"%rax");
                    if(in->op==OP_ZEXT)      fputs("  movzbq %al, %rax\n",o);
                    else if(in->op==OP_SEXT) fputs("  movsbq %al, %rax\n",o);
                    else                     fputs("  movzbq %al, %rax\n",o);
                    canon(o,in->type);
                    store_dst(o,in->dst,"%rax");
                    break;
                }

                case OP_ALLOCA:
                    if(in->dst>=0){
                        fprintf(o,"  leaq -%u(%%rbp), %%rax\n",in->pred);
                        fprintf(o,"  movq %%rax, %d(%%rbp)\n",vreg_off(in->dst));
                    }
                    break;

                case OP_LOAD: {
                    load_arg(o,in,0,"%rax");
                    int lsz=type_size(in->type);
                    if(lsz==1)fputs("  movsbq (%rax), %rax\n",o);
                    else if(lsz==2)fputs("  movswq (%rax), %rax\n",o);
                    else if(lsz==4)fputs("  movslq (%rax), %rax\n",o);
                    else fputs("  movq (%rax), %rax\n",o);
                    store_dst(o,in->dst,"%rax");
                    break;
                }
                case OP_STORE: {
                    load_arg(o,in,0,"%rax");
                    load_arg(o,in,1,"%rcx");
                    int ssz=type_size(in->type);
                    if(ssz==1)fputs("  movb %al, (%rcx)\n",o);
                    else if(ssz==2)fputs("  movw %ax, (%rcx)\n",o);
                    else if(ssz==4)fputs("  movl %eax, (%rcx)\n",o);
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
                case OP_UNREACHABLE: fputs("  ud2\n",o); break;
                default: break;
                }
            }
        }
        fprintf(o,".size %s, .-%s\n",f->name,f->name);
    }
    fputs(".section .note.GNU-stack,\"\",@progbits\n",o);
    return 0;
}
