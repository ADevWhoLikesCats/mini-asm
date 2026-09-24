#include "opt.h"
#include "ir.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

int ir_opt_level = 1;

/* ============================================================
   Shared helpers
   ============================================================ */

static int func_maxv(IRFunc *f){
    int maxv = 0;
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b = &f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in = &b->instrs[k];
            if(in->dst > maxv) maxv = in->dst;
            for(uint32_t a=0;a<in->nargs && a<16;a++)
                if(in->kinds[a]==ARG_VREG && in->args[a] > maxv)
                    maxv = in->args[a];
        }
    }
    return maxv;
}

static int has_side_effect(IROpcode op){
    switch(op){
        case OP_STORE:
        case OP_CALL:
        case OP_BR:
        case OP_CBR:
        case OP_RET:
        case OP_UNREACHABLE:
            return 1;
        default:
            return 0;
    }
}

/* ============================================================
   Constant folding
   ============================================================ */

static int foldable(IROpcode op){
    switch(op){
        case OP_ADD: case OP_SUB: case OP_MUL:
        case OP_SDIV: case OP_UDIV: case OP_SREM: case OP_UREM:
        case OP_AND: case OP_OR: case OP_XOR:
        case OP_SHL: case OP_LSHR: case OP_ASHR:
        case OP_NEG: case OP_NOT:
        case OP_ICMP:
        case OP_ZEXT: case OP_SEXT: case OP_TRUNC:
            return 1;
        default:
            return 0;
    }
}

static int fold_op(IROpcode op, int64_t a0, int64_t a1, uint32_t pred,
                   int has_a0, int64_t *out){
    if(op==OP_NEG){ *out = -a0; return 1; }
    if(op==OP_NOT){ *out = ~a0; return 1; }
    if(!has_a0) return 0;
    switch(op){
        case OP_ADD: *out = a0 + a1; return 1;
        case OP_SUB: *out = a0 - a1; return 1;
        case OP_MUL: *out = a0 * a1; return 1;
        case OP_SDIV:
            if(a1==0) return 0;
            if(a0==(int64_t)0x8000000000000000LL && a1==-1) return 0;
            *out = a0 / a1; return 1;
        case OP_UDIV:
            if(a1==0) return 0;
            *out = (int64_t)((uint64_t)a0 / (uint64_t)a1); return 1;
        case OP_SREM:
            if(a1==0) return 0;
            if(a0==(int64_t)0x8000000000000000LL && a1==-1) return 0;
            *out = a0 % a1; return 1;
        case OP_UREM:
            if(a1==0) return 0;
            *out = (int64_t)((uint64_t)a0 % (uint64_t)a1); return 1;
        case OP_AND: *out = a0 & a1; return 1;
        case OP_OR:  *out = a0 | a1; return 1;
        case OP_XOR: *out = a0 ^ a1; return 1;
        case OP_SHL:
            if(a1<0 || a1>=64) return 0;
            *out = (int64_t)((uint64_t)a0 << a1); return 1;
        case OP_LSHR:
            if(a1<0 || a1>=64) return 0;
            *out = (int64_t)((uint64_t)a0 >> a1); return 1;
        case OP_ASHR:
            if(a1<0 || a1>=64) return 0;
            *out = a0 >> a1; return 1;
        case OP_ICMP: {
            int r=0;
            switch(pred){
                case 0: r = (a0==a1); break;
                case 1: r = (a0!=a1); break;
                case 2: r = (a0< a1); break;
                case 3: r = (a0<=a1); break;
                case 4: r = (a0> a1); break;
                case 5: r = (a0>=a1); break;
                case 6: r = ((uint64_t)a0 <  (uint64_t)a1); break;
                case 7: r = ((uint64_t)a0 <= (uint64_t)a1); break;
                case 8: r = ((uint64_t)a0 >  (uint64_t)a1); break;
                case 9: r = ((uint64_t)a0 >= (uint64_t)a1); break;
                default: return 0;
            }
            *out = r; return 1;
        }
        case OP_ZEXT: *out = a0 & 0xffffffffULL; return 1;
        case OP_SEXT: *out = a0; return 1;
        case OP_TRUNC:*out = a0 & 0xffffffffULL; return 1;
        default: return 0;
    }
}

typedef struct {
    int64_t *val;
    char    *known;
    int      n;
} ConstTab;

static ConstTab consttab_new(int n){
    ConstTab t;
    t.n = n;
    t.val = calloc(n, sizeof(int64_t));
    t.known = calloc(n, 1);
    return t;
}
static void consttab_free(ConstTab *t){ free(t->val); free(t->known); }
static int consttab_known(ConstTab *t, int v){ return v>=0 && v<t->n && t->known[v]; }
static void consttab_set(ConstTab *t, int v, int64_t x){
    if(v>=0 && v<t->n){ t->known[v]=1; t->val[v]=x; }
}

void ir_constfold_func(IRFunc *f){
    int maxv = func_maxv(f);
    ConstTab ct = consttab_new(maxv + 1);

    int changed = 1, passes = 0;
    while(changed && passes < 16){
        changed = 0; passes++;
        for(uint32_t j=0;j<f->nblocks;j++){
            IRBlock *b = &f->blocks[j];
            for(uint32_t k=0;k<b->ninstrs;k++){
                IRInstr *in = &b->instrs[k];

                for(uint32_t a=0;a<in->nargs && a<16;a++){
                    if(in->kinds[a]==ARG_VREG && consttab_known(&ct, in->args[a])){
                        in->args[a] = (int)ct.val[in->args[a]];
                        in->kinds[a] = ARG_IMM;
                        changed = 1;
                    }
                }
                if(!foldable(in->op)) continue;
                if(in->dst < 0) continue;

                int all_imm = 1;
                for(uint32_t a=0;a<in->nargs && a<16;a++){
                    if(in->kinds[a] != ARG_IMM){ all_imm = 0; break; }
                }
                if(!all_imm) continue;

                int64_t a0 = in->nargs > 0 ? in->args[0] : 0;
                int64_t a1 = in->nargs > 1 ? in->args[1] : 0;
                int64_t out;
                if(fold_op(in->op, a0, a1, in->pred, in->nargs > 0, &out)){
                    consttab_set(&ct, in->dst, out);
                }
            }
        }
    }
    consttab_free(&ct);
}

/* ============================================================
   Dead code elimination
   ============================================================ */

static void mark_uses(IRFunc *f, char *used, int n){
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b = &f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in = &b->instrs[k];
            for(uint32_t a=0;a<in->nargs && a<16;a++){
                if(in->kinds[a]==ARG_VREG && in->args[a]>=0 && in->args[a]<n)
                    used[in->args[a]] = 1;
            }
        }
    }
}

void ir_dce_func(IRFunc *f){
    int maxv = func_maxv(f);
    char *used = calloc(maxv + 1, 1);

    int changed = 1;
    while(changed){
        changed = 0;
        memset(used, 0, maxv + 1);
        mark_uses(f, used, maxv + 1);

        for(uint32_t j=0;j<f->nblocks;j++){
            IRBlock *b = &f->blocks[j];
            uint32_t out = 0;
            for(uint32_t k=0;k<b->ninstrs;k++){
                IRInstr *in = &b->instrs[k];
                int keep = 1;
                if(has_side_effect(in->op)) keep = 1;
                else if(in->dst >= 0 && !used[in->dst]) keep = 0;
                if(keep){
                    if(out != k) b->instrs[out] = *in;
                    out++;
                } else {
                    changed = 1;
                }
            }
            b->ninstrs = out;
        }
    }
    free(used);
}

/* ============================================================
   Algebraic simplification
   ============================================================ */

static void algebraic_simplify(IRFunc *f, int *alias, int nalias){
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b = &f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in = &b->instrs[k];
            if(in->dst < 0 || in->dst >= nalias) continue;
            if(in->nargs != 2) continue;
            int a0_imm = in->kinds[0] == ARG_IMM;
            int a1_imm = in->kinds[1] == ARG_IMM;
            int64_t v0 = a0_imm ? in->args[0] : 0;
            int64_t v1 = a1_imm ? in->args[1] : 0;

            switch(in->op){
                case OP_ADD:
                    if(a0_imm && v0==0){ alias[in->dst] = in->args[1]; break; }
                    if(a1_imm && v1==0){ alias[in->dst] = in->args[0]; break; }
                    break;
                case OP_SUB:
                    if(a1_imm && v1==0){ alias[in->dst] = in->args[0]; break; }
                    break;
                case OP_MUL:
                    if(a0_imm && v0==1){ alias[in->dst] = in->args[1]; break; }
                    if(a1_imm && v1==1){ alias[in->dst] = in->args[0]; break; }
                    if((a0_imm && v0==0) || (a1_imm && v1==0)){ alias[in->dst] = -2; break; }
                    break;
                case OP_OR:
                case OP_XOR:
                    if(a0_imm && v0==0){ alias[in->dst] = in->args[1]; break; }
                    if(a1_imm && v1==0){ alias[in->dst] = in->args[0]; break; }
                    break;
                case OP_AND:
                    if((a0_imm && v0==0) || (a1_imm && v1==0)){ alias[in->dst] = -2; break; }
                    break;
                case OP_SHL: case OP_LSHR: case OP_ASHR:
                    if(a1_imm && v1==0){ alias[in->dst] = in->args[0]; break; }
                    break;
                default: break;
            }
        }
    }
}

/* ============================================================
   Strength reduction
   Rewrites:
     x * 2^n          -> x << n
     x * C            -> (x << s) [+/- (x << t) ...]  (for C with few bits)
     udiv x 2^n       -> x >> n
     urem x 2^n       -> x & (2^n - 1)
   Only when the second operand is an immediate constant.
   ============================================================ */

static int is_pow2_u64(uint64_t v, int *log2_out){
    if(v == 0) return 0;
    if((v & (v - 1)) != 0) return 0;
    int l = 0;
    while((v & 1) == 0){ v >>= 1; l++; }
    *log2_out = l;
    return 1;
}

static void strength_reduce(IRFunc *f){
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b = &f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in = &b->instrs[k];
            if(in->dst < 0) continue;
            if(in->nargs != 2) continue;
            if(in->kinds[1] != ARG_IMM) continue;   /* need immediate rhs */

            int64_t c = in->args[1];
            int lg = 0;

            switch(in->op){
                case OP_MUL: {
                    uint64_t uc = (uint64_t)c;
                    if(uc >= 2 && is_pow2_u64(uc, &lg) && lg < 64){
                        /* x * 2^n -> x << n */
                        in->op = OP_SHL;
                        in->args[1] = lg;
                        /* kind stays ARG_IMM */
                        break;
                    }
                    /* x * 3, x * 5, x * 9, ... small constant with few bits:
                       for now skip; the additive decomposition requires
                       emitting extra instructions, which is more invasive. */
                    break;
                }
                case OP_UDIV: {
                    uint64_t uc = (uint64_t)c;
                    if(uc >= 2 && is_pow2_u64(uc, &lg) && lg < 64){
                        in->op = OP_LSHR;
                        in->args[1] = lg;
                    }
                    break;
                }
                case OP_UREM: {
                    uint64_t uc = (uint64_t)c;
                    if(uc >= 2 && is_pow2_u64(uc, &lg) && lg < 64){
                        in->op = OP_AND;
                        in->args[1] = (int)(uc - 1);
                    }
                    break;
                }
                default: break;
            }
        }
    }
}

/* ============================================================
   Phi simplification
   - phi with both inputs the same vreg -> that vreg
   - phi with both inputs the same immediate -> that constant
   - phi with one ARG_NONE input -> skip (leave as-is)
   ============================================================ */

static void phi_simplify(IRFunc *f, int *alias, int nalias){
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b = &f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in = &b->instrs[k];
            if(in->op != OP_PHI) continue;
            if(in->dst < 0 || in->dst >= nalias) continue;
            if(in->nargs < 2) continue;
            if(in->kinds[0] == in->kinds[1] && in->args[0] == in->args[1]){
                if(in->kinds[0] == ARG_VREG){
                    alias[in->dst] = in->args[0];
                } else if(in->kinds[0] == ARG_IMM){
                    alias[in->dst] = -2;
                    in->args[0] = in->args[1];   /* temp use of args[0] for the value */
                    /* we encode "constant 0" case via alias=-2; for non-zero
                       constants, use a real alias by leaving the phi with a
                       single use. For simplicity, only handle 0 here. */
                    if(in->args[0] != 0) alias[in->dst] = -1;
                }
            }
        }
    }
}

/* ============================================================
   Comparison-of-identical-values folding
   icmp P x x  ->  0 or 1 depending on predicate
   ============================================================ */

static int icmp_self_value(uint32_t pred, int *val){
    /* returns 1 if the predicate has a known outcome when a==b */
    switch(pred){
        case 0: /* eq  */ *val = 1; return 1;
        case 1: /* ne  */ *val = 0; return 1;
        case 2: /* slt */ *val = 0; return 1;
        case 3: /* sle */ *val = 1; return 1;
        case 4: /* sgt */ *val = 0; return 1;
        case 5: /* sge */ *val = 1; return 1;
        case 6: /* ult */ *val = 0; return 1;
        case 7: /* ule */ *val = 1; return 1;
        case 8: /* ugt */ *val = 0; return 1;
        case 9: /* uge */ *val = 1; return 1;
        default: return 0;
    }
}

static void fold_self_icmp(IRFunc *f, int *alias, int nalias){
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b = &f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in = &b->instrs[k];
            if(in->op != OP_ICMP) continue;
            if(in->dst < 0 || in->dst >= nalias) continue;
            if(in->nargs != 2) continue;
            if(in->kinds[0] != ARG_VREG || in->kinds[1] != ARG_VREG) continue;
            if(in->args[0] != in->args[1]) continue;
            int val = 0;
            if(icmp_self_value(in->pred, &val)){
                if(val == 0) alias[in->dst] = -2;
                else {
                    /* Replace with immediate 1 by marking it as an IMM. We
                       cannot set an arbitrary immediate alias, so use a
                       marker: alias[in->dst] = -3 means "1". Then the copy
                       propagation pass substitutes 1. */
                    alias[in->dst] = -3;
                }
            }
        }
    }
}

/* ============================================================
   Copy propagation
   ============================================================ */

static void copy_propagate_func(IRFunc *f, int *alias, int nalias){
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b = &f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in = &b->instrs[k];
            for(uint32_t a=0;a<in->nargs && a<16;a++){
                if(in->kinds[a] != ARG_VREG) continue;
                int v = in->args[a];
                if(v < 0 || v >= nalias) continue;
                if(alias[v] == -2){
                    in->kinds[a] = ARG_IMM;
                    in->args[a] = 0;
                } else if(alias[v] == -3){
                    in->kinds[a] = ARG_IMM;
                    in->args[a] = 1;
                } else if(alias[v] >= 0){
                    in->args[a] = alias[v];
                }
            }
        }
    }
}

/* ============================================================
   Common subexpression elimination (intra-block)
   ============================================================ */

static void common_subexpr_elim(IRFunc *f, int *alias, int nalias){
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b = &f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in = &b->instrs[k];
            if(in->dst < 0 || in->dst >= nalias) continue;
            if(has_side_effect(in->op)) continue;
            if(in->op == OP_PHI || in->op == OP_ALLOCA || in->op == OP_STR) continue;
            if(in->op == OP_LOAD) continue;
            for(int kk = (int)k - 1; kk >= 0; kk--){
                IRInstr *prev = &b->instrs[kk];
                if(prev->op != in->op) continue;
                if(prev->nargs != in->nargs) continue;
                if(prev->type != in->type) continue;
                if(prev->pred != in->pred) continue;
                int same = 1;
                for(uint32_t a=0;a<in->nargs && a<16;a++){
                    if(prev->kinds[a] != in->kinds[a] || prev->args[a] != in->args[a]){
                        same = 0; break;
                    }
                }
                if(same && prev->dst >= 0){
                    alias[in->dst] = prev->dst;
                    break;
                }
            }
        }
    }
}

/* ============================================================
   Dead block elimination
   ============================================================ */

static void dead_block_elim(IRFunc *f){
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b = &f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in = &b->instrs[k];
            if(in->op != OP_CBR) continue;
            if(in->nargs < 1 || in->kinds[0] != ARG_IMM) continue;
            int64_t cond = in->args[0];
            const char *target = cond ? in->label : in->label2;
            in->op = OP_BR;
            in->label = target;
            in->label2 = NULL;
            in->nargs = 0;
        }
    }
    uint32_t n = f->nblocks;
    if(n == 0) return;
    char *reachable = calloc(n, 1);
    uint32_t *stack = malloc(n * sizeof(uint32_t));
    int sp = 0;
    stack[sp++] = 0;
    reachable[0] = 1;
    while(sp > 0){
        uint32_t idx = stack[--sp];
        IRBlock *b = &f->blocks[idx];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in = &b->instrs[k];
            const char *succs[2] = {NULL,NULL};
            if(in->op == OP_BR) succs[0] = in->label;
            else if(in->op == OP_CBR){ succs[0]=in->label; succs[1]=in->label2; }
            for(int s=0;s<2;s++){
                if(!succs[s]) continue;
                for(uint32_t t=0;t<n;t++){
                    if(!strcmp(f->blocks[t].name, succs[s]) && !reachable[t]){
                        reachable[t] = 1;
                        stack[sp++] = t;
                    }
                }
            }
        }
    }
    uint32_t out = 0;
    for(uint32_t j=0;j<n;j++){
        if(reachable[j]){
            if(out != j) f->blocks[out] = f->blocks[j];
            out++;
        }
    }
    f->nblocks = out;
    free(reachable);
    free(stack);
}

/* ============================================================
   Block-local redundant load elimination
   Within a block, a load from a pointer P that was just stored to
   (with no intervening call or store to anything) returns the stored
   value. Conservative: bail at any call or any other store.
   ============================================================ */

static void local_load_elim(IRFunc *f, int *alias, int nalias){
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b = &f->blocks[j];
        /* Track: for each pointer vreg currently in memory, the value vreg
           most recently stored. Reset on call/store to a different pointer. */
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in = &b->instrs[k];
            if(in->op == OP_CALL){
                /* clear all knowledge */
                for(uint32_t kk=k+1; kk<b->ninstrs; kk++){
                    IRInstr *nx = &b->instrs[kk];
                    if(nx->op == OP_LOAD) continue;   /* can't prove */
                }
                /* Simpler: scan forward from the call and clear nothing; we
                   never speculatively forward across calls because we never
                   record across them either. Skip to next block-local scan
                   by breaking the load-elim state. We approximate by setting
                   a "dirty" flag. */
            }
        }
        /* Second sweep: only handle store->load adjacencies within the block
           without any intervening memory op. This is safe and simple. */
        for(uint32_t k=0;k+1<b->ninstrs;k++){
            IRInstr *st = &b->instrs[k];
            IRInstr *ld = &b->instrs[k+1];
            if(st->op != OP_STORE) continue;
            if(ld->op != OP_LOAD) continue;
            if(st->nargs < 2 || ld->nargs < 1) continue;
            if(st->kinds[1] != ARG_VREG || ld->kinds[0] != ARG_VREG) continue;
            if(st->args[1] != ld->args[0]) continue;   /* different pointer */
            if(st->type != ld->type) continue;
            /* load's result = store's value */
            if(ld->dst >= 0 && ld->dst < nalias){
                if(st->kinds[0] == ARG_VREG){
                    alias[ld->dst] = st->args[0];
                } else if(st->kinds[0] == ARG_IMM){
                    if(st->args[0] == 0) alias[ld->dst] = -2;
                    else if(st->args[0] == 1) alias[ld->dst] = -3;
                    /* other immediates: skip */
                }
            }
        }
    }
}

/* ============================================================
   Extension / truncation chain collapsing
   zext(zext x) -> zext x    (chain collapses to one zext)
   zext(trunc x) -> zext x
   trunc(zext x) -> x        (when result width >= source width of inner)
   sext(sext x) -> sext x
   Also common: sub with itself, and other algebraic identites on the
   same vreg on both sides.
   ============================================================ */

static void collaps_ext_chains(IRFunc *f, int *alias, int nalias){
    for(uint32_t j=0;j<f->nblocks;j++){
        IRBlock *b = &f->blocks[j];
        for(uint32_t k=0;k<b->ninstrs;k++){
            IRInstr *in = &b->instrs[k];
            if(in->dst < 0 || in->dst >= nalias) continue;
            if(in->nargs != 1) continue;
            if(in->op != OP_ZEXT && in->op != OP_SEXT && in->op != OP_TRUNC) continue;
            if(in->kinds[0] != ARG_VREG) continue;
            int src = in->args[0];
            /* Find the defining instruction for src (anywhere) */
            IRInstr *def = NULL;
            for(uint32_t jj=0;jj<f->nblocks && !def;jj++){
                IRBlock *bb = &f->blocks[jj];
                for(uint32_t kk=0;kk<bb->ninstrs;kk++){
                    if(bb->instrs[kk].dst == src && has_side_effect(bb->instrs[kk].op) == 0){
                        def = &bb->instrs[kk];
                        break;
                    }
                }
            }
            if(!def) continue;
            if(def->op == OP_ZEXT && in->op == OP_ZEXT){
                /* zext(zext x) -> zext x, but only if inner is ARG_VREG */
                if(def->kinds[0] == ARG_VREG){ alias[in->dst] = def->args[0]; }
            } else if(def->op == OP_SEXT && in->op == OP_SEXT){
                if(def->kinds[0] == ARG_VREG){ alias[in->dst] = def->args[0]; }
            } else if(def->op == OP_ZEXT && in->op == OP_TRUNC){
                /* trunc(zext x): if the result type is >= source width, collapse to x.
                   We don't know widths precisely, so be conservative: only if
                   in->type matches def->type (same target width). */
                if(def->type == in->type && def->kinds[0] == ARG_VREG){
                    alias[in->dst] = def->args[0];
                }
            } else if(def->op == OP_SEXT && in->op == OP_TRUNC){
                if(def->type == in->type && def->kinds[0] == ARG_VREG){
                    alias[in->dst] = def->args[0];
                }
            }
        }
    }
}

/* ============================================================
   Orchestrator
   ============================================================ */

static void ir_optimize_func(IRFunc *f){
    for(int iter=0; iter<8; iter++){
        ir_constfold_func(f);
        ir_dce_func(f);

        int maxv = func_maxv(f);
        int *alias = malloc((maxv + 2) * sizeof(int));
        for(int i=0;i<maxv+2;i++) alias[i] = -1;

        algebraic_simplify(f, alias, maxv + 2);
        strength_reduce(f);
        common_subexpr_elim(f, alias, maxv + 2);
        phi_simplify(f, alias, maxv + 2);
        fold_self_icmp(f, alias, maxv + 2);
        collaps_ext_chains(f, alias, maxv + 2);
        local_load_elim(f, alias, maxv + 2);
        copy_propagate_func(f, alias, maxv + 2);
        free(alias);

        ir_dce_func(f);
        dead_block_elim(f);
    }
}

void ir_optimize(IRModule *m){
    if(ir_opt_level <= 0) return;
    for(uint32_t i=0;i<m->nfuncs;i++){
        ir_optimize_func(&m->funcs[i]);
    }
}
