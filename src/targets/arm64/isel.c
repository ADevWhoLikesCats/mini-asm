#include "targets/arm64.h"
#include "backend.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define PARAM_BASE 1000
extern double ir_fpimm[];

#define SCRATCH1 "x9"
#define SCRATCH2 "x10"
#define SCRATCH3 "x15"

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
static int slotoff(int v){ return 8*v; }

/* Operand string for arg i: "#imm", "xN", or "[sp, #N]" */
static const char *op_str(IRInstr *in, int i, const RegAlloc *ra, char *buf, size_t cap){
    if(in->kinds[i]==ARG_IMM){ snprintf(buf,cap,"#%d",in->args[i]); return buf; }
    if(in->kinds[i]==ARG_FP){ snprintf(buf,cap,"#0"); return buf; }
    int v = in->args[i];
    if(ra && v>=0 && v<ra->nvregs && ra->reg_of[v]>=0)
        snprintf(buf,cap,"%s",regalloc_name(ra, ra->reg_of[v]));
    else
        snprintf(buf,cap,"[sp, #%d]",slotoff(v));
    return buf;
}
static const char *vop_str(int v, const RegAlloc *ra, char *buf, size_t cap){
    if(ra && v>=0 && v<ra->nvregs && ra->reg_of[v]>=0)
        snprintf(buf,cap,"%s",regalloc_name(ra, ra->reg_of[v]));
    else
        snprintf(buf,cap,"[sp, #%d]",slotoff(v));
    return buf;
}
/* Load a memory-operand into a register. No-op if op is already the reg. */
static void load_to(FILE *o, const char *ops, const char *reg){
    if(ops[0]=='#'){ fprintf(o,"  mov %s, %s\n", reg, ops); return; }
    if(ops[0]=='['){ fprintf(o,"  ldr %s, %s\n", reg, ops); return; }
    if(strcmp(ops,reg)!=0) fprintf(o,"  mov %s, %s\n", reg, ops);
}
static void store_from(FILE *o, const char *dstops, const char *reg){
    if(dstops[0]=='['){ fprintf(o,"  str %s, %s\n", reg, dstops); return; }
    if(strcmp(dstops,reg)!=0) fprintf(o,"  mov %s, %s\n", dstops, reg);
}
static void canon(FILE *o, IRType *t){
    int sz=type_size(t);
    if(sz==1) fputs("  sxtb x0, w0\n",o);
    else if(sz==2) fputs("  sxth x0, w0\n",o);
    else if(sz==4) fputs("  sxtw x0, w0\n",o);
}

static int is_fp_class(IROpcode op){
    switch(op){
        case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: case OP_FNEG:
        case OP_FCMP:
        case OP_SITOFP: case OP_UITOFP: case OP_FPTOSI: case OP_FPTOUI:
        case OP_FPEXT: case OP_FPTRUNC:
            return 1;
        default: return 0;
    }
}

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
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b=&f->blocks[j];
        if(strcmp(b->name,succ))continue;
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in=&b->instrs[k];
            if(in->op!=OP_PHI || in->dst<0) continue;
            for(int s=0;s<2;s++){
                const char *from = s==0 ? in->label : in->label2;
                if(!from || strcmp(from,pred)) continue;
                char sb[64], db[64];
                const char *src;
                if(in->kinds[s]==ARG_VREG)      src = vop_str(in->args[s], ra, sb, sizeof sb);
                else if(in->kinds[s]==ARG_IMM){ snprintf(sb,sizeof sb,"#%d",in->args[s]); src=sb; }
                else continue;
                const char *dst = vop_str(in->dst, ra, db, sizeof db);
                load_to(o, src, SCRATCH1);
                store_from(o, dst, SCRATCH1);
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
        RegAlloc ra = regalloc_run_target(f, &arm64_reginfo);
        regalloc_dump(&ra, f, o);

        int maxv=count_maxv(f);
        int vreg_bytes=(maxv+1)*8;
        int total=assign_allocas(f, vreg_bytes);
        int framesize=(total+15)&~15;
        if(framesize==0)framesize=16;

        /* Which callee-saved regs in our pool (indices 4..8 → x19..x23) are used? */
        int used_cs[5] = {0};
        for(int v=0;v<ra.nvregs;v++){
            int r = ra.reg_of[v];
            if(r >= 4 && r <= 8) used_cs[r-4] = 1;
        }
        static const char *CS[] = {"x19","x20","x21","x22","x23"};

        fprintf(o,".globl %s\n.type %s, %%function\n%s:\n",f->name,f->name,f->name);
        fputs("  stp x29, x30, [sp, #-16]!\n  mov x29, sp\n", o);
        if(framesize <= 4095)
            fprintf(o,"  sub sp, sp, #%d\n", framesize);
        else{
            fprintf(o,"  mov x9, #%d\n", framesize);
            fputs("  sub sp, sp, x9\n", o);
        }
        /* Push callee-saved used regs in pairs. */
        int pair_idx = 0;
        for(int k=0;k<5;k++){
            if(!used_cs[k]) continue;
            if(pair_idx==0){
                /* stp with next one if available */
                int k2=-1;
                for(int kk=k+1;kk<5;kk++) if(used_cs[kk]){ k2=kk; break; }
                if(k2>=0){
                    fprintf(o,"  stp %s, %s, [sp, #-16]!\n", CS[k], CS[k2]);
                    used_cs[k2]=0;
                } else {
                    fprintf(o,"  str %s, [sp, #-16]!\n", CS[k]);
                }
                pair_idx = 1;
            } else {
                fprintf(o,"  str %s, [sp, #-16]!\n", CS[k]);
            }
            /* Simplify: after each push, mark used_cs[k]=0 */
            used_cs[k]=0;
        }
        /* Recompute used_cs for epilogue */
        int used_cs2[5] = {0};
        for(int v=0;v<ra.nvregs;v++){
            int r = ra.reg_of[v];
            if(r >= 4 && r <= 8) used_cs2[r-4] = 1;
        }

        /* Save incoming args to slots, and load into regs where assigned */
        static const char *AREG[]={"x0","x1","x2","x3","x4","x5","x6","x7"};
        for(uint32_t p=0;p<f->nparams && p<8;p++){
            int pv = PARAM_BASE + (int)p;
            fprintf(o,"  str %s, [sp, #%d]\n", AREG[p], slotoff(pv));
        }
        for(uint32_t p=8;p<f->nparams;p++){
            int pv = PARAM_BASE + (int)p;
            /* args 9+ are on caller stack above [x29+16], need ldr via x29 */
            fprintf(o,"  ldr x9, [x29, #%d]\n  str x9, [sp, #%d]\n",
                    16+(p-8)*8, slotoff(pv));
        }
        for(uint32_t p=0;p<f->nparams;p++){
            int pv = PARAM_BASE + (int)p;
            if(pv < ra.nvregs && ra.reg_of[pv] >= 0){
                const char *r = regalloc_name(&ra, ra.reg_of[pv]);
                fprintf(o,"  ldr %s, [sp, #%d]\n", r, slotoff(pv));
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
                char a0b[64], a1b[64], db[64];
                switch(in->op){
                case OP_RET: {
                    if(in->nargs>=1){
                        const char *s = op_str(in,0,&ra,a0b,sizeof a0b);
                        load_to(o, s, "x0");
                    } else fputs("  mov x0, #0\n",o);
                    /* Epilogue: restore callee-saved, sp, x29, x30 */
                    for(int k=4;k>=0;k--){
                        if(used_cs2[k]) fprintf(o,"  ldr %s, [sp], #16\n", CS[k]);
                    }
                    fputs("  mov sp, x29\n  ldp x29, x30, [sp], #16\n  ret\n", o);
                    break;
                }
                case OP_PHI: break;

                case OP_ADD: case OP_SUB: case OP_MUL:
                case OP_AND: case OP_OR:  case OP_XOR: {
                    const char *dop = (in->dst>=0 && ra.reg_of[in->dst]>=0)
                        ? regalloc_name(&ra, ra.reg_of[in->dst]) : SCRATCH1;
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, dop);
                    /* second operand: load into scratch if it's a memory ref or
                       an immediate that the chosen mnemonic doesn't accept. */
                    int a1_is_imm = (a1[0]=='#');
                    if(a1[0]=='['){ load_to(o,a1,SCRATCH2); a1=SCRATCH2; }
                    else if(a1_is_imm && (in->op==OP_MUL)){
                        /* mul doesn't take immediates */
                        fprintf(o,"  mov %s, %s\n", SCRATCH2, a1); a1=SCRATCH2;
                    }
                    const char *mn="add";
                    if(in->op==OP_SUB)mn="sub";
                    else if(in->op==OP_MUL)mn="mul";
                    else if(in->op==OP_AND)mn="and";
                    else if(in->op==OP_OR) mn="orr";
                    else if(in->op==OP_XOR)mn="eor";
                    fprintf(o,"  %s %s, %s, %s\n", mn, dop, dop, a1);
                    if(sz==1) fputs("  sxtb x0, w0\n",o); /* placeholder */
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, dop);
                    }
                    break;
                }
                case OP_SDIV: case OP_UDIV: case OP_SREM: case OP_UREM: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, SCRATCH1);
                    if(a1[0]=='['){ load_to(o,a1,SCRATCH2); a1=SCRATCH2; }
                    else if(a1[0]=='#'){ fprintf(o,"  mov %s, %s\n", SCRATCH2, a1); a1=SCRATCH2; }
                    const char *dop = (in->dst>=0 && ra.reg_of[in->dst]>=0)
                        ? regalloc_name(&ra, ra.reg_of[in->dst]) : SCRATCH1;
                    if(in->op==OP_SDIV||in->op==OP_UDIV){
                        fprintf(o,"  %s %s, %s, %s\n",
                                in->op==OP_SDIV?"sdiv":"udiv", dop, SCRATCH1, a1);
                    } else {
                        fprintf(o,"  %s %s, %s, %s\n",
                                in->op==OP_SREM?"sdiv":"udiv", SCRATCH3, SCRATCH1, a1);
                        fprintf(o,"  msub %s, %s, %s, %s\n", dop, SCRATCH3, a1, SCRATCH1);
                    }
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, dop);
                    }
                    break;
                }
                case OP_SHL: case OP_LSHR: case OP_ASHR: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    const char *dop = (in->dst>=0 && ra.reg_of[in->dst]>=0)
                        ? regalloc_name(&ra, ra.reg_of[in->dst]) : SCRATCH1;
                    load_to(o, a0, dop);
                    /* For shift counts, we need a reg. */
                    if(a1[0]=='#'){ fprintf(o,"  mov %s, %s\n", SCRATCH2, a1); a1=SCRATCH2; }
                    else if(a1[0]=='['){ load_to(o,a1,SCRATCH2); a1=SCRATCH2; }
                    const char *mn = in->op==OP_SHL?"lsl":(in->op==OP_LSHR?"lsr":"asr");
                    fprintf(o,"  %s %s, %s, %s\n", mn, dop, dop, a1);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, dop);
                    }
                    break;
                }
                case OP_NEG: case OP_NOT: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *dop = (in->dst>=0 && ra.reg_of[in->dst]>=0)
                        ? regalloc_name(&ra, ra.reg_of[in->dst]) : SCRATCH1;
                    load_to(o, a0, dop);
                    fprintf(o,"  %s %s, %s\n", in->op==OP_NEG?"neg":"mvn", dop, dop);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, dop);
                    }
                    break;
                }
                case OP_ICMP: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, SCRATCH1);
                    if(a1[0]=='['){ load_to(o,a1,SCRATCH2); a1=SCRATCH2; }
                    const char *cc;
                    switch(in->pred){
                        case 0: cc="eq"; break; case 1: cc="ne"; break;
                        case 2: cc="lt"; break; case 3: cc="le"; break;
                        case 4: cc="gt"; break; case 5: cc="ge"; break;
                        case 6: cc="lo"; break; case 7: cc="ls"; break;
                        case 8: cc="hi"; break; default: cc="hs"; break;
                    }
                    fprintf(o,"  cmp %s, %s\n  cset %s, %s\n", SCRATCH1, a1, "x0", cc);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, "x0");
                    }
                    break;
                }
                case OP_ZEXT: case OP_SEXT: case OP_TRUNC: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, "x0");
                    if(in->op==OP_SEXT) fputs("  sxtb x0, w0\n",o);
                    else                 fputs("  uxtb w0, w0\n",o);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, "x0");
                    }
                    break;
                }
                case OP_ALLOCA:
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        fprintf(o,"  add x0, sp, #%u\n", in->pred);
                        store_from(o, dd, "x0");
                    }
                    break;
                case OP_LOAD: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, "x0");
                    int lsz=type_size(in->type);
                    if(lsz==1)fputs("  ldrsb x0, [x0]\n",o);
                    else if(lsz==2)fputs("  ldrsh x0, [x0]\n",o);
                    else if(lsz==4)fputs("  ldrsw x0, [x0]\n",o);
                    else fputs("  ldr x0, [x0]\n",o);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, "x0");
                    }
                    break;
                }
                case OP_STORE: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, "x0");
                    load_to(o, a1, "x9");
                    int ssz=type_size(in->type);
                    if(ssz==1)fputs("  strb w0, [x9]\n",o);
                    else if(ssz==2)fputs("  strh w0, [x9]\n",o);
                    else if(ssz==4)fputs("  str w0, [x9]\n",o);
                    else fputs("  str x0, [x9]\n",o);
                    break;
                }
                case OP_GEP: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, SCRATCH1);
                    if(a1[0]=='#'){ fprintf(o,"  mov %s, %s\n", SCRATCH2, a1); a1=SCRATCH2; }
                    else if(a1[0]=='['){ load_to(o,a1,SCRATCH2); a1=SCRATCH2; }
                    unsigned esz=in->pred;
                    if(esz==1) fputs("  add x0, x9, x10\n",o);
                    else if(esz==2) fputs("  add x0, x9, x10, lsl #1\n",o);
                    else if(esz==4) fputs("  add x0, x9, x10, lsl #2\n",o);
                    else if(esz==8) fputs("  add x0, x9, x10, lsl #3\n",o);
                    else { fprintf(o,"  mov x11, #%u\n  madd x0, x10, x11, x9\n",esz); }
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, "x0");
                    }
                    break;
                }
                case OP_GEP_FIELD: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, "x0");
                    fprintf(o,"  add x0, x0, #%u\n", in->pred);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, "x0");
                    }
                    break;
                }
                case OP_STR: {
                    uint32_t idx=(uint32_t)in->args[0];
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        fprintf(o,"  adrp x0, %s\n  add x0, x0, :lo12:%s\n",
                                m->strings[idx].label, m->strings[idx].label);
                        store_from(o, dd, "x0");
                    }
                    break;
                }
                case OP_CALL: {
                    static const char *AREG2[]={"x0","x1","x2","x3","x4","x5","x6","x7"};
                    uint32_t n = in->nargs;
                    /* Copy arguments into ABI regs. Handle stack args (>8). */
                    uint32_t stackargs = n > 8 ? n - 8 : 0;
                    if(stackargs){
                        for(int a=(int)n-1; a>=8; a--){
                            const char *s = op_str(in,a,&ra,a0b,sizeof a0b);
                            load_to(o, s, "x9");
                            fprintf(o,"  str x9, [sp, #-16]!\n");
                        }
                    }
                    for(uint32_t a=0;a<8&&a<n;a++){
                        const char *s = op_str(in,a,&ra,a0b,sizeof a0b);
                        load_to(o, s, AREG2[a]);
                    }
                    if(in->callee) fprintf(o,"  bl %s\n", in->callee);
                    if(stackargs){
                        fprintf(o,"  add sp, sp, #%u\n", stackargs*16);
                    }
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, "x0");
                    }
                    break;
                }
                case OP_CBR: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, SCRATCH1);
                    int need_t = phi_copies_needed(f, b->name, in->label);
                    int need_f = phi_copies_needed(f, b->name, in->label2);
                    if(!need_t && !need_f){
                        fprintf(o,"  cbnz %s, .L%s_%s\n  b .L%s_%s\n",
                                SCRATCH1, f->name, in->label, f->name, in->label2);
                    } else {
                        fprintf(o,"  cbnz %s, .L%s_%s__ct_%u\n", SCRATCH1, f->name, b->name, k);
                        fprintf(o,".L%s_%s__fall_%u:\n", f->name, b->name, k);
                        if(need_f) emit_phi_copies_for_edge(o,f,b->name,in->label2,&ra);
                        fprintf(o,"  b .L%s_%s\n", f->name, in->label2);
                        fprintf(o,".L%s_%s__ct_%u:\n", f->name, b->name, k);
                        if(need_t) emit_phi_copies_for_edge(o,f,b->name,in->label,&ra);
                        fprintf(o,"  b .L%s_%s\n", f->name, in->label);
                    }
                    break;
                }
                case OP_BR:
                    if(in->label)fprintf(o,"  b .L%s_%s\n",f->name,in->label);
                    break;
                case OP_UNREACHABLE: fputs("  brk #0\n",o); break;

                /* FP ops stay slot-based. */
                case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: {
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[0]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movz x9, #%u\n  movk x9, #%u, lsl #16\n  movk x9, #%u, lsl #32\n  movk x9, #%u, lsl #48\n  fmov d0, x9\n",
                            (unsigned)(bits&0xffff),(unsigned)((bits>>16)&0xffff),
                            (unsigned)((bits>>32)&0xffff),(unsigned)((bits>>48)&0xffff));
                    } else fprintf(o,"  ldr d0, [sp, #%d]\n",slotoff(in->args[0]));
                    if(in->kinds[1]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[1]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movz x9, #%u\n  movk x9, #%u, lsl #16\n  movk x9, #%u, lsl #32\n  movk x9, #%u, lsl #48\n  fmov d1, x9\n",
                            (unsigned)(bits&0xffff),(unsigned)((bits>>16)&0xffff),
                            (unsigned)((bits>>32)&0xffff),(unsigned)((bits>>48)&0xffff));
                    } else fprintf(o,"  ldr d1, [sp, #%d]\n",slotoff(in->args[1]));
                    const char *suf=(in->type&&in->type->kind==TY_F32)?"s":"d";
                    const char *mn="fadd";
                    if(in->op==OP_FSUB)mn="fsub";
                    else if(in->op==OP_FMUL)mn="fmul";
                    else if(in->op==OP_FDIV)mn="fdiv";
                    fprintf(o,"  %s %s0, %s0, %s1\n", mn, suf, suf, suf);
                    if(in->dst>=0) fprintf(o,"  str d0, [sp, #%d]\n", slotoff(in->dst));
                    break;
                }
                case OP_FNEG: {
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[0]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movz x9, #%u\n  movk x9, #%u, lsl #16\n  movk x9, #%u, lsl #32\n  movk x9, #%u, lsl #48\n  fmov d0, x9\n",
                            (unsigned)(bits&0xffff),(unsigned)((bits>>16)&0xffff),
                            (unsigned)((bits>>32)&0xffff),(unsigned)((bits>>48)&0xffff));
                    } else fprintf(o,"  ldr d0, [sp, #%d]\n",slotoff(in->args[0]));
                    fputs("  fneg d0, d0\n",o);
                    if(in->dst>=0) fprintf(o,"  str d0, [sp, #%d]\n", slotoff(in->dst));
                    break;
                }
                case OP_FCMP: {
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[0]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movz x9, #%u\n  movk x9, #%u, lsl #16\n  movk x9, #%u, lsl #32\n  movk x9, #%u, lsl #48\n  fmov d0, x9\n",
                            (unsigned)(bits&0xffff),(unsigned)((bits>>16)&0xffff),
                            (unsigned)((bits>>32)&0xffff),(unsigned)((bits>>48)&0xffff));
                    } else fprintf(o,"  ldr d0, [sp, #%d]\n",slotoff(in->args[0]));
                    if(in->kinds[1]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[1]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movz x9, #%u\n  movk x9, #%u, lsl #16\n  movk x9, #%u, lsl #32\n  movk x9, #%u, lsl #48\n  fmov d1, x9\n",
                            (unsigned)(bits&0xffff),(unsigned)((bits>>16)&0xffff),
                            (unsigned)((bits>>32)&0xffff),(unsigned)((bits>>48)&0xffff));
                    } else fprintf(o,"  ldr d1, [sp, #%d]\n",slotoff(in->args[1]));
                    const char *suf=(in->type&&in->type->kind==TY_F32)?"s":"d";
                    fprintf(o,"  fcmp %s0, %s1\n", suf, suf);
                    const char *cc;
                    switch(in->pred){
                        case 0: cc="eq"; break; case 1: cc="ne"; break;
                        case 2: cc="mi"; break; case 3: cc="ls"; break;
                        case 4: cc="gt"; break; case 5: cc="ge"; break;
                        default: cc="eq"; break;
                    }
                    fprintf(o,"  cset x0, %s\n", cc);
                    if(in->dst>=0) fprintf(o,"  str x0, [sp, #%d]\n", slotoff(in->dst));
                    break;
                }
                case OP_SITOFP: case OP_UITOFP: case OP_FPTOSI: case OP_FPTOUI:
                case OP_FPEXT: case OP_FPTRUNC: {
                    int src_is_fp = (in->pred==TY_F32 || in->pred==TY_F64);
                    if(src_is_fp){
                        if(in->kinds[0]==ARG_FP){
                            uint64_t bits; double d=ir_fpimm[in->args[0]];
                            memcpy(&bits,&d,8);
                            fprintf(o,"  movz x9, #%u\n  movk x9, #%u, lsl #16\n  movk x9, #%u, lsl #32\n  movk x9, #%u, lsl #48\n  fmov d0, x9\n",
                                (unsigned)(bits&0xffff),(unsigned)((bits>>16)&0xffff),
                                (unsigned)((bits>>32)&0xffff),(unsigned)((bits>>48)&0xffff));
                        } else fprintf(o,"  ldr d0, [sp, #%d]\n",slotoff(in->args[0]));
                    } else if(in->kinds[0]==ARG_IMM){
                        fprintf(o,"  mov x0, #%d\n", in->args[0]);
                    } else {
                        fprintf(o,"  ldr x0, [sp, #%d]\n",slotoff(in->args[0]));
                    }
                    int dt = in->type ? in->type->kind : TY_F64;
                    if(in->op==OP_SITOFP){
                        if(dt==TY_F32)fputs("  scvtf s0, x0\n",o); else fputs("  scvtf d0, x0\n",o);
                    } else if(in->op==OP_UITOFP){
                        if(dt==TY_F32)fputs("  ucvtf s0, x0\n",o); else fputs("  ucvtf d0, x0\n",o);
                    } else if(in->op==OP_FPTOSI){
                        if(in->pred==TY_F32)fputs("  fcvtzs x0, s0\n",o); else fputs("  fcvtzs x0, d0\n",o);
                    } else if(in->op==OP_FPTOUI){
                        if(in->pred==TY_F32)fputs("  fcvtzu x0, s0\n",o); else fputs("  fcvtzu x0, d0\n",o);
                    } else if(in->op==OP_FPEXT){
                        fputs("  fcvt d0, s0\n",o);
                    } else if(in->op==OP_FPTRUNC){
                        fputs("  fcvt s0, d0\n",o);
                    }
                    if(in->dst>=0){
                        int produces_fp = (in->op==OP_SITOFP||in->op==OP_UITOFP||in->op==OP_FPEXT||in->op==OP_FPTRUNC);
                        if(produces_fp) fprintf(o,"  str d0, [sp, #%d]\n", slotoff(in->dst));
                        else            fprintf(o,"  str x0, [sp, #%d]\n", slotoff(in->dst));
                    }
                    break;
                }
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
    int has_main = 0;
    for(uint32_t i=0;i<m->nfuncs;i++) if(!strcmp(m->funcs[i].name,"main")){ has_main=1; break; }
    if(has_main){
        fputs(".globl _start\n.type _start, %function\n_start:\n", o);
        fputs("  bl main\n", o);
        fputs("  mov x8, #93\n  svc #0\n", o);
        fputs(".size _start, .-_start\n", o);
    }
    fputs(".section .note.GNU-stack,\"\",%progbits\n",o);
    return 0;
}
