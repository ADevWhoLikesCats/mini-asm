#include "opt.h"
#include "ir.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

int ir_opt_level = 1;

/* ---------- Constant folding ---------- */

/* Returns 1 if op is foldable when all its operands are constants. */
static int foldable(IROpcode op){
    switch(op){
        case OP_ADD: case OP_SUB: case OP_MUL:
        case OP_SDIV: case OP_UDIV: case OP_SREM: case OP_UREM:
        case OP_AND: case OP_OR: case OP_XOR:
        case OP_SHL: case OP_LSHR: case OP_ASHR:
        case OP_NEG: case OP_NOT:
        case OP_ICMP:
        case OP_ZEXT: case OP_SEXT: case OP_TRUNC:
        case OP_GEP_FIELD:
            return 1;
        default:
            return 0;
    }
}

/* Compute a foldable op given integer operands. Returns 1 on success. */
static int fold_op(IROpcode op, int64_t a0, int64_t a1, uint32_t pred, int has_a0, int64_t *out){
    if(op==OP_NEG){ *out = -a0; return 1; }
    if(op==OP_NOT){ *out = ~a0; return 1; }

    if(!has_a0) return 0;
    switch(op){
        case OP_ADD: *out = a0 + a1; return 1;
        case OP_SUB: *out = a0 - a1; return 1;
        case OP_MUL: *out = a0 * a1; return 1;
        case OP_SDIV:
            if(a1==0) return 0;
            /* avoid INT64_MIN / -1 */
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
        case OP_ZEXT:
            /* we don't know source width from op alone; assume i32 → i32 (identity) */
            *out = a0 & 0xffffffffULL; return 1;
        case OP_SEXT:
            *out = a0; return 1;
        case OP_TRUNC:
            *out = a0 & 0xffffffffULL; return 1;
        case OP_GEP_FIELD:
            *out = a0; return 1; /* address computation needs the runtime address; skip */
        default: return 0;
    }
}

/* Known-constant table. Index by vreg. -1 = unknown (using INT64_MIN as sentinel
   is risky because it's a valid value; use a companion bool array instead). */
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
static void consttab_clear(ConstTab *t, int v){
    if(v>=0 && v<t->n){ t->known[v]=0; }
}

void ir_constfold_func(IRFunc *f){
    /* Find maxv */
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
    ConstTab ct = consttab_new(maxv + 1);

    int changed = 1;
    int passes = 0;
    while(changed && passes < 16){
        changed = 0;
        passes++;
        for(uint32_t j=0;j<f->nblocks;j++){
            IRBlock *b = &f->blocks[j];
            for(uint32_t k=0;k<b->ninstrs;k++){
                IRInstr *in = &b->instrs[k];

                /* Skip terminators and side-effecting ops for folding,
                   but still propagate constants into their operands. */
                int is_foldable = foldable(in->op);

                /* Step 1: replace ARG_VREG operands with immediates where known */
                for(uint32_t a=0;a<in->nargs && a<16;a++){
                    if(in->kinds[a]==ARG_VREG && consttab_known(&ct, in->args[a])){
                        in->args[a] = (int)ct.val[in->args[a]];
                        in->kinds[a] = ARG_IMM;
                        changed = 1;
                    }
                }

                if(!is_foldable) continue;
                if(in->dst < 0) continue;

                /* Step 2: if all operands are ARG_IMM, fold */
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
                    /* Mark the instruction dead by zeroing its args and
                       setting a flag... we can't easily delete here because
                       we're iterating. Just note dst is known and leave. */
                }
            }
        }
    }

    /* Second pass: rewrite dst defs. For instructions whose dst is known-constant,
       replace the instruction with a "mov from immediate" — but simpler: clear
       their operands and mark them as nop-folded by setting nargs=0, op stays
       but backend ignores... Actually the simplest: leave them, but ensure all
       uses are constants. The inst itself still computes something, but its
       result is never read because all uses became immediates. DCE will remove it. */
    consttab_free(&ct);
}

/* ---------- Dead code elimination ---------- */

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

/* Marks all vregs that are used as operands. */
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

/* ---------- Module-level ---------- */

void ir_optimize(IRModule *m){
    if(ir_opt_level <= 0) return;
    for(uint32_t i=0;i<m->nfuncs;i++){
        ir_constfold_func(&m->funcs[i]);
        ir_dce_func(&m->funcs[i]);
    }
}
