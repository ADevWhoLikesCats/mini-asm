#include "targets/arm64.h"
#include "backend.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define PARAM_BASE 1000
extern double ir_fpimm[];

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
static int is_fp(IRType *t){return t&&(t->kind==TY_F32||t->kind==TY_F64);}
static const char *fpsuf(IRType *t){return (t&&t->kind==TY_F32)?"s":"d";}

/* AArch64 stack model: every vreg is 8 bytes at [sp + 8*v] from base of frame.
   We compute offsets from sp directly, positive. */
static int slotoff(int v){return 8*v;}

static void load_int(FILE *o, IRInstr *in, int i, const char *reg){
    if(in->kinds[i]==ARG_IMM)
        fprintf(o,"  mov %s, #%d\n",reg,in->args[i]);
    else
        fprintf(o,"  ldr %s, [sp, #%d]\n",reg,slotoff(in->args[i]));
}
static void store_int(FILE *o, int v, const char *reg){
    if(v>=0)fprintf(o,"  str %s, [sp, #%d]\n",reg,slotoff(v));
}
static void load_fp(FILE *o, IRInstr *in, int i, const char *dreg){
    if(in->kinds[i]==ARG_FP){
        uint64_t bits; double d=ir_fpimm[in->args[i]];
        __builtin_memcpy(&bits,&d,8);
        fprintf(o,"  movz x9, #%u\n  movk x9, #%u, lsl #16\n  movk x9, #%u, lsl #32\n  movk x9, #%u, lsl #48\n  fmov %s, x9\n",
            (unsigned)(bits&0xffff),(unsigned)((bits>>16)&0xffff),
            (unsigned)((bits>>32)&0xffff),(unsigned)((bits>>48)&0xffff),dreg);
    } else if(in->kinds[i]==ARG_IMM){
        fprintf(o,"  mov x9, #%d\n  fmov %s, x9\n",in->args[i],dreg);
    } else {
        fprintf(o,"  ldr %s, [sp, #%d]\n",dreg,slotoff(in->args[i]));
    }
}
static void store_fp(FILE *o, int v, const char *dreg){
    if(v>=0)fprintf(o,"  str %s, [sp, #%d]\n",dreg,slotoff(v));
}

static void canon(FILE *o, IRType *t){
    int sz=type_size(t);
    if(sz==1)fputs("  sxtb x0, w0\n",o);
    else if(sz==2)fputs("  sxth x0, w0\n",o);
    else if(sz==4)fputs("  sxtw x0, w0\n",o);
}

static void emit_phi_copies_for_edge(FILE *o, IRFunc *f, const char *pred_name, const char *succ_name){
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b=&f->blocks[j];
        if(strcmp(b->name,succ_name))continue;
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in=&b->instrs[k];
            if(in->op!=OP_PHI||in->dst<0)continue;
            for(int s=0;s<2;s++){
                const char *from = s==0?in->label:in->label2;
                if(!from||strcmp(from,pred_name))continue;
                if(in->kinds[s]==ARG_VREG){
                    fprintf(o,"  ldr x9, [sp, #%d]\n  str x9, [sp, #%d]\n",
                        slotoff(in->args[s]),slotoff(in->dst));
                } else if(in->kinds[s]==ARG_IMM){
                    fprintf(o,"  mov x9, #%d\n  str x9, [sp, #%d]\n",
                        in->args[s],slotoff(in->dst));
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
            sz=(sz+15)&~15;
            cursor+=sz;
            in->pred=(uint32_t)cursor;
        }
    }
    return cursor;
}

int arm64_emit(IRModule *m, FILE *o, const TargetDesc *t){
    (void)t;
    fputs(".text\n",o);
    for(uint32_t i=0;i<m->nfuncs;i++){
        IRFunc *f=&m->funcs[i];
        int maxv=count_maxv(f);
        int vreg_bytes=(maxv+1)*8;
        int total=assign_allocas(f,vreg_bytes);
        int framesize=(total+15)&~15;
        if(framesize==0)framesize=16;

        fprintf(o,".globl %s\n.type %s, %%function\n%s:\n",f->name,f->name,f->name);
        fputs("  stp x29, x30, [sp, #-16]!\n  mov x29, sp\n",o);
        fprintf(o,"  sub sp, sp, #%d\n",framesize);

        static const char *areg[]={"x0","x1","x2","x3","x4","x5"};
        for(uint32_t p=0;p<f->nparams&&p<6;p++)
            fprintf(o,"  str %s, [sp, #%d]\n",areg[p],slotoff(PARAM_BASE+(int)p));

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
                case OP_RET:
                    if(in->nargs>=1){
                        if(in->kinds[0]==ARG_IMM)fprintf(o,"  mov x0, #%d\n",in->args[0]);
                        else fprintf(o,"  ldr x0, [sp, #%d]\n",slotoff(in->args[0]));
                    } else fputs("  mov x0, #0\n",o);
                    fputs("  mov sp, x29\n  ldp x29, x30, [sp], #16\n  ret\n",o);
                    break;

                case OP_PHI: break;

                case OP_ADD: case OP_SUB: case OP_MUL:
                case OP_AND: case OP_OR:  case OP_XOR: {
                    load_int(o,in,0,"x0");
                    load_int(o,in,1,"x1");
                    const char *mn="add";
                    if(in->op==OP_SUB)mn="sub";
                    else if(in->op==OP_MUL)mn="mul";
                    else if(in->op==OP_AND)mn="and";
                    else if(in->op==OP_OR) mn="orr";
                    else if(in->op==OP_XOR)mn="eor";
                    fprintf(o,"  %s x0, x0, x1\n",mn);
                    canon(o,in->type);
                    store_int(o,in->dst,"x0");
                    break;
                }
                case OP_SDIV: case OP_UDIV: {
                    load_int(o,in,0,"x0");
                    load_int(o,in,1,"x1");
                    fputs(in->op==OP_SDIV?"  sdiv x0, x0, x1\n":"  udiv x0, x0, x1\n",o);
                    store_int(o,in->dst,"x0");
                    break;
                }
                case OP_SREM: case OP_UREM: {
                    load_int(o,in,0,"x0");
                    load_int(o,in,1,"x1");
                    fputs(in->op==OP_SREM?"  sdiv x2, x0, x1\n":"  udiv x2, x0, x1\n",o);
                    fputs("  msub x0, x2, x1, x0\n",o);
                    store_int(o,in->dst,"x0");
                    break;
                }
                case OP_SHL: case OP_LSHR: case OP_ASHR: {
                    load_int(o,in,0,"x0");
                    load_int(o,in,1,"x1");
                    const char *mn = in->op==OP_SHL?"lsl":(in->op==OP_LSHR?"lsr":"asr");
                    fprintf(o,"  %s x0, x0, x1\n",mn);
                    store_int(o,in->dst,"x0");
                    break;
                }
                case OP_SITOFP: case OP_UITOFP: case OP_FPTOSI: case OP_FPTOUI:
                case OP_FPEXT:  case OP_FPTRUNC: {
                    IRTypeKind src=(IRTypeKind)in->pred;
                    IRTypeKind dt=in->type?in->type->kind:TY_F64;
                    int src_is_fp = (src==TY_F32||src==TY_F64);
                    if(src_is_fp) load_fp(o,in,0,"d0");
                    else          load_int(o,in,0,"x0");
                    if(in->op==OP_SITOFP){
                        if(dt==TY_F32)fputs("  scvtf s0, x0\n",o);
                        else          fputs("  scvtf d0, x0\n",o);
                        store_fp(o,in->dst,"d0");
                    } else if(in->op==OP_UITOFP){
                        if(dt==TY_F32)fputs("  ucvtf s0, x0\n",o);
                        else          fputs("  ucvtf d0, x0\n",o);
                        store_fp(o,in->dst,"d0");
                    } else if(in->op==OP_FPTOSI){
                        if(src==TY_F32)fputs("  fcvtzs x0, s0\n",o);
                        else           fputs("  fcvtzs x0, d0\n",o);
                        store_int(o,in->dst,"x0");
                    } else if(in->op==OP_FPTOUI){
                        if(src==TY_F32)fputs("  fcvtzu x0, s0\n",o);
                        else           fputs("  fcvtzu x0, d0\n",o);
                        store_int(o,in->dst,"x0");
                    } else if(in->op==OP_FPEXT){
                        fputs("  fcvt d0, s0\n",o);
                        store_fp(o,in->dst,"d0");
                    } else if(in->op==OP_FPTRUNC){
                        fputs("  fcvt s0, d0\n",o);
                        store_fp(o,in->dst,"d0");
                    }
                    break;
                }
                case OP_NEG:
                    if(in->type && (in->type->kind==TY_F32 || in->type->kind==TY_F64)){
                        load_fp(o,in,0,"d0");
                        fputs("  fneg d0, d0\n",o);
                        store_fp(o,in->dst,"d0");
                    } else {
                        load_int(o,in,0,"x0");
                        fputs("  neg x0, x0\n",o);
                        store_int(o,in->dst,"x0");
                    }
                    break;
                case OP_NOT:
                    load_int(o,in,0,"x0");
                    fputs("  mvn x0, x0\n",o);
                    store_int(o,in->dst,"x0");
                    break;

                case OP_ICMP: {
                    load_int(o,in,0,"x0");
                    load_int(o,in,1,"x1");
                    const char *cc;
                    switch(in->pred){
                        case 0: cc="eq"; break; case 1: cc="ne"; break;
                        case 2: cc="lt"; break; case 3: cc="le"; break;
                        case 4: cc="gt"; break; case 5: cc="ge"; break;
                        case 6: cc="lo"; break; case 7: cc="ls"; break;
                        case 8: cc="hi"; break; default: cc="hs"; break;
                    }
                    fprintf(o,"  cmp x0, x1\n  cset x0, %s\n",cc);
                    store_int(o,in->dst,"x0");
                    break;
                }
                case OP_ZEXT: case OP_SEXT: case OP_TRUNC:
                    load_int(o,in,0,"x0");
                    if(in->op==OP_ZEXT)      fputs("  uxtb w0, w0\n",o);
                    else if(in->op==OP_SEXT) fputs("  sxtb x0, w0\n",o);
                    store_int(o,in->dst,"x0");
                    break;

                case OP_ALLOCA:
                    if(in->dst>=0){
                        fprintf(o,"  add x9, sp, #%u\n",in->pred);
                        fprintf(o,"  str x9, [sp, #%d]\n",slotoff(in->dst));
                    }
                    break;
                case OP_LOAD: {
                    load_int(o,in,0,"x9");
                    int lsz=type_size(in->type);
                    if(lsz==1)fputs("  ldrsb x0, [x9]\n",o);
                    else if(lsz==2)fputs("  ldrsh x0, [x9]\n",o);
                    else if(lsz==4)fputs("  ldrsw x0, [x9]\n",o);
                    else fputs("  ldr x0, [x9]\n",o);
                    store_int(o,in->dst,"x0");
                    break;
                }
                case OP_STORE: {
                    load_int(o,in,0,"x0");
                    load_int(o,in,1,"x9");
                    int ssz=type_size(in->type);
                    if(ssz==1)fputs("  strb w0, [x9]\n",o);
                    else if(ssz==2)fputs("  strh w0, [x9]\n",o);
                    else if(ssz==4)fputs("  str w0, [x9]\n",o);
                    else fputs("  str x0, [x9]\n",o);
                    break;
                }
                case OP_CALL: {
                    static const char *areg2[]={"x0","x1","x2","x3","x4","x5"};
                    for(uint32_t a=0;a<in->nargs&&a<6;a++)
                        load_int(o,in,a,areg2[a]);
                    if(in->callee)fprintf(o,"  bl %s\n",in->callee);
                    store_int(o,in->dst,"x0");
                    break;
                }

                case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: {
                    const char *suf=fpsuf(in->type);
                    load_fp(o,in,0,"d0");
                    load_fp(o,in,1,"d1");
                    const char *mn="fadd";
                    if(in->op==OP_FSUB)mn="fsub";
                    else if(in->op==OP_FMUL)mn="fmul";
                    else if(in->op==OP_FDIV)mn="fdiv";
                    fprintf(o,"  %s %s0, %s0, %s1\n",mn,suf,suf,suf);
                    store_fp(o,in->dst,"d0");
                    break;
                }
                case OP_FNEG:
                    load_fp(o,in,0,"d0");
                    fputs("  fneg d0, d0\n",o);
                    store_fp(o,in->dst,"d0");
                    break;
                case OP_FCMP: {
                    const char *suf=fpsuf(in->type);
                    load_fp(o,in,0,"d0");
                    load_fp(o,in,1,"d1");
                    fprintf(o,"  fcmp %s0, %s1\n",suf,suf);
                    const char *cc;
                    switch(in->pred){
                        case 0: cc="eq"; break; case 1: cc="ne"; break;
                        case 2: cc="mi"; break; case 3: cc="ls"; break;
                        case 4: cc="gt"; break; case 5: cc="ge"; break;
                        default: cc="eq"; break;
                    }
                    fprintf(o,"  cset x0, %s\n",cc);
                    store_int(o,in->dst,"x0");
                    break;
                }

                case OP_CBR:
                    load_int(o,in,0,"x0");
                    fprintf(o,"  cbnz x0, .L%s_%s\n  b .L%s_%s\n",
                            f->name,in->label,f->name,in->label2);
                    break;
                case OP_BR:
                    if(in->label)fprintf(o,"  b .L%s_%s\n",f->name,in->label);
                    break;
                case OP_UNREACHABLE:
                    fputs("  brk #0\n",o);
                    break;
                default: break;
                }
            }
        }
        fprintf(o,".size %s, .-%s\n",f->name,f->name);
    }
    fputs(".section .note.GNU-stack,\"\",%progbits\n",o);
    return 0;
}
