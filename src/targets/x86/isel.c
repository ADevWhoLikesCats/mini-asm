#include "targets/x86.h"
#include "backend.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define PARAM_BASE 1000


#define SCRATCH1 "%eax"
#define SCRATCH2 "%edx"
#define SCRATCH3 "%ecx"

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
/* 4-byte slots on i386. */
static int slotoff(int v){ return -4*(v+1); }

static const char *op_str(IRInstr *in, int i, const RegAlloc *ra, char *buf, size_t cap){
    if(in->kinds[i]==ARG_IMM){ snprintf(buf,cap,"$%d",in->args[i]); return buf; }
    if(in->kinds[i]==ARG_FP){ snprintf(buf,cap,"$0"); return buf; }
    int v = in->args[i];
    if(ra && v>=0 && v<ra->nvregs && ra->reg_of[v]>=0)
        snprintf(buf,cap,"%s",regalloc_name(ra,ra->reg_of[v]));
    else
        snprintf(buf,cap,"%d(%%ebp)",slotoff(v));
    return buf;
}
static const char *vop_str(int v, const RegAlloc *ra, char *buf, size_t cap){
    if(ra && v>=0 && v<ra->nvregs && ra->reg_of[v]>=0)
        snprintf(buf,cap,"%s",regalloc_name(ra,ra->reg_of[v]));
    else
        snprintf(buf,cap,"%d(%%ebp)",slotoff(v));
    return buf;
}
static void load_to(FILE *o, const char *ops, const char *reg){
    if(strcmp(ops,reg)==0) return;
    fprintf(o,"  movl %s, %s\n", ops, reg);
}
static void store_from(FILE *o, const char *dstops, const char *reg){
    if(strcmp(dstops,reg)==0) return;
    fprintf(o,"  movl %s, %s\n", reg, dstops);
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
                else if(in->kinds[s]==ARG_IMM){ snprintf(sb,sizeof sb,"$%d",in->args[s]); src=sb; }
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

int x86_emit(IRModule *m, FILE *o, const TargetDesc *t){
    (void)t;
    fputs(".text\n",o);
    for(uint32_t i=0;i<m->nfuncs;i++){
        IRFunc *f=&m->funcs[i];
        RegAlloc ra = regalloc_run_target(f, &x86_reginfo);
        regalloc_dump(&ra, f, o);

        int maxv=count_maxv(f);
        int vreg_bytes=(maxv+1)*4;

        int used_cs[3]={0}; /* ebx, esi, edi (pool indices 0,1,2) */
        for(int v=0;v<ra.nvregs;v++){
            int r = ra.reg_of[v];
            if(r >= 0 && r <= 2) used_cs[r]=1;
        }
        int csn=0; for(int k=0;k<3;k++) if(used_cs[k]) csn++;
        int total = vreg_bytes + 4*csn;
        total = assign_allocas(f, total);
        int framesize = (total + 15) & ~15;

        static const char *CS[] = {"%ebx","%esi","%edi"};

        fprintf(o,".globl %s\n.type %s, @function\n%s:\n",f->name,f->name,f->name);
        fputs("  pushl %ebp\n  movl %esp, %ebp\n",o);
        if(framesize) fprintf(o,"  subl $%d, %%esp\n", framesize);
        int coff = vreg_bytes;
        for(int k=0;k<3;k++){
            if(used_cs[k]){ fprintf(o,"  movl %s, %d(%%ebp)\n", CS[k], -coff-4); coff += 4; }
        }

        /* cdecl: params pushed right-to-left. Param 0 at [ebp+8], param 1 at [ebp+12], etc. */
        for(uint32_t p=0;p<f->nparams;p++){
            int pv = PARAM_BASE + (int)p;
            fprintf(o,"  movl %d(%%ebp), %%eax\n  movl %%eax, %d(%%ebp)\n",
                    8 + 4*(int)p, slotoff(pv));
        }
        for(uint32_t p=0;p<f->nparams;p++){
            int pv = PARAM_BASE + (int)p;
            if(pv < ra.nvregs && ra.reg_of[pv] >= 0){
                const char *r = regalloc_name(&ra, ra.reg_of[pv]);
                fprintf(o,"  movl %d(%%ebp), %s\n", slotoff(pv), r);
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
                        load_to(o, s, "%eax");
                    } else fputs("  xorl %eax, %eax\n",o);
                    for(int q=2;q>=0;q--){
                        if(used_cs[q]) fprintf(o,"  movl %d(%%ebp), %s\n", -vreg_bytes-4*(q+1), CS[q]);
                    }
                    fputs("  leave\n  ret\n",o);
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
                    const char *mn="addl";
                    if(in->op==OP_SUB)mn="subl";
                    else if(in->op==OP_MUL)mn="imull";
                    else if(in->op==OP_AND)mn="andl";
                    else if(in->op==OP_OR) mn="orl";
                    else if(in->op==OP_XOR)mn="xorl";
                    fprintf(o,"  %s %s, %s\n", mn, a1, dop);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, dop);
                    }
                    break;
                }
                case OP_SDIV: case OP_UDIV: case OP_SREM: case OP_UREM: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, "%eax");
                    /* load divisor into a reg */
                    const char *div;
                    if(a1[0]=='$'){ fprintf(o,"  movl %s, %%ecx\n", a1); div="%ecx"; }
                    else if(strcmp(a1,"%eax")==0){ fprintf(o,"  movl %%eax, %%ecx\n"); div="%ecx"; }
                    else div=a1;
                    if(in->op==OP_SDIV||in->op==OP_SREM)fputs("  cltd\n  idivl ",o);
                    else fputs("  xorl %edx, %edx\n  divl ",o);
                    fprintf(o,"%s\n",div);
                    const char *res = (in->op==OP_SREM||in->op==OP_UREM) ? "%edx" : "%eax";
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, res);
                    }
                    break;
                }
                case OP_SHL: case OP_LSHR: case OP_ASHR: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, SCRATCH1);
                    /* count in %cl */
                    if(a1[0]=='$'){ fprintf(o,"  movl %s, %%ecx\n", a1); }
                    else if(strcmp(a1,"%ecx")!=0){ fprintf(o,"  movl %s, %%ecx\n", a1); }
                    const char *mn = in->op==OP_SHL?"shll":(in->op==OP_LSHR?"shrl":"sarl");
                    fprintf(o,"  %s %%cl, %s\n", mn, SCRATCH1);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                }
                case OP_NEG: case OP_NOT: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, SCRATCH1);
                    fputs(in->op==OP_NEG?"  negl %eax\n":"  notl %eax\n",o);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                }
                case OP_ICMP: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, SCRATCH1);
                    fprintf(o,"  cmpl %s, %%eax\n", a1);
                    static const char *cm[]={"sete","setne","setl","setle","setg","setge","setb","setbe","seta","setae"};
                    fprintf(o,"  %s %%al\n  movzbl %%al, %%eax\n",cm[in->pred%10]);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                }
                case OP_ZEXT: case OP_SEXT: case OP_TRUNC: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, SCRATCH1);
                    fputs(in->op==OP_SEXT?"  movsbl %al, %eax\n":"  movzbl %al, %eax\n",o);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                }
                case OP_ALLOCA:
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        fprintf(o,"  leal -%u(%%ebp), %%eax\n", in->pred);
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                case OP_LOAD: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, "%eax");
                    int lsz=type_size(in->type);
                    if(lsz==1)fputs("  movsbl (%eax), %eax\n",o);
                    else if(lsz==2)fputs("  movswl (%eax), %eax\n",o);
                    else fputs("  movl (%eax), %eax\n",o);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                }
                case OP_STORE: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, "%eax");
                    load_to(o, a1, "%ecx");
                    int ssz=type_size(in->type);
                    if(ssz==1)fputs("  movb %al, (%ecx)\n",o);
                    else if(ssz==2)fputs("  movw %ax, (%ecx)\n",o);
                    else fputs("  movl %eax, (%ecx)\n",o);
                    break;
                }
                case OP_MEMCPY: {
                    char a0b[64], a1b[64];
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    int size = in->args[2];
                    load_to(o, a0, "%edi");
                    load_to(o, a1, "%esi");
                    fprintf(o,"  movl $%d, %%ecx\n  rep movsb\n", size);
                    break;
                }
                case OP_GEP: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    const char *a1 = op_str(in,1,&ra,a1b,sizeof a1b);
                    load_to(o, a0, SCRATCH1);
                    load_to(o, a1, "%ecx");
                    unsigned esz=in->pred;
                    if(esz==1) fputs("  addl %ecx, %eax\n",o);
                    else if(esz==2) fputs("  leal (%eax,%ecx,2), %eax\n",o);
                    else if(esz==4) fputs("  leal (%eax,%ecx,4), %eax\n",o);
                    else if(esz==8) fputs("  leal (%eax,%ecx,8), %eax\n",o);
                    else { fprintf(o,"  imull $%u, %%ecx, %%ecx\n  addl %%ecx, %%eax\n",esz); }
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                }
                case OP_GEP_FIELD: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, SCRATCH1);
                    fprintf(o,"  addl $%u, %%eax\n", in->pred);
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
                        fprintf(o,"  movl $%s, %%eax\n", m->strings[idx].label);
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                }
                case OP_CALL: {
                    uint32_t n = in->nargs;
                    /* cdecl: push right-to-left. */
                    for(int a=(int)n-1; a>=0; a--){
                        const char *s = op_str(in,a,&ra,a0b,sizeof a0b);
                        if(s[0]=='$') fprintf(o,"  pushl %s\n", s);
                        else { load_to(o, s, "%eax"); fputs("  pushl %eax\n",o); }
                    }
                    if(in->callee) fprintf(o,"  call %s\n", in->callee);
                    if(n) fprintf(o,"  addl $%u, %%esp\n", n*4);
                    if(in->dst>=0){
                        const char *dd = vop_str(in->dst,&ra,db,sizeof db);
                        store_from(o, dd, SCRATCH1);
                    }
                    break;
                }
                case OP_CBR: {
                    const char *a0 = op_str(in,0,&ra,a0b,sizeof a0b);
                    load_to(o, a0, SCRATCH1);
                    int need_t = phi_copies_needed(f, b->name, in->label);
                    int need_f = phi_copies_needed(f, b->name, in->label2);
                    if(!need_t && !need_f){
                        fprintf(o,"  testl %%eax, %%eax\n  jne .L%s_%s\n  jmp .L%s_%s\n",
                                f->name, in->label, f->name, in->label2);
                    } else {
                        fprintf(o,"  testl %%eax, %%eax\n");
                        fprintf(o,"  jne .L%s_%s__ct_%u\n", f->name, b->name, k);
                        fprintf(o,".L%s_%s__fall_%u:\n", f->name, b->name, k);
                        if(need_f) emit_phi_copies_for_edge(o,f,b->name,in->label2,&ra);
                        fprintf(o,"  jmp .L%s_%s\n", f->name, in->label2);
                        fprintf(o,".L%s_%s__ct_%u:\n", f->name, b->name, k);
                        if(need_t) emit_phi_copies_for_edge(o,f,b->name,in->label,&ra);
                        fprintf(o,"  jmp .L%s_%s\n", f->name, in->label);
                    }
                    break;
                }
                case OP_BR:
                    if(in->label)fprintf(o,"  jmp .L%s_%s\n",f->name,in->label);
                    break;
                case OP_VA_START: {
                    /* cdecl: all args on the stack. va_list is just a char*
                       pointing at the first vararg. */
                    int offset = 8 + 4 * (int)f->nparams;
                    if(in->dst >= 0){
                        char db[64]; const char *dd = vop_str(in->dst, &ra, db, sizeof db);
                        fprintf(o, "  leal %d(%%ebp), %%eax\n", offset);
                        fprintf(o, "  movl %%eax, %s\n", dd);
                    }
                    break;
                }
                case OP_VA_ARG: {
                    char ab[64];
                    const char *aps = vop_str(in->args[0], &ra, ab, sizeof ab);
                    int sz = type_size(in->type);
                    fprintf(o, "  movl %s, %%eax\n", aps);
                    fprintf(o, "  movl (%%eax), %%edx\n");
                    fprintf(o, "  addl $4, %%eax\n");
                    fprintf(o, "  movl %%eax, %s\n", aps);
                    if(sz == 1) fputs("  movsbl %dl, %edx\n", o);
                    else if(sz == 2) fputs("  movswl %dx, %edx\n", o);
                    if(in->dst >= 0){
                        char db[64]; const char *dd = vop_str(in->dst, &ra, db, sizeof db);
                        fprintf(o, "  movl %%edx, %s\n", dd);
                    }
                    break;
                }
                case OP_VA_END:
                    break;

                case OP_UNREACHABLE: fputs("  ud2\n",o); break;

                case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV: {
                    /* i386 SSE2. FP vregs stay in 8-byte slots. */
                    const char *suf = (in->type && in->type->kind==TY_F32) ? "ss" : "sd";
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm_get(in->args[0]);
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movl $%u, %%eax\n  movl $%u, %%edx\n  movd %%eax, %%xmm0\n  movd %%edx, %%xmm1\n  punpckldq %%xmm1, %%xmm0\n",
                            (unsigned)(bits&0xffffffff),(unsigned)(bits>>32));
                    } else {
                        fprintf(o,"  movsd %d(%%ebp), %%xmm0\n", slotoff(in->args[0]));
                    }
                    if(in->kinds[1]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm_get(in->args[1]);
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movl $%u, %%eax\n  movl $%u, %%edx\n  movd %%eax, %%xmm1\n  movd %%edx, %%xmm2\n  punpckldq %%xmm2, %%xmm1\n",
                            (unsigned)(bits&0xffffffff),(unsigned)(bits>>32));
                    } else {
                        fprintf(o,"  movsd %d(%%ebp), %%xmm1\n", slotoff(in->args[1]));
                    }
                    const char *mn="add";
                    if(in->op==OP_FSUB)mn="sub";
                    else if(in->op==OP_FMUL)mn="mul";
                    else if(in->op==OP_FDIV)mn="div";
                    fprintf(o,"  %s%s %%xmm1, %%xmm0\n", mn, suf);
                    if(in->dst>=0) fprintf(o,"  movsd %%xmm0, %d(%%ebp)\n", slotoff(in->dst));
                    break;
                }
                case OP_FNEG: {
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm_get(in->args[0]);
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movl $%u, %%eax\n  movl $%u, %%edx\n  movd %%eax, %%xmm0\n  movd %%edx, %%xmm1\n  punpckldq %%xmm1, %%xmm0\n",
                            (unsigned)(bits&0xffffffff),(unsigned)(bits>>32));
                    } else fprintf(o,"  movsd %d(%%ebp), %%xmm0\n",slotoff(in->args[0]));
                    fputs("  movl $0x80000000, %eax\n  movd %eax, %xmm1\n  movl $0, %eax\n  movd %eax, %xmm2\n  punpckldq %xmm2, %xmm1\n  xorpd %xmm1, %xmm0\n", o);
                    if(in->dst>=0) fprintf(o,"  movsd %%xmm0, %d(%%ebp)\n", slotoff(in->dst));
                    break;
                }
                case OP_FCMP: {
                    if(in->kinds[0]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm_get(in->args[0]);
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movl $%u, %%eax\n  movl $%u, %%edx\n  movd %%eax, %%xmm0\n  movd %%edx, %%xmm1\n  punpckldq %%xmm1, %%xmm0\n",
                            (unsigned)(bits&0xffffffff),(unsigned)(bits>>32));
                    } else fprintf(o,"  movsd %d(%%ebp), %%xmm0\n",slotoff(in->args[0]));
                    if(in->kinds[1]==ARG_FP){
                        uint64_t bits; double d=ir_fpimm_get(in->args[1]);
                        memcpy(&bits,&d,8);
                        fprintf(o,"  movl $%u, %%eax\n  movl $%u, %%edx\n  movd %%eax, %%xmm1\n  movd %%edx, %%xmm2\n  punpckldq %%xmm2, %%xmm1\n",
                            (unsigned)(bits&0xffffffff),(unsigned)(bits>>32));
                    } else fprintf(o,"  movsd %d(%%ebp), %%xmm1\n",slotoff(in->args[1]));
                    const char *suf=(in->type&&in->type->kind==TY_F32)?"ss":"sd";
                    fprintf(o,"  ucomi%s %%xmm1, %%xmm0\n",suf);
                    static const char *cm[]={"sete","setne","setb","setbe","seta","setae","setnp","setp"};
                    fprintf(o,"  %s %%al\n  movzbl %%al, %%eax\n",cm[in->pred%8]);
                    if(in->dst>=0) fprintf(o,"  movl %%eax, %d(%%ebp)\n", slotoff(in->dst));
                    break;
                }
                case OP_SITOFP: case OP_UITOFP: case OP_FPTOSI: case OP_FPTOUI:
                case OP_FPEXT: case OP_FPTRUNC: {
                    int src_is_fp = (in->pred==TY_F32 || in->pred==TY_F64);
                    if(src_is_fp){
                        if(in->kinds[0]==ARG_FP){
                            uint64_t bits; double d=ir_fpimm_get(in->args[0]);
                            memcpy(&bits,&d,8);
                            fprintf(o,"  movl $%u, %%eax\n  movl $%u, %%edx\n  movd %%eax, %%xmm0\n  movd %%edx, %%xmm1\n  punpckldq %%xmm1, %%xmm0\n",
                                (unsigned)(bits&0xffffffff),(unsigned)(bits>>32));
                        } else fprintf(o,"  movsd %d(%%ebp), %%xmm0\n",slotoff(in->args[0]));
                    } else if(in->kinds[0]==ARG_IMM){
                        fprintf(o,"  movl $%d, %%eax\n", in->args[0]);
                    } else {
                        fprintf(o,"  movl %d(%%ebp), %%eax\n",slotoff(in->args[0]));
                    }
                    int dt = in->type ? in->type->kind : TY_F64;
                    if(in->op==OP_SITOFP){
                        if(dt==TY_F32)fputs("  cvtsi2ss %eax, %xmm0\n",o); else fputs("  cvtsi2sd %eax, %xmm0\n",o);
                    } else if(in->op==OP_UITOFP){
                        if(dt==TY_F32)fputs("  cvtsi2ss %eax, %xmm0\n",o); else fputs("  cvtsi2sd %eax, %xmm0\n",o);
                    } else if(in->op==OP_FPTOSI||in->op==OP_FPTOUI){
                        if(in->pred==TY_F32)fputs("  cvttss2si %xmm0, %eax\n",o); else fputs("  cvttsd2si %xmm0, %eax\n",o);
                    } else if(in->op==OP_FPEXT){
                        fputs("  cvtss2sd %xmm0, %xmm0\n",o);
                    } else if(in->op==OP_FPTRUNC){
                        fputs("  cvtsd2ss %xmm0, %xmm0\n",o);
                    }
                    if(in->dst>=0){
                        int produces_fp = (in->op==OP_SITOFP||in->op==OP_UITOFP||in->op==OP_FPEXT||in->op==OP_FPTRUNC);
                        if(produces_fp) fprintf(o,"  movsd %%xmm0, %d(%%ebp)\n", slotoff(in->dst));
                        else            fprintf(o,"  movl %%eax, %d(%%ebp)\n", slotoff(in->dst));
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
        fputs("  movl %eax, %ebx\n  movl $1, %eax\n  int $0x80\n", o);
        fputs(".size _start, .-_start\n", o);
    }
    fputs(".section .note.GNU-stack,\"\",@progbits\n",o);
    return 0;
}
