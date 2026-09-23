#include "targets/arm.h"
#include "backend.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define PARAM_BASE 1000
extern double ir_fpimm[];

#define SCRATCH1 "r0"
#define SCRATCH2 "r1"
#define SCRATCH3 "r2"
#define SCRATCH4 "r12"

static int type_size(IRType *t){
    if(!t)return 4;
    switch(t->kind){
        case TY_I8:  return 1;
        case TY_I16: return 2;
        case TY_I32: return 4;
        case TY_I64: case TY_PTR: return 4;
        case TY_F32: return 4;
        case TY_F64: return 8;
        default: return 4;
    }
}
/* 4-byte slots: vreg v lives at [sp, #4*v] */
static int slotoff(int v){ return 4*v; }

static const char *op_str(IRInstr *in, int i, const RegAlloc *ra, char *buf, size_t cap){
    if(in->kinds[i]==ARG_IMM){ snprintf(buf,cap,"#%d",in->args[i]); return buf; }
    if(in->kinds[i]==ARG_FP){ snprintf(buf,cap,"#0"); return buf; }
    int v = in->args[i];
    if(ra && v>=0 && v<ra->nvregs && ra->reg_of[v]>=0)
        snprintf(buf,cap,"%s",regalloc_name(ra,ra->reg_of[v]));
    else
        snprintf(buf,cap,"[sp, #%d]",slotoff(v));
    return buf;
}
static const char *vop_str(int v, const RegAlloc *ra, char *buf, size_t cap){
    if(ra && v>=0 && v<ra->nvregs && ra->reg_of[v]>=0)
        snprintf(buf,cap,"%s",regalloc_name(ra,ra->reg_of[v]));
    else
        snprintf(buf,cap,"[sp, #%d]",slotoff(v));
    return buf;
}
/* Load op into reg. op can be "#imm", "rN", or "[sp, #N]". */
static void load_to(FILE *o, const char *ops, const char *reg){
    if(ops[0]=='['){ fprintf(o,"  ldr %s, %s\n", reg, ops); return; }
    if(ops[0]=='#'){ fprintf(o,"  mov %s, %s\n", reg, ops); return; }
    if(strcmp(ops,reg)!=0) fprintf(o,"  mov %s, %s\n", reg, ops);
}
/* Store reg into op. op can be "rN" or "[sp, #N]". */
static void store_from(FILE *o, const char *ops, const char *reg){
    if(ops[0]=='['){ fprintf(o,"  str %s, %s\n", reg, ops); return; }
    if(strcmp(ops,reg)!=0) fprintf(o,"  mov %s, %s\n", ops, reg);
}
/* Sign-extend canonical i8/i16/i32 values in a register. */
static void canon(FILE *o, IRType *t, const char *reg){
    int sz=type_size(t);
    if(sz==1) fprintf(o,"  sxtb %s, %s\n", reg, reg);
    else if(sz==2) fprintf(o,"  sxth %s, %s\n", reg, reg);
    /* ARM: 32-bit ops naturally zero-extend to 32 bits — nothing to do for i32. */
}

static int is_imm_str(const char *s){ return s[0]=='#'; }

/* icmp predicate → ARM condition suffix for "true" case.
   We emit: movCC reg, #1 ; movNC reg, #0 */
static const char *arm_cc(int pred){
    switch(pred){
        case 0: return "eq"; /* eq */
        case 1: return "ne";
        case 2: return "lt"; /* slt */
        case 3: return "le";
        case 4: return "gt";
        case 5: return "ge";
        case 6: return "lo"; /* ult */
        case 7: return "ls";
        case 8: return "hi";
        default:return "hs"; /* uge */
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
            sz=(sz+7)&~7;
            cursor+=sz;
            in->pred=(uint32_t)cursor;
        }
    }
    return cursor;
}

int arm_emit(IRModule *m, FILE *o, const TargetDesc *t){
    (void)t;
    fputs(".text\n.syntax unified\n.arch armv7-a\n.fpu vfpv3-d16\n", o);
    for(uint32_t i=0;i<m->nfuncs;i++){
        IRFunc *f=&m->funcs[i];
        RegAlloc ra = regalloc_run_target(f, &arm_reginfo);
        regalloc_dump(&ra, f, o);

        int maxv=count_maxv(f);
        int vreg_bytes=(maxv+1)*4;

        int used_cs[8]={0}; /* r4..r11 = pool indices 0..7 */
        for(int v=0;v<ra.nvregs;v++){
            int r = ra.reg_of[v];
            if(r >= 0 && r <= 7) used_cs[r]=1;
        }
        int total = assign_allocas(f, vreg_bytes);
        /* Prologue pushes 9 registers (r4-r11, lr) = 36 bytes. To keep sp
           8-aligned at the ABI boundary, we need (36 + framesize) % 8 == 0,
           so framesize % 8 == 4. */
        int framesize = ((total + 3) & ~3) + 4;
        if(framesize < 4) framesize = 4;

        fprintf(o,".globl %s\n.type %s, %%function\n%s:\n",f->name,f->name,f->name);
        fputs("  push {r4-r11, lr}\n", o);
        if(framesize){
            fprintf(o,"  movw r12, #%u\n", (unsigned)(framesize & 0xffff));
            if(framesize > 0xffff)
                fprintf(o,"  movt r12, #%u\n", (unsigned)((framesize>>16) & 0xffff));
            fputs("  sub sp, sp, r12\n", o);
        }

        /* Save incoming args to their slots, then optionally to registers.
           Args 0-3 in r0-r3, args 4+ on caller's stack at
           [sp, #framesize + 36 + 4*(p-4)]. */
        static const char *AREG[]={"r0","r1","r2","r3"};
        for(uint32_t p=0;p<f->nparams && p<4;p++){
            int pv = PARAM_BASE + (int)p;
            fprintf(o,"  str %s, [sp, #%d]\n", AREG[p], slotoff(pv));
        }
        for(uint32_t p=4;p<f->nparams;p++){
            int pv = PARAM_BASE + (int)p;
            int off = framesize + 36 + 4*((int)p-4);
            fprintf(o,"  ldr r0, [sp, #%d]\n  str r0, [sp, #%d]\n", off, slotoff(pv));
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

                char a0b[64], a1b[64], db[64];
                switch(in->op){
                case OP_RET: {
                    if(in->nargs>=1){
                        const char *s = op_str(in,0,&ra,a0b,sizeof a0b);
                        load_to(o, s, "r0");
                    } else fputs("  mov r0, #0\n", o);
                    fprintf(o,"  movw r12, #%u\n", (unsigned)(framesize & 0xffff));
                    if(framesize > 0xffff)
                        fprintf(o,"  movt r12, #%u\n", (unsigned)((framesize>>16) & 0xffff));
                    fputs("  add sp, sp, r12\n", o);
                    fputs("  pop {r4-r11, pc}\n", o);
                    break;
                }
                case OP_PHI: break;

                case OP_ADD: case OP_SUB: case OP_MUL:
                case OP_AND: case OP_OR:  case OP_XOR: {
                    const char *dop = (in->dst>=0 && ra.reg_of[in->dst]>=0)
                        ? regalloc_name(&ra,ra.reg_of[in->dst]) : SCRATCH1;
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, dop);
                    /* ARM: add/sub/and/orr/eor accept a second register or #imm.
                       mul only accepts registers. */
                    if(in->op==OP_MUL && a1[0]=='#'){
                        fprintf(o,"  mov %s, %s\n", SCRATCH2, a1);
                        a1 = SCRATCH2;
                    }
                    if(a1[0]=='['){
                        fprintf(o,"  ldr %s, %s\n", SCRATCH2, a1);
                        a1 = SCRATCH2;
                    }
                    const char *mn="add";
                    if(in->op==OP_SUB)mn="sub";
                    else if(in->op==OP_MUL)mn="mul";
                    else if(in->op==OP_AND)mn="and";
                    else if(in->op==OP_OR) mn="orr";
                    else if(in->op==OP_XOR)mn="eor";
                    fprintf(o,"  %s %s, %s, %s\n", mn, dop, dop, a1);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, dop);
                    }
                    break;
                }
                case OP_SDIV: case OP_UDIV: case OP_SREM: case OP_UREM: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, "r0");
                    load_to(o, a1, "r1");
                    /* Call __aeabi_{i,u}divmod. Returns quotient in r0, remainder in r1. */
                    const char *fn = (in->op==OP_SDIV||in->op==OP_SREM)
                                     ? "__aeabi_idivmod" : "__aeabi_uidivmod";
                    fprintf(o,"  bl %s\n", fn);
                    const char *res = (in->op==OP_SREM||in->op==OP_UREM) ? "r1" : "r0";
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, res);
                    }
                    break;
                }
                case OP_SHL: case OP_LSHR: case OP_ASHR: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    const char *dop = (in->dst>=0 && ra.reg_of[in->dst]>=0)
                        ? regalloc_name(&ra,ra.reg_of[in->dst]) : SCRATCH1;
                    load_to(o, a0, dop);
                    if(a1[0]=='['){ fprintf(o,"  ldr %s, %s\n", SCRATCH2, a1); a1=SCRATCH2; }
                    else if(a1[0]=='#'){ fprintf(o,"  mov %s, %s\n", SCRATCH2, a1); a1=SCRATCH2; }
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
                        ? regalloc_name(&ra,ra.reg_of[in->dst]) : SCRATCH1;
                    load_to(o, a0, dop);
                    if(in->op==OP_NEG) fprintf(o,"  rsb %s, %s, #0\n", dop, dop);
                    else               fprintf(o,"  mvn %s, %s\n", dop, dop);
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
                    if(a1[0]=='['){ fprintf(o,"  ldr %s, %s\n", SCRATCH2, a1); a1=SCRATCH2; }
                    fprintf(o,"  cmp %s, %s\n", SCRATCH1, a1);
                    const char *cc = arm_cc(in->pred);
                    fprintf(o,"  mov%s r3, #1\n  mov%s r3, #0\n", cc, /* opposite */ 
                            (!strcmp(cc,"eq"))?"ne":
                            (!strcmp(cc,"ne"))?"eq":
                            (!strcmp(cc,"lt"))?"ge":
                            (!strcmp(cc,"le"))?"gt":
                            (!strcmp(cc,"gt"))?"le":
                            (!strcmp(cc,"ge"))?"lt":
                            (!strcmp(cc,"lo"))?"hs":
                            (!strcmp(cc,"ls"))?"hi":
                            (!strcmp(cc,"hi"))?"ls":"lo");
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, "r3");
                    }
                    break;
                }
                case OP_ZEXT: case OP_SEXT: case OP_TRUNC: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, SCRATCH1);
                    if(in->op==OP_SEXT) fputs("  sxtb r0, r0\n",o);
                    else                fputs("  uxtb r0, r0\n",o);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                }
                case OP_ALLOCA:
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        /* allocas are at [sp + total_after_allocas ...] but the
                           address itself is [sp + cursor]. in->pred holds cursor. */
                        unsigned ao = in->pred;
                        if(ao <= 4095)
                            fprintf(o,"  add %s, sp, #%u\n", SCRATCH1, ao);
                        else {
                            fprintf(o,"  movw r12, #%u\n  movt r12, #%u\n  add %s, sp, r12\n",
                                    (unsigned)(ao & 0xffff), (unsigned)((ao>>16) & 0xffff), SCRATCH1);
                        }
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                case OP_LOAD: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, SCRATCH1);
                    int lsz=type_size(in->type);
                    if(lsz==1)fputs("  ldrsb r0, [r0]\n",o);
                    else if(lsz==2)fputs("  ldrsh r0, [r0]\n",o);
                    else fputs("  ldr r0, [r0]\n",o);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                }
                case OP_STORE: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, "r0");
                    load_to(o, a1, "r1");
                    int ssz=type_size(in->type);
                    if(ssz==1)fputs("  strb r0, [r1]\n",o);
                    else if(ssz==2)fputs("  strh r0, [r1]\n",o);
                    else fputs("  str r0, [r1]\n",o);
                    break;
                }
                case OP_GEP: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, SCRATCH1);
                    if(a1[0]=='['){ fprintf(o,"  ldr %s, %s\n", SCRATCH2, a1); a1=SCRATCH2; }
                    else if(a1[0]=='#'){ fprintf(o,"  mov %s, %s\n", SCRATCH2, a1); a1=SCRATCH2; }
                    unsigned esz=in->pred;
                    if(esz==1) fputs("  add r0, r0, r1\n",o);
                    else if(esz==2) fputs("  add r0, r0, r1, lsl #1\n",o);
                    else if(esz==4) fputs("  add r0, r0, r1, lsl #2\n",o);
                    else if(esz==8) fputs("  add r0, r0, r1, lsl #3\n",o);
                    else {
                        fprintf(o,"  mov %s, #%u\n  mul %s, %s, %s\n  add r0, r0, %s\n",
                                SCRATCH3, esz, SCRATCH3, SCRATCH2, SCRATCH3, SCRATCH3);
                    }
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                }
                case OP_MEMCPY: {
                    char a0b[64], a1b[64];
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    int size = in->args[2];
                    load_to(o, a0, "r2");
                    load_to(o, a1, "r3");
                    int n4 = size / 4, rem = size % 4;
                    for(int i=0;i<n4;i++){
                        fprintf(o,"  ldr r12, [r3, #%d]\n  str r12, [r2, #%d]\n", i*4, i*4);
                    }
                    for(int i=0;i<rem;i++){
                        int off = n4*4 + i;
                        fprintf(o,"  ldrb r12, [r3, #%d]\n  strb r12, [r2, #%d]\n", off, off);
                    }
                    break;
                }
                case OP_GEP_FIELD: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, SCRATCH1);
                    fprintf(o,"  add %s, %s, #%u\n", SCRATCH1, SCRATCH1, in->pred);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                }
                case OP_STR: {
                    uint32_t idx=(uint32_t)in->args[0];
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        fprintf(o,"  ldr r0, =%s\n", m->strings[idx].label);
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                }
                case OP_CALL: {
                    static const char *AREG2[]={"r0","r1","r2","r3"};
                    uint32_t n = in->nargs;
                    /* Push args 4+ right-to-left onto the stack. */
                    if(n > 4){
                        int sn = (int)n - 4;
                        /* align sp to 8 */
                        int pad = (sn & 1) ? 1 : 0;
                        if(pad) fputs("  sub sp, sp, #4\n", o);
                        for(int a=(int)n-1; a>=4; a--){
                            const char *s = op_str(in,a,&ra,a0b,sizeof a0b);
                            load_to(o, s, "r0");
                            fputs("  push {r0}\n", o);
                        }
                    }
                    for(uint32_t a=0;a<4 && a<n;a++){
                        const char *s = op_str(in,a,&ra,a0b,sizeof a0b);
                        load_to(o, s, AREG2[a]);
                    }
                    if(in->callee) fprintf(o,"  bl %s\n", in->callee);
                    if(n > 4){
                        int sn = (int)n - 4;
                        int pad = (sn & 1) ? 1 : 0;
                        fprintf(o,"  add sp, sp, #%d\n", 4*(sn+pad));
                    }
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, "r0");
                    }
                    break;
                }
                case OP_CBR: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, SCRATCH1);
                    int need_t = phi_copies_needed(f, b->name, in->label);
                    int need_f = phi_copies_needed(f, b->name, in->label2);
                    if(!need_t && !need_f){
                        fprintf(o,"  cmp %s, #0\n  bne .L%s_%s\n  b .L%s_%s\n",
                                SCRATCH1, f->name, in->label, f->name, in->label2);
                    } else {
                        fprintf(o,"  cmp %s, #0\n", SCRATCH1);
                        fprintf(o,"  bne .L%s_%s__ct_%u\n", f->name, b->name, k);
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
                case OP_UNREACHABLE: fputs("  udf #0\n",o); break;

                /* FP via VFP (d0, d1 scratch, slot-based). */
                case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: {
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[0]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movw r0, #%u\n  movt r0, #%u\n  movw r1, #%u\n  movt r1, #%u\n  vmov d0, r0, r1\n",
                            (unsigned)(bits&0xffff),(unsigned)((bits>>16)&0xffff),
                            (unsigned)((bits>>32)&0xffff),(unsigned)((bits>>48)&0xffff));
                    } else fprintf(o,"  vldr d0, [sp, #%d]\n",slotoff(in->args[0]));
                    if(in->kinds[1]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[1]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movw r0, #%u\n  movt r0, #%u\n  movw r1, #%u\n  movt r1, #%u\n  vmov d1, r0, r1\n",
                            (unsigned)(bits&0xffff),(unsigned)((bits>>16)&0xffff),
                            (unsigned)((bits>>32)&0xffff),(unsigned)((bits>>48)&0xffff));
                    } else fprintf(o,"  vldr d1, [sp, #%d]\n",slotoff(in->args[1]));
                    const char *mn="vadd";
                    if(in->op==OP_FSUB)mn="vsub";
                    else if(in->op==OP_FMUL)mn="vmul";
                    else if(in->op==OP_FDIV)mn="vdiv";
                    fprintf(o,"  %s.f64 d0, d0, d1\n", mn);
                    if(in->dst>=0) fprintf(o,"  vstr d0, [sp, #%d]\n", slotoff(in->dst));
                    break;
                }
                case OP_FNEG: {
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[0]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movw r0, #%u\n  movt r0, #%u\n  movw r1, #%u\n  movt r1, #%u\n  vmov d0, r0, r1\n",
                            (unsigned)(bits&0xffff),(unsigned)((bits>>16)&0xffff),
                            (unsigned)((bits>>32)&0xffff),(unsigned)((bits>>48)&0xffff));
                    } else fprintf(o,"  vldr d0, [sp, #%d]\n",slotoff(in->args[0]));
                    fputs("  vneg.f64 d0, d0\n", o);
                    if(in->dst>=0) fprintf(o,"  vstr d0, [sp, #%d]\n", slotoff(in->dst));
                    break;
                }
                case OP_FCMP: {
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[0]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movw r0, #%u\n  movt r0, #%u\n  movw r1, #%u\n  movt r1, #%u\n  vmov d0, r0, r1\n",
                            (unsigned)(bits&0xffff),(unsigned)((bits>>16)&0xffff),
                            (unsigned)((bits>>32)&0xffff),(unsigned)((bits>>48)&0xffff));
                    } else fprintf(o,"  vldr d0, [sp, #%d]\n",slotoff(in->args[0]));
                    if(in->kinds[1]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm[in->args[1]];
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movw r0, #%u\n  movt r0, #%u\n  movw r1, #%u\n  movt r1, #%u\n  vmov d1, r0, r1\n",
                            (unsigned)(bits&0xffff),(unsigned)((bits>>16)&0xffff),
                            (unsigned)((bits>>32)&0xffff),(unsigned)((bits>>48)&0xffff));
                    } else fprintf(o,"  vldr d1, [sp, #%d]\n",slotoff(in->args[1]));
                    fputs("  vcmp.f64 d0, d1\n  vmrs APSR_nzcv, fpscr\n", o);
                    /* crude: only handle oeq via eq, everything else defaults to eq for now */
                    fputs("  moveq r0, #1\n  movne r0, #0\n", o);
                    if(in->dst>=0) fprintf(o,"  str r0, [sp, #%d]\n", slotoff(in->dst));
                    break;
                }
                case OP_SITOFP: case OP_UITOFP: case OP_FPTOSI: case OP_FPTOUI:
                case OP_FPEXT: case OP_FPTRUNC: {
                    int src_is_fp = (in->pred==TY_F32 || in->pred==TY_F64);
                    if(src_is_fp){
                        if(in->kinds[0]==ARG_FP){
                            uint64_t bits; double d=ir_fpimm[in->args[0]];
                            memcpy(&bits,&d,8);
                            fprintf(o,"  movw r0, #%u\n  movt r0, #%u\n  movw r1, #%u\n  movt r1, #%u\n  vmov d0, r0, r1\n",
                                (unsigned)(bits&0xffff),(unsigned)((bits>>16)&0xffff),
                                (unsigned)((bits>>32)&0xffff),(unsigned)((bits>>48)&0xffff));
                        } else fprintf(o,"  vldr d0, [sp, #%d]\n",slotoff(in->args[0]));
                    } else if(in->kinds[0]==ARG_IMM){
                        fprintf(o,"  mov r0, #%d\n", in->args[0]);
                    } else {
                        fprintf(o,"  ldr r0, [sp, #%d]\n",slotoff(in->args[0]));
                    }
                    int dt = in->type ? in->type->kind : TY_F64;
                    if(in->op==OP_SITOFP){
                        fputs("  vmov s0, r0\n  vcvt.f64.s32 d0, s0\n", o);
                    } else if(in->op==OP_UITOFP){
                        fputs("  vmov s0, r0\n  vcvt.f64.u32 d0, s0\n", o);
                    } else if(in->op==OP_FPTOSI){
                        fputs("  vcvt.s32.f64 s0, d0\n  vmov r0, s0\n", o);
                    } else if(in->op==OP_FPTOUI){
                        fputs("  vcvt.u32.f64 s0, d0\n  vmov r0, s0\n", o);
                    } else if(in->op==OP_FPEXT){
                        fputs("  vcvt.f64.f32 d0, s0\n", o);
                    } else if(in->op==OP_FPTRUNC){
                        fputs("  vcvt.f32.f64 s0, d0\n", o);
                    }
                    if(in->dst>=0){
                        int produces_fp = (in->op==OP_SITOFP||in->op==OP_UITOFP||in->op==OP_FPEXT||in->op==OP_FPTRUNC);
                        if(produces_fp) fprintf(o,"  vstr d0, [sp, #%d]\n", slotoff(in->dst));
                        else            fprintf(o,"  str r0, [sp, #%d]\n", slotoff(in->dst));
                    }
                    break;
                }
                default: break;
                }
            }
        }
        fprintf(o,".size %s, .-%s\n",f->name,f->name);
    }
    /* _start */
    int has_main=0;
    for(uint32_t i=0;i<m->nfuncs;i++) if(!strcmp(m->funcs[i].name,"main")){ has_main=1; break; }
    if(has_main){
        fputs(".globl _start\n.type _start, %function\n_start:\n", o);
        fputs("  bl main\n", o);
        fputs("  mov r7, #1\n  svc #0\n", o);
        fputs(".size _start, .-_start\n", o);
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
    /* Software integer division helpers (__aeabi_idivmod, __aeabi_uidivmod).
       Implemented in ARM assembly. Quotient -> r0, remainder -> r1. */
    fputs(".section .text.__aeabi_divmod,\"ax\",%progbits\n", o);
    /* unsigned divmod */
    fputs("__aeabi_uidivmod:\n"
          "  cmp r1, #0\n"
          "  beq __aeabi_uidivmod_zero\n"
          "  mov r2, #0\n"
          "1:\n"
          "  cmp r0, r1\n"
          "  blo 2f\n"
          "  sub r0, r0, r1\n"
          "  add r2, r2, #1\n"
          "  b 1b\n"
          "2:\n"
          "  mov r1, r0\n"
          "  mov r0, r2\n"
          "  bx lr\n"
          "__aeabi_uidivmod_zero:\n"
          "  mov r0, #0\n  mov r1, #0\n  bx lr\n", o);
    /* signed divmod: handle signs, call unsigned, restore signs */
    fputs("__aeabi_idivmod:\n"
          "  push {r4, lr}\n"
          "  mov r4, #0\n"
          "  cmp r0, #0\n"
          "  rsblt r0, r0, #0\n"
          "  eorlt r4, r4, #1\n"
          "  cmp r1, #0\n"
          "  rsblt r1, r1, #0\n"
          "  eorlt r4, r4, #1\n"
          "  mov r2, r4\n"
          "  push {r2}\n"
          "  bl __aeabi_uidivmod\n"
          "  pop {r2}\n"
          "  cmp r2, #0\n"
          "  rsbne r0, r0, #0\n"
          "  rsbne r1, r1, #0\n"
          "  pop {r4, pc}\n", o);
    fputs(".section .note.GNU-stack,\"\",%progbits\n",o);
    return 0;
}
