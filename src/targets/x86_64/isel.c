#include "targets/x86_64.h"
#include "backend.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define PARAM_BASE 1000
extern double ir_fpimm[];

/* Scratch registers reserved for codegen. Never allocated to vregs. */
#define SCRATCH1 "%r10"
#define SCRATCH2 "%r11"
#define SCRATCHA "%rax"   /* div/rem mandatory */
#define SCRATCHD "%rdx"

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

static int vreg_off(int v){return -8*(v+1);}

/* Produce an operand string for arg i of instruction `in`:
   - "$imm" for immediates
   - "%reg" if the vreg is assigned a physical register
   - "-off(%rbp)" if spilled
   Caller must pass a buffer of at least 32 bytes. */
static const char *op_str(IRInstr *in, int i, const RegAlloc *ra, char *buf){
    if(in->kinds[i]==ARG_IMM){
        snprintf(buf, 32, "$%d", in->args[i]);
        return buf;
    }
    if(in->kinds[i]==ARG_FP){
        /* FP immediates are handled separately */
        snprintf(buf, 32, "$0");
        return buf;
    }
    int v = in->args[i];
    if(ra && v >= 0 && v < ra->nvregs && ra->reg_of[v] >= 0){
        const char *r = regalloc_name(ra, ra->reg_of[v]);
        snprintf(buf, 32, "%s", r);
        return buf;
    }
    snprintf(buf, 32, "%d(%%rbp)", vreg_off(v));
    return buf;
}

/* Operand of a *specific* vreg v (not from args[]). */
static const char *vop_str(int v, const RegAlloc *ra, char *buf){
    if(ra && v >= 0 && v < ra->nvregs && ra->reg_of[v] >= 0){
        snprintf(buf, 32, "%s", regalloc_name(ra, ra->reg_of[v]));
        return buf;
    }
    snprintf(buf, 32, "%d(%%rbp)", vreg_off(v));
    return buf;
}

/* Load arg i into `dst_reg` (movq). Skips if it's already there. */
static void load_arg(FILE *o, IRInstr *in, int i, const char *dst_reg, const RegAlloc *ra){
    char b[32];
    const char *src = op_str(in, i, ra, b);
    if(!strcmp(src, dst_reg)) return;
    fprintf(o, "  movq %s, %s\n", src, dst_reg);
}

/* Store `src_reg` into the destination vreg dst_v. Skips if already there. */
static void store_dst(FILE *o, int dst_v, const char *src_reg, const RegAlloc *ra){
    if(dst_v < 0) return;
    char b[32];
    const char *dst = vop_str(dst_v, ra, b);
    if(!strcmp(dst, src_reg)) return;
    fprintf(o, "  movq %s, %s\n", src_reg, dst);
}

static void canon(FILE *o, IRType *t){
    int sz=type_size(t);
    if(sz==1)fputs("  movsbq %al, %rax\n",o);
    else if(sz==2)fputs("  movswq %ax, %rax\n",o);
    else if(sz==4)fputs("  movslq %eax, %rax\n",o);
}

/* Phi copies now regalloc-aware. */
static int phi_copies_needed(IRFunc *f, const char *pred, const char *succ){
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b=&f->blocks[j];
        if(strcmp(b->name,succ))continue;
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in=&b->instrs[k];
            if(in->op!=OP_PHI || in->dst<0) continue;
            for(int s=0;s<2;s++){
                const char *from = s==0 ? in->label : in->label2;
                if(from && !strcmp(from,pred) && in->kinds[s]!=ARG_NONE) return 1;
            }
        }
    }
    return 0;
}
static void emit_phi_copies_for_edge(FILE *o, IRFunc *f, const char *pred, const char *succ, const RegAlloc *ra){
    if(getenv("CC_DEBUG_PHI"))
        fprintf(o, "# phi-copies %s -> %s\n", pred, succ);
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b=&f->blocks[j];
        if(strcmp(b->name,succ))continue;
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in=&b->instrs[k];
            if(in->op!=OP_PHI || in->dst<0) continue;
            for(int s=0;s<2;s++){
                const char *from = s==0 ? in->label : in->label2;
                if(!from || strcmp(from,pred)) continue;
                char sb[32], db[32];
                const char *src;
                if(in->kinds[s]==ARG_VREG)      src = vop_str(in->args[s], ra, sb);
                else if(in->kinds[s]==ARG_IMM){ snprintf(sb,32,"$%d",in->args[s]); src=sb; }
                else continue;
                const char *dst = vop_str(in->dst, ra, db);
                fprintf(o, "  movq %s, %s\n", src, SCRATCH1);
                fprintf(o, "  movq %s, %s\n", SCRATCH1, dst);
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
            for(uint32_t a=0;a<in->nargs&&a<16;a++)
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

int x86_64_emit(IRModule *m, FILE *o, const TargetDesc *t){
    (void)t;
    fputs(".text\n",o);
    for(uint32_t i=0;i<m->nfuncs;i++){
        IRFunc *f=&m->funcs[i];
        RegAlloc ra = regalloc_run(f);
        regalloc_dump(&ra, f, o);

        int maxv=count_maxv(f);
        int vreg_bytes=(maxv+1)*8;
        int total=assign_allocas(f, vreg_bytes);
        int framesize=(total+15)&~15;

        fprintf(o,".globl %s\n.type %s, @function\n%s:\n",f->name,f->name,f->name);
        fputs("  pushq %rbp\n  movq %rsp, %rbp\n",o);
        if(framesize)fprintf(o,"  subq $%d, %%rsp\n",framesize);

        /* Save callee-saved regs we'll use, if any vreg is assigned to them */
        int used_cs[5] = {0}; /* rbx r12 r13 r14 r15 */
        for(int v=0;v<ra.nvregs;v++){
            int r = ra.reg_of[v];
            if(r >= 5 && r < 10) used_cs[r-5]=1; /* pool indices 5-9 are callee-saved */
        }
        for(int k=0;k<5;k++){
            if(used_cs[k]){
                static const char *cs[]={"%rbx","%r12","%r13","%r14","%r15"};
                fprintf(o,"  pushq %s\n",cs[k]);
            }
        }

        /* Save incoming args to their stack slots FIRST (before we risk
           clobbering ABI arg registers with our own scratch). */
        static const char *areg[]={"%rdi","%rsi","%rdx","%rcx","%r8","%r9"};
        for(uint32_t p=0;p<f->nparams && p<6;p++){
            int pv = PARAM_BASE + (int)p;
            fprintf(o,"  movq %s, %d(%%rbp)\n",areg[p],vreg_off(pv));
        }
        for(uint32_t p=6;p<f->nparams;p++){
            int pv = PARAM_BASE + (int)p;
            fprintf(o,"  movq %u(%%rbp), %%rax\n  movq %%rax, %d(%%rbp)\n",
                    16+(p-6)*8, vreg_off(pv));
        }
        /* If a param was assigned a register, load it there now. */
        for(uint32_t p=0;p<f->nparams;p++){
            int pv = PARAM_BASE + (int)p;
            if(pv < ra.nvregs && ra.reg_of[pv] >= 0){
                const char *r = regalloc_name(&ra, ra.reg_of[pv]);
                fprintf(o,"  movq %d(%%rbp), %s\n",vreg_off(pv),r);
            }
        }

        for(uint32_t j=0;j<f->nblocks;j++){
            IRBlock *b=&f->blocks[j];
            fprintf(o,".L%s_%s:\n",f->name,b->name);
            for(uint32_t k=0;k<b->ninstrs;k++){
                IRInstr *in=&b->instrs[k];

                if(in->op==OP_BR){
                    if(in->label)emit_phi_copies_for_edge(o,f,b->name,in->label,&ra);
                }

                int sz=type_size(in->type);
                switch(in->op){
                case OP_RET: {
                    if(in->nargs>=1){
                        if(in->kinds[0]==ARG_IMM)
                            fprintf(o,"  movq $%d, %%rax\n",in->args[0]);
                        else{
                            char b[32];
                            const char *s = vop_str(in->args[0], &ra, b);
                            fprintf(o,"  movq %s, %%rax\n",s);
                        }
                    } else fputs("  xorl %eax, %eax\n",o);
                    /* Restore callee-saved */
                    for(int q=4;q>=0;q--){
                        if(used_cs[q]){
                            static const char *cs[]={"%rbx","%r12","%r13","%r14","%r15"};
                            fprintf(o,"  popq %s\n",cs[q]);
                        }
                    }
                    fputs("  leave\n  ret\n",o);
                    break;
                }
                case OP_PHI: break;

                case OP_ADD: case OP_SUB: case OP_MUL:
                case OP_AND: case OP_OR:  case OP_XOR: {
                    /* Try to do it in-place: if dst has a register and it equals
                       arg0's register, we can do `op arg1, dst_reg`. Otherwise
                       use SCRATCH1 as the working reg and copy at end. */
                    char a0b[32];
                    const char *a0 = op_str(in,0,&ra,a0b);
                    char dstb[32];
                    const char *dstreg = (in->dst>=0 && ra.reg_of[in->dst]>=0)
                                         ? regalloc_name(&ra, ra.reg_of[in->dst])
                                         : SCRATCH1;

                    /* Load a0 into dstreg. */
                    if(strcmp(a0,dstreg)) fprintf(o,"  movq %s, %s\n",a0,dstreg);

                    /* Now op arg1 into dstreg. arg1 may be immediate, reg, or slot. */
                    char a1b[32];
                    const char *a1 = op_str(in,1,&ra,a1b);

                    const char *mn="add";
                    if(in->op==OP_SUB)mn="sub";
                    else if(in->op==OP_MUL)mn="imul";
                    else if(in->op==OP_AND)mn="and";
                    else if(in->op==OP_OR) mn="or";
                    else if(in->op==OP_XOR)mn="xor";

                    fprintf(o,"  %sq %s, %s\n",mn,a1,dstreg);
                    canon(o,in->type);
                    if(in->dst>=0 && strcmp(dstreg, vop_str(in->dst,&ra,dstb)))
                        fprintf(o,"  movq %s, %s\n",dstreg,dstb);
                    break;
                }
                case OP_SDIV: case OP_UDIV: case OP_SREM: case OP_UREM: {
                    char a0b[32],a1b[32];
                    const char *a0 = op_str(in,0,&ra,a0b);
                    const char *a1 = op_str(in,1,&ra,a1b);
                    fprintf(o,"  movq %s, %%rax\n",a0);
                    fprintf(o,"  movq %s, %%rcx\n",a1);
                    if(in->op==OP_SDIV||in->op==OP_SREM)fputs("  cqto\n  idivq %rcx\n",o);
                    else fputs("  xorl %edx, %edx\n  divq %rcx\n",o);
                    const char *res = (in->op==OP_SREM||in->op==OP_UREM) ? "%rdx" : "%rax";
                    store_dst(o, in->dst, res, &ra);
                    break;
                }
                case OP_SHL: case OP_LSHR: case OP_ASHR: {
                    char a0b[32],a1b[32];
                    const char *a0 = op_str(in,0,&ra,a0b);
                    const char *a1 = op_str(in,1,&ra,a1b);
                    fprintf(o,"  movq %s, %s\n",a0,SCRATCH1);
                    fprintf(o,"  movq %s, %%rcx\n",a1);
                    const char *mn = in->op==OP_SHL?"shl":(in->op==OP_LSHR?"shr":"sar");
                    fprintf(o,"  %sq %%cl, %s\n",mn,SCRATCH1);
                    canon(o,in->type);
                    store_dst(o, in->dst, SCRATCH1, &ra);
                    break;
                }
                case OP_NEG: case OP_NOT: {
                    char a0b[32];
                    const char *a0 = op_str(in,0,&ra,a0b);
                    fprintf(o,"  movq %s, %s\n",a0,SCRATCH1);
                    fputs(in->op==OP_NEG?"  negq ":"  notq ",o);
                    fprintf(o,"%s\n",SCRATCH1);
                    canon(o,in->type);
                    store_dst(o, in->dst, SCRATCH1, &ra);
                    break;
                }
                case OP_ICMP: {
                    char a0b[32],a1b[32];
                    const char *a0 = op_str(in,0,&ra,a0b);
                    const char *a1 = op_str(in,1,&ra,a1b);
                    fprintf(o,"  movq %s, %s\n",a0,SCRATCH1);
                    fprintf(o,"  movq %s, %s\n",a1,SCRATCH2);
                    if(sz==1)fputs("  cmpb %r11b, %r10b\n",o);
                    else if(sz==2)fputs("  cmpw %r11w, %r10w\n",o);
                    else if(sz==4)fputs("  cmpl %r11d, %r10d\n",o);
                    else fputs("  cmpq %r11, %r10\n",o);
                    static const char *cm[]={"sete","setne","setl","setle","setg","setge","setb","setbe","seta","setae"};
                    fprintf(o,"  %s %%al\n  movzbq %%al, %%rax\n",cm[in->pred%10]);
                    store_dst(o, in->dst, "%rax", &ra);
                    break;
                }
                case OP_ZEXT: case OP_SEXT: case OP_TRUNC: {
                    char a0b[32];
                    const char *a0 = op_str(in,0,&ra,a0b);
                    fprintf(o,"  movq %s, %%rax\n",a0);
                    if(in->op==OP_SEXT) fputs("  movsbq %al, %rax\n",o);
                    else fputs("  movzbq %al, %rax\n",o);
                    canon(o,in->type);
                    store_dst(o, in->dst, "%rax", &ra);
                    break;
                }
                case OP_SITOFP: case OP_UITOFP: case OP_FPTOSI: case OP_FPTOUI:
                case OP_FPEXT: case OP_FPTRUNC: {
                    /* Not regalloc-aware yet — FP vregs stay in slots */
                    int src_is_fp = (in->pred==TY_F32 || in->pred==TY_F64);
                    if(src_is_fp){
                        if(in->kinds[0]==ARG_FP){
                            uint64_t bits; double d=ir_fpimm[in->args[0]];
                            memcpy(&bits,&d,8);
                            fprintf(o,"  movabsq $%llu, %%rax\n  movq %%rax, %%xmm0\n",(unsigned long long)bits);
                        } else {
                            fprintf(o,"  movq %d(%%rbp), %%xmm0\n",vreg_off(in->args[0]));
                        }
                    } else {
                        char a0b[32];
                        const char *a0 = op_str(in,0,&ra,a0b);
                        fprintf(o,"  movq %s, %%rax\n",a0);
                    }
                    int dt = in->type ? in->type->kind : TY_F64;
                    if(in->op==OP_SITOFP){
                        if(dt==TY_F32)fputs("  cvtsi2ssq %rax, %xmm0\n",o);
                        else fputs("  cvtsi2sdq %rax, %xmm0\n",o);
                    } else if(in->op==OP_UITOFP){
                        if(dt==TY_F32)fputs("  cvtsi2ssq %rax, %xmm0\n",o);
                        else fputs("  cvtsi2sdq %rax, %xmm0\n",o);
                    } else if(in->op==OP_FPTOSI || in->op==OP_FPTOUI){
                        if(in->pred==TY_F32)fputs("  cvttss2siq %xmm0, %rax\n",o);
                        else fputs("  cvttsd2siq %xmm0, %rax\n",o);
                    } else if(in->op==OP_FPEXT){
                        fputs("  cvtss2sd %xmm0, %xmm0\n",o);
                    } else if(in->op==OP_FPTRUNC){
                        fputs("  cvtsd2ss %xmm0, %xmm0\n",o);
                    }
                    if(in->dst>=0){
                        int produces_fp = (in->op==OP_SITOFP||in->op==OP_UITOFP||in->op==OP_FPEXT||in->op==OP_FPTRUNC);
                        if(produces_fp) fprintf(o,"  movq %%xmm0, %d(%%rbp)\n", vreg_off(in->dst));
                        else           fprintf(o,"  movq %%rax, %d(%%rbp)\n", vreg_off(in->dst));
                    }
                    break;
                }
                case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: {
                    /* FP vregs stay in slots for now. */
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[0]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movabsq $%llu, %%rax\n  movq %%rax, %%xmm0\n",(unsigned long long)bits);
                    } else fprintf(o,"  movq %d(%%rbp), %%xmm0\n",vreg_off(in->args[0]));
                    if(in->kinds[1]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[1]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movabsq $%llu, %%rax\n  movq %%rax, %%xmm1\n",(unsigned long long)bits);
                    } else fprintf(o,"  movq %d(%%rbp), %%xmm1\n",vreg_off(in->args[1]));
                    const char *suf = (in->type && in->type->kind==TY_F32) ? "ss" : "sd";
                    const char *mn="add";
                    if(in->op==OP_FSUB)mn="sub";
                    else if(in->op==OP_FMUL)mn="mul";
                    else if(in->op==OP_FDIV)mn="div";
                    fprintf(o,"  %s%s %%xmm1, %%xmm0\n",mn,suf);
                    if(in->dst>=0) fprintf(o,"  movq %%xmm0, %d(%%rbp)\n", vreg_off(in->dst));
                    break;
                }
                case OP_FNEG: {
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[0]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movabsq $%llu, %%rax\n  movq %%rax, %%xmm0\n",(unsigned long long)bits);
                    } else fprintf(o,"  movq %d(%%rbp), %%xmm0\n",vreg_off(in->args[0]));
                    fputs("  movabsq $0x8000000000000000, %rax\n  movq %rax, %xmm1\n  xorpd %xmm1, %xmm0\n",o);
                    if(in->dst>=0) fprintf(o,"  movq %%xmm0, %d(%%rbp)\n", vreg_off(in->dst));
                    break;
                }
                case OP_FCMP: {
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[0]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movabsq $%llu, %%rax\n  movq %%rax, %%xmm0\n",(unsigned long long)bits);
                    } else fprintf(o,"  movq %d(%%rbp), %%xmm0\n",vreg_off(in->args[0]));
                    if(in->kinds[1]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[1]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movabsq $%llu, %%rax\n  movq %%rax, %%xmm1\n",(unsigned long long)bits);
                    } else fprintf(o,"  movq %d(%%rbp), %%xmm1\n",vreg_off(in->args[1]));
                    const char *suf = (in->type && in->type->kind==TY_F32) ? "ss" : "sd";
                    fprintf(o,"  ucomi%s %%xmm1, %%xmm0\n",suf);
                    static const char *cm[]={"sete","setne","setb","setbe","seta","setae","setnp","setp"};
                    fprintf(o,"  %s %%al\n  movzbq %%al, %%rax\n",cm[in->pred%8]);
                    store_dst(o, in->dst, "%rax", &ra);
                    break;
                }

                case OP_ALLOCA:
                    if(in->dst>=0){
                        char db[32];
                        const char *dst = vop_str(in->dst,&ra,db);
                        fprintf(o,"  leaq -%u(%%rbp), %%rax\n",in->pred);
                        fprintf(o,"  movq %%rax, %s\n",dst);
                    }
                    break;

                case OP_LOAD: {
                    char a0b[32];
                    const char *a0 = op_str(in,0,&ra,a0b);
                    fprintf(o,"  movq %s, %%rax\n",a0);
                    int lsz=type_size(in->type);
                    if(lsz==1)fputs("  movsbq (%rax), %rax\n",o);
                    else if(lsz==2)fputs("  movswq (%rax), %rax\n",o);
                    else if(lsz==4)fputs("  movslq (%rax), %rax\n",o);
                    else fputs("  movq (%rax), %rax\n",o);
                    store_dst(o, in->dst, "%rax", &ra);
                    break;
                }
                case OP_STORE: {
                    char a0b[32],a1b[32];
                    const char *a0 = op_str(in,0,&ra,a0b);
                    const char *a1 = op_str(in,1,&ra,a1b);
                    fprintf(o,"  movq %s, %%rax\n",a0);
                    fprintf(o,"  movq %s, %%rcx\n",a1);
                    int ssz=type_size(in->type);
                    if(ssz==1)fputs("  movb %al, (%rcx)\n",o);
                    else if(ssz==2)fputs("  movw %ax, (%rcx)\n",o);
                    else if(ssz==4)fputs("  movl %eax, (%rcx)\n",o);
                    else fputs("  movq %rax, (%rcx)\n",o);
                    break;
                }
                case OP_GEP: case OP_GEP_FIELD: {
                    char a0b[32];
                    const char *a0 = op_str(in,0,&ra,a0b);
                    fprintf(o,"  movq %s, %s\n",a0,SCRATCH1);
                    if(in->op==OP_GEP_FIELD){
                        unsigned off = in->pred;
                        fprintf(o,"  addq $%u, %s\n",off,SCRATCH1);
                    } else {
                        char a1b[32];
                        const char *a1 = op_str(in,1,&ra,a1b);
                        fprintf(o,"  movq %s, %s\n",a1,SCRATCH2);
                        unsigned esz = in->pred;
                        if(esz==2)      fputs("  leaq (r10,r11,2), %r10\n",o);
                        else if(esz==4) fputs("  leaq (r10,r11,4), %r10\n",o);
                        else if(esz==8) fputs("  leaq (r10,r11,8), %r10\n",o);
                        else{
                            fprintf(o,"  imulq $%u, %%r11, %%r11\n  addq %%r11, %%r10\n",esz);
                        }
                    }
                    store_dst(o, in->dst, SCRATCH1, &ra);
                    break;
                }
                case OP_STR: {
                    uint32_t idx=(uint32_t)in->args[0];
                    if(in->dst>=0){
                        char db[32];
                        const char *dst = vop_str(in->dst,&ra,db);
                        fprintf(o,"  leaq %s(%%rip), %%rax\n",m->strings[idx].label);
                        fprintf(o,"  movq %%rax, %s\n",dst);
                    }
                    break;
                }

                case OP_CALL: {
                    static const char *areg2[]={"%rdi","%rsi","%rdx","%rcx","%r8","%r9"};
                    uint32_t n = in->nargs;
                    uint32_t stackargs = n > 6 ? n - 6 : 0;
                    if(stackargs){
                        uint32_t bytes = stackargs * 8;
                        uint32_t pad = (bytes & 15) ? 8 : 0;
                        if(pad) fputs("  subq $8, %rsp\n",o);
                        for(int a=(int)n-1; a>=6; a--){
                            char ab[32];
                            const char *s = op_str(in,a,&ra,ab);
                            fprintf(o,"  movq %s, %%rax\n  pushq %%rax\n",s);
                        }
                    }
                    for(uint32_t a=0;a<6&&a<n;a++){
                        char ab[32];
                        const char *s = op_str(in,a,&ra,ab);
                        if(strcmp(s,areg2[a])) fprintf(o,"  movq %s, %s\n",s,areg2[a]);
                    }
                    fputs("  xorl %eax, %eax\n",o);
                    if(in->callee)fprintf(o,"  call %s\n",in->callee);
                    if(stackargs){
                        uint32_t bytes = stackargs * 8;
                        uint32_t pad = (bytes & 15) ? 8 : 0;
                        fprintf(o,"  addq $%u, %%rsp\n",bytes+pad);
                    }
                    store_dst(o, in->dst, "%rax", &ra);
                    break;
                }

                case OP_CBR: {
                    char a0b[32];
                    const char *a0 = op_str(in,0,&ra,a0b);
                    fprintf(o,"  movq %s, %%rax\n",a0);
                    int need_t = phi_copies_needed(f, b->name, in->label);
                    int need_f = phi_copies_needed(f, b->name, in->label2);
                    if(!need_t && !need_f){
                        fprintf(o,"  testq %%rax, %%rax\n  jne .L%s_%s\n  jmp .L%s_%s\n",
                                f->name,in->label,f->name,in->label2);
                    } else {
                        fprintf(o,"  testq %%rax, %%rax\n");
                        fprintf(o,"  jne .L%s_%s__ct_%u\n", f->name, b->name, k);
                        fprintf(o,".L%s_%s__fall_%u:\n", f->name, b->name, k);
                        if(need_f){
                            emit_phi_copies_for_edge(o,f,b->name,in->label2,&ra);
                        }
                        fprintf(o,"  jmp .L%s_%s\n", f->name, in->label2);
                        fprintf(o,".L%s_%s__ct_%u:\n", f->name, b->name, k);
                        if(need_t){
                            emit_phi_copies_for_edge(o,f,b->name,in->label,&ra);
                        }
                        fprintf(o,"  jmp .L%s_%s\n", f->name, in->label);
                    }
                    break;
                }
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
    if(m->nstrings){
        fputs(".section .rodata\n", o);
        for(uint32_t i=0;i<m->nstrings;i++){
            IRString *st = &m->strings[i];
            fprintf(o, "%s:\n", st->label);
            fputs("  .byte ", o);
            for(uint32_t j=0;j<st->len;j++)
                fprintf(o, "%u,", (unsigned char)st->bytes[j]);
            fputs("0\n", o);
        }
    }
    fputs(".section .note.GNU-stack,\"\",@progbits\n",o);
    return 0;
}
