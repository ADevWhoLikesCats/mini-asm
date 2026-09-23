#include "targets/riscv.h"
#include "backend.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define PARAM_BASE 1000


#define SCRATCH1 "t0"
#define SCRATCH2 "t1"
#define SCRATCH3 "t6"

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
/* Vreg v lives at sp + 8*v (positive offset). */
static int slotoff(int v){ return 8*v; }

static const char *op_str(IRInstr *in, int i, const RegAlloc *ra, char *buf, size_t cap){
    if(in->kinds[i]==ARG_IMM){ snprintf(buf,cap,"%d",in->args[i]); return buf; }
    if(in->kinds[i]==ARG_FP){ snprintf(buf,cap,"0"); return buf; }
    int v = in->args[i];
    if(ra && v>=0 && v<ra->nvregs && ra->reg_of[v]>=0){
        snprintf(buf,cap,"%s",regalloc_name(ra,ra->reg_of[v]));
    } else {
        snprintf(buf,cap,"%d(sp)",slotoff(v));
    }
    return buf;
}
static const char *vop_str(int v, const RegAlloc *ra, char *buf, size_t cap){
    if(ra && v>=0 && v<ra->nvregs && ra->reg_of[v]>=0)
        snprintf(buf,cap,"%s",regalloc_name(ra,ra->reg_of[v]));
    else
        snprintf(buf,cap,"%d(sp)",slotoff(v));
    return buf;
}
/* RISC-V has no mem operand; op_str returns either "N" (imm), "reg", or "N(sp)". */
static int is_mem(const char *s){ return strchr(s,'(') != NULL; }
static int is_imm(const char *s){
    if(!s || !*s) return 0;
    int i = (s[0]=='-') ? 1 : 0;
    if(!s[i]) return 0;
    for(; s[i]; i++) if(s[i]<'0'||s[i]>'9') return 0;
    return 1;
}

static void load_to(FILE *o, const char *ops, const char *reg){
    if(is_mem(ops)) fprintf(o,"  ld %s, %s\n", reg, ops);
    else if(is_imm(ops)) fprintf(o,"  li %s, %s\n", reg, ops);
    else if(strcmp(ops,reg)!=0) fprintf(o,"  mv %s, %s\n", reg, ops);
}
static void store_from(FILE *o, const char *dstops, const char *reg){
    if(is_mem(dstops)){ fprintf(o,"  sd %s, %s\n", reg, dstops); return; }
    if(strcmp(dstops,reg)!=0) fprintf(o,"  mv %s, %s\n", dstops, reg);
}
/* Store/load register to/from sp+N(sp), handling large offsets via a scratch. */
static void sd_sp(FILE *o, const char *reg, int off){
    if(off >= -2048 && off <= 2047){
        fprintf(o,"  sd %s, %d(sp)\n", reg, off);
    } else {
        fprintf(o,"  li t6, %d\n  add t6, sp, t6\n  sd %s, 0(t6)\n", off, reg);
    }
}
static void ld_sp(FILE *o, const char *reg, int off){
    if(off >= -2048 && off <= 2047){
        fprintf(o,"  ld %s, %d(sp)\n", reg, off);
    } else {
        fprintf(o,"  li t6, %d\n  add t6, sp, t6\n  ld %s, 0(t6)\n", off, reg);
    }
}

static void canon(FILE *o, IRType *t, const char *reg){
    /* RISC-V arithmetic is already 64-bit; for i32 we sign-extend. */
    int sz=type_size(t);
    if(sz==1) fprintf(o,"  slli %s, %s, 56\n  srai %s, %s, 56\n", reg, reg, reg, reg);
    else if(sz==2) fprintf(o,"  slli %s, %s, 48\n  srai %s, %s, 48\n", reg, reg, reg, reg);
    else if(sz==4) fprintf(o,"  sext.w %s, %s\n", reg, reg);
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
                else if(in->kinds[s]==ARG_IMM){ snprintf(sb,sizeof sb,"%d",in->args[s]); src=sb; }
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
    if(f->vararg){
        /* va_list (32 bytes) then GP save area (64 bytes = 8 regs). */
        cursor += 32;
        f->va_list_off = cursor;
        cursor += 32;
        f->vararg_save_off = cursor;
        cursor += 64;
    }
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

int riscv_emit(IRModule *m, FILE *o, const TargetDesc *t){
    (void)t;
    fputs(".text\n",o);
    for(uint32_t i=0;i<m->nfuncs;i++){
        IRFunc *f=&m->funcs[i];
        RegAlloc ra = regalloc_run_target(f, &riscv_reginfo);
        regalloc_dump(&ra, f, o);

        int maxv=count_maxv(f);
        int vreg_bytes=(maxv+1)*8;

        /* Frame layout (sp-relative, positive offsets):
           [0 .. vreg_bytes)         vreg slots
           [vreg_bytes]              saved ra
           [vreg_bytes+8 .. +8*csn]  saved callee-saved regs
           [vreg_bytes+8+8*csn .. )  allocas
        */
        int used_cs[5]={0};
        for(int v=0;v<ra.nvregs;v++){
            int r = ra.reg_of[v];
            if(r >= 4 && r <= 8) used_cs[r-4]=1;
        }
        int csn=0; for(int k=0;k<5;k++) if(used_cs[k]) csn++;
        int total = vreg_bytes + 8 + 8*csn;
        total = assign_allocas(f, total);
        int framesize = (total + 15) & ~15;

        static const char *CS[] = {"s1","s2","s3","s4","s5"};

        fprintf(o,".globl %s\n.type %s, @function\n%s:\n",f->name,f->name,f->name);
        if(framesize <= 2047)
            fprintf(o,"  addi sp, sp, -%d\n", framesize);
        else {
            fprintf(o,"  li t0, %d\n  sub sp, sp, t0\n", framesize);
        }
        sd_sp(o, "ra", vreg_bytes);
        int coff = vreg_bytes + 8;
        for(int k=0;k<5;k++){
            if(used_cs[k]){
                sd_sp(o, CS[k], coff);
                coff += 8;
            }
        }
        if(f->vararg){
            int sb = f->vararg_save_off;
            if(sb <= 2047)
                fprintf(o,"  addi t0, sp, %d\n", sb);
            else {
                fprintf(o,"  li t0, %d\n  add t0, sp, t0\n", sb);
            }
            fputs("  sd a0, 0(t0)\n", o);
            fputs("  sd a1, 8(t0)\n", o);
            fputs("  sd a2, 16(t0)\n", o);
            fputs("  sd a3, 24(t0)\n", o);
            fputs("  sd a4, 32(t0)\n", o);
            fputs("  sd a5, 40(t0)\n", o);
            fputs("  sd a6, 48(t0)\n", o);
            fputs("  sd a7, 56(t0)\n", o);
        }

        static const char *AREG[]={"a0","a1","a2","a3","a4","a5","a6","a7"};
        for(uint32_t p=0;p<f->nparams && p<8;p++){
            int pv = PARAM_BASE + (int)p;
            sd_sp(o, AREG[p], slotoff(pv));
        }
        for(uint32_t p=8;p<f->nparams;p++){
            int pv = PARAM_BASE + (int)p;
            /* args 9+ on caller's stack above our frame. */
            fprintf(o,"  ld t0, %d(sp)\n  sd t0, %d(sp)\n",
                    framesize + (int)((p-8)*8), slotoff(pv));
        }
        for(uint32_t p=0;p<f->nparams;p++){
            int pv = PARAM_BASE + (int)p;
            if(pv < ra.nvregs && ra.reg_of[pv] >= 0){
                const char *r = regalloc_name(&ra, ra.reg_of[pv]);
                ld_sp(o, r, slotoff(pv));
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
                        load_to(o, s, "a0");
                    } else fputs("  li a0, 0\n",o);
                    ld_sp(o, "ra", vreg_bytes);
                    coff = vreg_bytes + 8;
                    for(int q=0;q<5;q++){
                        if(used_cs[q]){ ld_sp(o, CS[q], coff); coff+=8; }
                    }
                    if(framesize <= 2047)
                        fprintf(o,"  addi sp, sp, %d\n  ret\n", framesize);
                    else {
                        fprintf(o,"  li t0, %d\n  add sp, sp, t0\n  ret\n", framesize);
                    }
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
                    if(is_mem(a1)) load_to(o, a1, SCRATCH2), a1 = SCRATCH2;
                    else if(is_imm(a1)){ fprintf(o,"  li %s, %s\n", SCRATCH2, a1); a1 = SCRATCH2; }
                    const char *mn="add";
                    if(in->op==OP_SUB)mn="sub";
                    else if(in->op==OP_MUL)mn="mul";
                    else if(in->op==OP_AND)mn="and";
                    else if(in->op==OP_OR) mn="or";
                    else if(in->op==OP_XOR)mn="xor";
                    fprintf(o,"  %s %s, %s, %s\n", mn, dop, dop, a1);
                    canon(o,in->type,dop);
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
                    if(is_mem(a1)) load_to(o, a1, SCRATCH2), a1=SCRATCH2;
                    else if(is_imm(a1)){ fprintf(o,"  li %s, %s\n", SCRATCH2, a1); a1=SCRATCH2; }
                    const char *dop = (in->dst>=0 && ra.reg_of[in->dst]>=0)
                        ? regalloc_name(&ra,ra.reg_of[in->dst]) : SCRATCH1;
                    const char *mn1 = (in->op==OP_SDIV||in->op==OP_SREM) ? "div" : "divu";
                    const char *mn2 = (in->op==OP_SDIV||in->op==OP_SREM) ? "rem" : "remu";
                    if(in->op==OP_SDIV || in->op==OP_UDIV)
                        fprintf(o,"  %s %s, %s, %s\n", mn1, dop, SCRATCH1, a1);
                    else
                        fprintf(o,"  %s %s, %s, %s\n", mn2, dop, SCRATCH1, a1);
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
                        ? regalloc_name(&ra,ra.reg_of[in->dst]) : SCRATCH1;
                    load_to(o, a0, dop);
                    if(is_mem(a1)) load_to(o, a1, SCRATCH2), a1=SCRATCH2;
                    else if(is_imm(a1)){ fprintf(o,"  li %s, %s\n", SCRATCH2, a1); a1=SCRATCH2; }
                    const char *mn = in->op==OP_SHL?"sll":(in->op==OP_LSHR?"srl":"sra");
                    fprintf(o,"  %s %s, %s, %s\n", mn, dop, dop, a1);
                    canon(o,in->type,dop);
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
                    if(in->op==OP_NEG) fprintf(o,"  neg %s, %s\n", dop, dop);
                    else               fprintf(o,"  not %s, %s\n", dop, dop);
                    canon(o,in->type,dop);
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
                    if(is_mem(a1)) load_to(o, a1, SCRATCH2), a1=SCRATCH2;
                    else if(is_imm(a1)){ fprintf(o,"  li %s, %s\n", SCRATCH2, a1); a1=SCRATCH2; }
                    const char *mn="slt";
                    switch(in->pred){
                        case 0: /* eq */ fprintf(o,"  xor t0, t0, %s\n  seqz t0, t0\n", a1); break;
                        case 1: /* ne */ fprintf(o,"  xor t0, t0, %s\n  snez t0, t0\n", a1); break;
                        case 2: mn="slt";  fprintf(o,"  %s t0, t0, %s\n", mn, a1); break;
                        case 3: fprintf(o,"  slt t0, %s, t0\n  xori t0, t0, 1\n", a1); break;   /* a<=b == !(b<a) */
                        case 4: fprintf(o,"  slt t0, %s, t0\n", a1); break;                   /* a>b == b<a */
                        case 5: fprintf(o,"  slt t0, t0, %s\n  xori t0, t0, 1\n", a1); break;  /* a>=b == !(a<b) */
                        case 6: mn="sltu"; fprintf(o,"  %s t0, t0, %s\n", mn, a1); break;
                        case 7: fprintf(o,"  sltu t0, %s, t0\n  xori t0, t0, 1\n", a1); break;
                        case 8: fprintf(o,"  sltu t0, %s, t0\n", a1); break;
                        default:fprintf(o,"  sltu t0, t0, %s\n  xori t0, t0, 1\n", a1); break;
                    }
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, "t0");
                    }
                    break;
                }
                case OP_ZEXT: case OP_SEXT: case OP_TRUNC: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, "t0");
                    if(in->op==OP_SEXT) fputs("  slli t0, t0, 56\n  srai t0, t0, 56\n",o);
                    else                fputs("  andi t0, t0, 255\n",o);
                    canon(o,in->type,"t0");
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, "t0");
                    }
                    break;
                }
                case OP_ALLOCA:
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        if(in->pred <= 2047)
                            fprintf(o,"  addi t0, sp, %u\n", in->pred);
                        else {
                            fprintf(o,"  li t0, %u\n  add t0, sp, t0\n", in->pred);
                        }
                        store_from(o, dd, "t0");
                    }
                    break;
                case OP_LOAD: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, "t0");
                    int lsz=type_size(in->type);
                    if(lsz==1)fputs("  lb t0, 0(t0)\n",o);
                    else if(lsz==2)fputs("  lh t0, 0(t0)\n",o);
                    else if(lsz==4)fputs("  lw t0, 0(t0)\n",o);
                    else fputs("  ld t0, 0(t0)\n",o);
                    canon(o,in->type,"t0");
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, "t0");
                    }
                    break;
                }
                case OP_STORE: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, "t0");
                    load_to(o, a1, "t1");
                    int ssz=type_size(in->type);
                    if(ssz==1)fputs("  sb t0, 0(t1)\n",o);
                    else if(ssz==2)fputs("  sh t0, 0(t1)\n",o);
                    else if(ssz==4)fputs("  sw t0, 0(t1)\n",o);
                    else fputs("  sd t0, 0(t1)\n",o);
                    break;
                }
                case OP_GEP: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, SCRATCH1);
                    if(is_mem(a1)) load_to(o, a1, SCRATCH2), a1=SCRATCH2;
                    else if(is_imm(a1)){ fprintf(o,"  li %s, %s\n", SCRATCH2, a1); a1=SCRATCH2; }
                    unsigned esz=in->pred;
                    if(esz==1) fputs("  add t0, t0, t1\n",o);
                    else if(esz==2) fputs("  slli t1, t1, 1\n  add t0, t0, t1\n",o);
                    else if(esz==4) fputs("  slli t1, t1, 2\n  add t0, t0, t1\n",o);
                    else if(esz==8) fputs("  slli t1, t1, 3\n  add t0, t0, t1\n",o);
                    else {
                        fprintf(o,"  li t6, %u\n  mul t1, t1, t6\n  add t0, t0, t1\n",esz);
                    }
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, "t0");
                    }
                    break;
                }
                case OP_MEMCPY: {
                    char a0b[64], a1b[64];
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    int size = in->args[2];
                    load_to(o, a0, "t0");
                    load_to(o, a1, "t1");
                    int n8 = size / 8, rem = size % 8;
                    for(int i=0;i<n8;i++){
                        fprintf(o,"  ld t2, %d(t1)\n  sd t2, %d(t0)\n", i*8, i*8);
                    }
                    for(int i=0;i<rem;i++){
                        int off = n8*8 + i;
                        fprintf(o,"  lb t2, %d(t1)\n  sb t2, %d(t0)\n", off, off);
                    }
                    break;
                }
                case OP_GEP_FIELD: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, "t0");
                    if(in->pred <= 2047)
                        fprintf(o,"  addi t0, t0, %u\n", in->pred);
                    else {
                        fprintf(o,"  li t1, %u\n  add t0, t0, t1\n", in->pred);
                    }
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, "t0");
                    }
                    break;
                }
                case OP_STR: {
                    uint32_t idx=(uint32_t)in->args[0];
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        fprintf(o,"  la t0, %s\n", m->strings[idx].label);
                        store_from(o, dd, "t0");
                    }
                    break;
                }
                case OP_CALL: {
                    static const char *AREG2[]={"a0","a1","a2","a3","a4","a5","a6","a7"};
                    uint32_t n = in->nargs;
                    uint32_t stackargs = n > 8 ? n - 8 : 0;
                    if(stackargs){
                        /* Make room for stack args, then store. */
                        uint32_t bytes = stackargs * 8;
                        uint32_t pad = (bytes & 15) ? 8 : 0;
                        uint32_t total = bytes + pad;
                        if(total <= 2047)
                            fprintf(o,"  addi sp, sp, -%u\n", total);
                        else {
                            fprintf(o,"  li t0, %u\n  sub sp, sp, t0\n", total);
                        }
                        for(int a=8;a<(int)n;a++){
                            const char *s = op_str(in,a,&ra,a0b,sizeof a0b);
                            load_to(o, s, "t0");
                            fprintf(o,"  sd t0, %d(sp)\n", (a-8)*8);
                        }
                    }
                    for(uint32_t a=0;a<8&&a<n;a++){
                        const char *s = op_str(in,a,&ra,a0b,sizeof a0b);
                        load_to(o, s, AREG2[a]);
                    }
                    if(in->callee) fprintf(o,"  call %s\n", in->callee);
                    if(stackargs){
                        uint32_t bytes = stackargs * 8;
                        uint32_t pad = (bytes & 15) ? 8 : 0;
                        if(bytes+pad <= 2047)
                            fprintf(o,"  addi sp, sp, %u\n", bytes+pad);
                        else {
                            fprintf(o,"  li t0, %u\n  add sp, sp, t0\n", bytes+pad);
                        }
                    }
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, "a0");
                    }
                    break;
                }
                case OP_CBR: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, SCRATCH1);
                    int need_t = phi_copies_needed(f, b->name, in->label);
                    int need_f = phi_copies_needed(f, b->name, in->label2);
                    if(!need_t && !need_f){
                        fprintf(o,"  bnez %s, .L%s_%s\n  j .L%s_%s\n",
                                SCRATCH1, f->name, in->label, f->name, in->label2);
                    } else {
                        fprintf(o,"  bnez %s, .L%s_%s__ct_%u\n", SCRATCH1, f->name, b->name, k);
                        fprintf(o,".L%s_%s__fall_%u:\n", f->name, b->name, k);
                        if(need_f) emit_phi_copies_for_edge(o,f,b->name,in->label2,&ra);
                        fprintf(o,"  j .L%s_%s\n", f->name, in->label2);
                        fprintf(o,".L%s_%s__ct_%u:\n", f->name, b->name, k);
                        if(need_t) emit_phi_copies_for_edge(o,f,b->name,in->label,&ra);
                        fprintf(o,"  j .L%s_%s\n", f->name, in->label);
                    }
                    break;
                }
                case OP_BR:
                    if(in->label)fprintf(o,"  j .L%s_%s\n",f->name,in->label);
                    break;
                case OP_VA_START: {
                    /* RV64 va_list: { vr_top(0), gr_top, stack, gr_offs, vr_offs }
                       gr_top  = sp + sb + 64 (end of save area)
                       stack   = sp + framesize (first incoming stack arg)
                       gr_offs = 8*nparams - 64
                       vr_top, vr_offs = 0 */
                    int vl_base = f->va_list_off;
                    int sb      = f->vararg_save_off;
                    int gp_off  = 8 * (int)f->nparams - 64;
                    /* t0 = &va_list */
                    if(vl_base <= 2047)
                        fprintf(o,"  addi t0, sp, %d\n", vl_base);
                    else {
                        fprintf(o,"  li t0, %d\n  add t0, sp, t0\n", vl_base);
                    }
                    /* [t0+0] = vr_top = 0 */
                    fputs("  sd zero, 0(t0)\n", o);
                    /* [t0+8] = gr_top = sp + (sb + 64) */
                    int grtop = sb + 64;
                    if(grtop <= 2047)
                        fprintf(o,"  addi t1, sp, %d\n", grtop);
                    else {
                        fprintf(o,"  li t1, %d\n  add t1, sp, t1\n", grtop);
                    }
                    fputs("  sd t1, 8(t0)\n", o);
                    /* [t0+16] = stack = sp + framesize */
                    if(framesize <= 2047)
                        fprintf(o,"  addi t1, sp, %d\n", framesize);
                    else {
                        fprintf(o,"  li t1, %d\n  add t1, sp, t1\n", framesize);
                    }
                    fputs("  sd t1, 16(t0)\n", o);
                    /* [t0+24] = gr_offs */
                    fprintf(o,"  li t1, %d\n", gp_off);
                    fputs("  sw t1, 24(t0)\n", o);
                    /* [t0+28] = vr_offs = 0 */
                    fputs("  sw zero, 28(t0)\n", o);
                    if(in->dst>=0){
                        char db[64]; const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        fprintf(o,"  mv %s, t0\n", dd);
                    }
                    break;
                }
                case OP_VA_ARG: {
                    char ab[64];
                    const char *aps = vop_str(in->args[0], &ra, ab, sizeof ab);
                    int sz = type_size(in->type);
                    unsigned uid = (unsigned)(in - f->blocks[0].instrs);
                    uid ^= (unsigned)in->dst * 2654435761u;
                    uid &= 0x7fffffff;
                    fprintf(o,"  mv t0, %s\n", aps);
                    fputs("  lw t1, 24(t0)\n", o);
                    fprintf(o,"  bgez t1, .Lva_ovf_%u\n", uid);
                    /* register path: t6 = gr_top + gr_offs */
                    fputs("  ld t6, 8(t0)\n", o);
                    fputs("  add t6, t6, t1\n", o);
                    fputs("  addi t1, t1, 8\n", o);
                    fputs("  sw t1, 24(t0)\n", o);
                    fputs("  ld t1, 0(t6)\n", o);
                    fprintf(o,"  j .Lva_done_%u\n", uid);
                    fprintf(o,".Lva_ovf_%u:\n", uid);
                    fputs("  ld t6, 16(t0)\n", o);
                    fputs("  ld t1, 0(t6)\n", o);
                    fputs("  addi t6, t6, 8\n", o);
                    fputs("  sd t6, 16(t0)\n", o);
                    fprintf(o,".Lva_done_%u:\n", uid);
                    if(sz==1) fputs("  slli t1, t1, 56\n  srai t1, t1, 56\n", o);
                    else if(sz==2) fputs("  slli t1, t1, 48\n  srai t1, t1, 48\n", o);
                    else if(sz==4) fputs("  sext.w t1, t1\n", o);
                    if(in->dst>=0){
                        char db[64]; const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        fprintf(o,"  mv %s, t1\n", dd);
                    }
                    break;
                }
                case OP_VA_END:
                    break;

                case OP_UNREACHABLE: fputs("  ebreak\n",o); break;

                /* FP ops stay slot-based. */
                case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: {
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm_get(in->args[0]);
                        memcpy(&bits,&d,8);
                        fprintf(o,"  li t0, %llu\n  fmv.d.x ft0, t0\n",(unsigned long long)bits);
                    } else fprintf(o,"  fld ft0, %d(sp)\n",slotoff(in->args[0]));
                    if(in->kinds[1]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm_get(in->args[1]);
                        memcpy(&bits,&d,8);
                        fprintf(o,"  li t0, %llu\n  fmv.d.x ft1, t0\n",(unsigned long long)bits);
                    } else fprintf(o,"  fld ft1, %d(sp)\n",slotoff(in->args[1]));
                    const char *suf=(in->type&&in->type->kind==TY_F32)?"s":"d";
                    const char *mn="fadd";
                    if(in->op==OP_FSUB)mn="fsub";
                    else if(in->op==OP_FMUL)mn="fmul";
                    else if(in->op==OP_FDIV)mn="fdiv";
                    fprintf(o,"  %s.%s ft0, ft0, ft1\n", mn, suf);
                    if(in->dst>=0) fprintf(o,"  fsd ft0, %d(sp)\n", slotoff(in->dst));
                    break;
                }
                case OP_FNEG: {
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm_get(in->args[0]);
                        memcpy(&bits,&d,8);
                        fprintf(o,"  li t0, %llu\n  fmv.d.x ft0, t0\n",(unsigned long long)bits);
                    } else fprintf(o,"  fld ft0, %d(sp)\n",slotoff(in->args[0]));
                    fputs("  fneg.d ft0, ft0\n",o);
                    if(in->dst>=0) fprintf(o,"  fsd ft0, %d(sp)\n", slotoff(in->dst));
                    break;
                }
                case OP_FCMP: {
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm_get(in->args[0]);
                        memcpy(&bits,&d,8);
                        fprintf(o,"  li t0, %llu\n  fmv.d.x ft0, t0\n",(unsigned long long)bits);
                    } else fprintf(o,"  fld ft0, %d(sp)\n",slotoff(in->args[0]));
                    if(in->kinds[1]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm_get(in->args[1]);
                        memcpy(&bits,&d,8);
                        fprintf(o,"  li t0, %llu\n  fmv.d.x ft1, t0\n",(unsigned long long)bits);
                    } else fprintf(o,"  fld ft1, %d(sp)\n",slotoff(in->args[1]));
                    const char *suf=(in->type&&in->type->kind==TY_F32)?"s":"d";
                    fprintf(o,"  feq.%s t0, ft0, ft1\n", suf);  /* crude: only handles eq */
                    if(in->dst>=0) fprintf(o,"  sd t0, %d(sp)\n", slotoff(in->dst));
                    break;
                }
                case OP_SITOFP: case OP_UITOFP: case OP_FPTOSI: case OP_FPTOUI:
                case OP_FPEXT: case OP_FPTRUNC: {
                    int src_is_fp = (in->pred==TY_F32 || in->pred==TY_F64);
                    if(src_is_fp){
                        if(in->kinds[0]==ARG_FP){
                            uint64_t bits; double d=ir_fpimm_get(in->args[0]);
                            memcpy(&bits,&d,8);
                            fprintf(o,"  li t0, %llu\n  fmv.d.x ft0, t0\n",(unsigned long long)bits);
                        } else fprintf(o,"  fld ft0, %d(sp)\n",slotoff(in->args[0]));
                    } else if(in->kinds[0]==ARG_IMM){
                        fprintf(o,"  li t0, %d\n", in->args[0]);
                    } else {
                        fprintf(o,"  ld t0, %d(sp)\n",slotoff(in->args[0]));
                    }
                    int dt = in->type ? in->type->kind : TY_F64;
                    if(in->op==OP_SITOFP){
                        if(dt==TY_F32)fputs("  fcvt.s.w ft0, t0\n",o); else fputs("  fcvt.d.w ft0, t0\n",o);
                    } else if(in->op==OP_UITOFP){
                        if(dt==TY_F32)fputs("  fcvt.s.wu ft0, t0\n",o); else fputs("  fcvt.d.wu ft0, t0\n",o);
                    } else if(in->op==OP_FPTOSI){
                        if(in->pred==TY_F32)fputs("  fcvt.w.s t0, ft0\n",o); else fputs("  fcvt.w.d t0, ft0\n",o);
                    } else if(in->op==OP_FPTOUI){
                        if(in->pred==TY_F32)fputs("  fcvt.wu.s t0, ft0\n",o); else fputs("  fcvt.wu.d t0, ft0\n",o);
                    } else if(in->op==OP_FPEXT){
                        fputs("  fcvt.d.s ft0, ft0\n",o);
                    } else if(in->op==OP_FPTRUNC){
                        fputs("  fcvt.s.d ft0, ft0\n",o);
                    }
                    if(in->dst>=0){
                        int produces_fp = (in->op==OP_SITOFP||in->op==OP_UITOFP||in->op==OP_FPEXT||in->op==OP_FPTRUNC);
                        if(produces_fp) fprintf(o,"  fsd ft0, %d(sp)\n", slotoff(in->dst));
                        else            fprintf(o,"  sd t0, %d(sp)\n", slotoff(in->dst));
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
    if(has_main && !cc_libc_mode){
        fputs(".globl _start\n.type _start, @function\n_start:\n", o);
        fputs("  call main\n", o);
        fputs("  li a7, 93\n  ecall\n", o);
        fputs(".size _start, .-_start\n", o);
    }
    fputs(".section .note.GNU-stack,\"\",@progbits\n",o);
    return 0;
}
