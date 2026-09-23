#include "backend.h"
#include "ir.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* --- Use table: which vregs are referenced as operands? --- */
#define MAXV 65536
static char g_used[MAXV];

static void mark_used_vreg(int v){
    if(v >= 0 && v < MAXV) g_used[v] = 1;
}

static int has_side_effect(IROpcode op){
    switch(op){
        case OP_STORE:
        case OP_CALL:
        case OP_BR:
        case OP_CBR:
        case OP_SWITCH:
        case OP_RET:
        case OP_UNREACHABLE:
        case OP_PHI:
            return 1;
        default:
            return 0;
    }
}

/* Compute the set of vregs used as operands anywhere in the function. */
static void compute_uses(IRFunc *f){
    memset(g_used, 0, sizeof g_used);
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b=&f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in=&b->instrs[k];
            for(uint32_t a=0;a<in->nargs && a<16;a++){
                if(in->kinds[a] == ARG_VREG) mark_used_vreg(in->args[a]);
            }
        }
    }
}

/* --- Constant folding --- */
typedef struct { int vreg; int64_t value; } ConstEntry;
#define MAXCONST 4096
static ConstEntry g_consts[MAXCONST];
static int g_nconsts;

static int lookup_const(int vreg, int64_t *out){
    for(int i=0;i<g_nconsts;i++){
        if(g_consts[i].vreg == vreg){ *out = g_consts[i].value; return 1; }
    }
    return 0;
}
static void define_const(int vreg, int64_t val){
    if(g_nconsts < MAXCONST){
        g_consts[g_nconsts].vreg = vreg;
        g_consts[g_nconsts].value = val;
        g_nconsts++;
    }
}

/* Try to fold an arithmetic instruction. Returns 1 if folded. */
static int try_fold(IRInstr *in){
    /* Both operands must be immediates. */
    if(in->nargs < 2) return 0;
    if(in->kinds[0] != ARG_IMM || in->kinds[1] != ARG_IMM) return 0;
    int64_t a = in->args[0], b = in->args[1];
    int64_t r = 0;
    int ok = 1;
    switch(in->op){
        case OP_ADD:  r = a + b; break;
        case OP_SUB:  r = a - b; break;
        case OP_MUL:  r = a * b; break;
        case OP_AND:  r = a & b; break;
        case OP_OR:   r = a | b; break;
        case OP_XOR:  r = a ^ b; break;
        case OP_SHL:  r = (b >= 0 && b < 64) ? (a << b) : 0; break;
        case OP_LSHR: r = (b >= 0 && b < 64) ? (int64_t)((uint64_t)a >> b) : 0; break;
        case OP_ASHR: r = (b >= 0 && b < 64) ? (a >> b) : 0; break;
        case OP_SDIV: if(b == 0) { ok = 0; break; } r = a / b; break;
        case OP_UDIV: if(b == 0) { ok = 0; break; } r = (int64_t)((uint64_t)a / (uint64_t)b); break;
        case OP_SREM: if(b == 0) { ok = 0; break; } r = a % b; break;
        case OP_UREM: if(b == 0) { ok = 0; break; } r = (int64_t)((uint64_t)a % (uint64_t)b); break;
        case OP_NEG:  r = -a; break;
        case OP_NOT:  r = ~a; break;
        case OP_ICMP: {
            int pred = (int)in->pred;
            switch(pred){
                case 0: r = (a == b); break;
                case 1: r = (a != b); break;
                case 2: r = (a <  b); break;
                case 3: r = (a <= b); break;
                case 4: r = (a >  b); break;
                case 5: r = (a >= b); break;
                case 6: r = ((uint64_t)a <  (uint64_t)b); break;
                case 7: r = ((uint64_t)a <= (uint64_t)b); break;
                case 8: r = ((uint64_t)a >  (uint64_t)b); break;
                case 9: r = ((uint64_t)a >= (uint64_t)b); break;
                default: ok = 0; break;
            }
            break;
        }
        default: ok = 0; break;
    }
    if(!ok) return 0;
    if(in->dst >= 0) define_const(in->dst, r);
    return 1;
}

/* Rewrite all operands that are known constants. */
static void propagate_consts(IRFunc *f){
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b=&f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in=&b->instrs[k];
            for(uint32_t a=0;a<in->nargs && a<16;a++){
                if(in->kinds[a] != ARG_VREG) continue;
                int64_t v;
                if(lookup_const(in->args[a], &v)){
                    in->kinds[a] = ARG_IMM;
                    in->args[a] = (int)v;
                }
            }
        }
    }
}

/* --- DCE --- */
/* A simple, correct-enough pass: iteratively remove instructions whose dst
   is not used, has no side effects, and is not a phi. Returns 1 if anything
   was removed. */
static int dce_pass(IRFunc *f){
    compute_uses(f);
    int removed = 0;
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b=&f->blocks[j];
        uint32_t w = 0;
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in=&b->instrs[k];
            int drop = 0;
            if(in->dst >= 0 && !has_side_effect(in->op)){
                if(in->dst >= MAXV || !g_used[in->dst]){
                    drop = 1;
                }
            }
            if(drop){
                removed++;
                continue;
            }
            b->instrs[w++] = b->instrs[k];
        }
        b->ninstrs = w;
    }
    return removed;
}

/* Remove folded instructions: any instruction that had a folded dst whose
   dst is no longer used as a VREG anywhere. */
static int dce_folded(IRFunc *f){
    int removed = 0;
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b=&f->blocks[j];
        uint32_t w = 0;
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in=&b->instrs[k];
            int64_t v;
            int drop = 0;
            if(in->dst >= 0 && lookup_const(in->dst, &v) && !has_side_effect(in->op)){
                /* If no remaining use of this dst as ARG_VREG, drop. */
                int still_used = 0;
                for(uint32_t jj=0;jj<f->nblocks && !still_used;jj++){
                    IRBlock *bb=&f->blocks[jj];
                    for(uint32_t kk=0;kk<bb->ninstrs && !still_used;kk++){
                        IRInstr *i2=&bb->instrs[kk];
                        for(uint32_t a=0;a<i2->nargs && a<16;a++){
                            if(i2->kinds[a]==ARG_VREG && i2->args[a]==in->dst){
                                still_used = 1; break;
                            }
                        }
                    }
                }
                if(!still_used) drop = 1;
            }
            if(drop){ removed++; continue; }
            b->instrs[w++] = b->instrs[k];
        }
        b->ninstrs = w;
    }
    return removed;
}

static int opt_func(IRFunc *f){
    int changes = 0;
    for(int iter=0; iter<4; iter++){
        int did = 0;
        g_nconsts = 0;
        /* Fold */
        for(uint32_t j=0;j<f->nblocks;j++){
            IRBlock *b=&f->blocks[j];
            for(uint32_t k=0;k<b->ninstrs;k++){
                IRInstr *in=&b->instrs[k];
                if(try_fold(in)) did = 1;
            }
        }
        /* Propagate */
        if(g_nconsts) propagate_consts(f);
        /* Drop folded instructions */
        if(dce_folded(f)) did = 1;
        /* Standard DCE */
        if(dce_pass(f)) did = 1;
        if(!did) break;
        changes += did;
    }
    return changes;
}

void opt_run(IRModule *m){
    for(uint32_t i=0;i<m->nfuncs;i++){
        opt_func(&m->funcs[i]);
    }
}
