#include "backend.h"
#include "ir.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

/* Physical registers available for allocation (x86_64).
   Deliberately excludes %rsp and %rbp. */
/* Allocatable registers. Excluded: %rsp, %rbp (structural), %rax, %rdx
   (clobbered by idiv/div and used as scratch), %r10, %r11 (scratch). */
static const char *PHYS[] = {
    "%rsi","%rdi","%r8","%r9",
    "%rbx","%r12","%r13","%r14","%r15"
};
#define NREG ((int)(sizeof PHYS / sizeof *PHYS))
#define NREG_CALLER_SAVED 4   /* first 9 are clobbered by call */
#define NREG_CALLEE_SAVED 5   /* last 5 survive call */

/* Per-target register pools. The defaults match x86_64. */
const TargetRegInfo x86_64_reginfo = {
    .names = (const char*[]){
        "%rsi","%rdi","%r8","%r9",
        "%rbx","%r12","%r13","%r14","%r15"
    },
    .nregs = 9,
    .n_caller_saved = 4,
};
const TargetRegInfo arm64_reginfo = {
    .names = (const char*[]){
        "x11","x12","x13","x14",
        "x19","x20","x21","x22","x23"
    },
    .nregs = 9,
    .n_caller_saved = 4,
};

const TargetRegInfo riscv_reginfo = {
    .names = (const char*[]){
        "t2","t3","t4","t5",
        "s1","s2","s3","s4","s5"
    },
    .nregs = 9,
    .n_caller_saved = 4,
};
const TargetRegInfo x86_reginfo = {
    .names = (const char*[]){
        "%ebx","%esi","%edi"
    },
    .nregs = 3,
    .n_caller_saved = 0,
};
const TargetRegInfo arm_reginfo = {
    .names = (const char*[]){
        "r4","r5","r6","r7","r8","r9","r10","r11"
    },
    .nregs = 8,
    .n_caller_saved = 0,
};
static const TargetRegInfo *g_target = NULL;

const char *regalloc_name(const RegAlloc *ra, int idx){
    const TargetRegInfo *info = (ra && ra->info) ? ra->info : &x86_64_reginfo;
    return (idx>=0 && idx<info->nregs) ? info->names[idx] : NULL;
}

const char *regalloc_regname(int idx){
    const TargetRegInfo *info = g_target ? g_target : &x86_64_reginfo;
    return (idx>=0 && idx<info->nregs) ? info->names[idx] : NULL;
}
const char *regalloc_regname_for(int idx, const TargetRegInfo *info){
    if(!info) info = &x86_64_reginfo;
    return (idx>=0 && idx<info->nregs) ? info->names[idx] : NULL;
}
int regalloc_nregs(void){ return (g_target?g_target:&x86_64_reginfo)->nregs; }

RegAlloc regalloc_run_target(IRFunc *f, const TargetRegInfo *info){
    const TargetRegInfo *saved = g_target;
    g_target = info;
    RegAlloc ra = regalloc_run(f);
    g_target = saved;
    ra.info = info;
    return ra;
}

/* --- Interval bookkeeping --- */
typedef struct {
    int vreg;
    int start;
    int end;
    int crosses_call;
    int force_spill;
    int assigned;   /* -1 unset, -2 spilled, else register idx */
} Interval;

typedef struct {
    int *reg_of;
    int  nvregs;
    int  nspills;
    int  nassigned;
} RegAllocInternal;

/* We iterate blocks in order, assign each instruction a "position".
   We then build intervals keyed by vreg. */
typedef struct {
    Interval *iv;
    int       n;
    int       cap;
} IvList;

static Interval *find_iv(IvList *L, int vreg){
    for(int i=0;i<L->n;i++) if(L->iv[i].vreg == vreg) return &L->iv[i];
    return NULL;
}
static Interval *add_iv(IvList *L, int vreg){
    if(L->n == L->cap){
        L->cap = L->cap ? L->cap*2 : 32;
        L->iv = realloc(L->iv, L->cap * sizeof(Interval));
    }
    Interval *i = &L->iv[L->n++];
    i->vreg = vreg;
    i->start = INT_MAX;
    i->end = -1;
    i->crosses_call = 0;
    i->force_spill = 0;
    i->assigned = -1;
    return i;
}
static void touch_use(IvList *L, int vreg, int pos){
    Interval *i = find_iv(L, vreg);
    if(!i) i = add_iv(L, vreg);
    if(pos < i->start) i->start = pos;
    if(pos > i->end)   i->end   = pos;
}
static void touch_def(IvList *L, int vreg, int pos){
    Interval *i = find_iv(L, vreg);
    if(!i) i = add_iv(L, vreg);
    if(pos < i->start) i->start = pos;
    if(pos > i->end)   i->end   = pos;
}
static void mark_call_crossing(IvList *L, int call_pos){
    for(int i=0;i<L->n;i++)
        if(L->iv[i].start <= call_pos && L->iv[i].end >= call_pos)
            L->iv[i].crosses_call = 1;
}

/* Compute intervals by walking blocks in order.
   Position counter increments per instruction; phi copies on pred edges count
   as uses of the source vreg at the pred block's tail. */
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

static IvList compute_intervals(IRFunc *f, int *out_nvregs){
    IvList L = {0};
    int pos = 0;
    int maxv = -1;
    int *block_start = calloc(f->nblocks, sizeof(int));
    int *block_end   = calloc(f->nblocks, sizeof(int));

    /* Pass 1: assign positions and record uses/defs. */
    for(uint32_t bi=0; bi<f->nblocks; bi++){
        IRBlock *b = &f->blocks[bi];
        block_start[bi] = pos;
        for(uint32_t ii=0; ii<b->ninstrs; ii++){
            IRInstr *in = &b->instrs[ii];
            pos++;
            if(in->op == OP_PHI){
                /* phi: dst defined at block entry, args used at predecessors' tails.
                   We record the dst def here and defer arg uses. */
                if(in->dst >= 0){
                    touch_def(&L, in->dst, block_start[bi]);
                    if(in->dst > maxv) maxv = in->dst;
                }
                continue;
            }
            int fp_class = is_fp_class(in->op);
            for(uint32_t a=0; a<in->nargs; a++){
                if(in->kinds[a] == ARG_VREG){
                    touch_use(&L, in->args[a], pos);
                    if(fp_class){ Interval *iv = find_iv(&L, in->args[a]);
                                  if(iv) iv->force_spill = 1; }
                    if(in->args[a] > maxv) maxv = in->args[a];
                }
            }
            if(in->dst >= 0){
                touch_def(&L, in->dst, pos);
                if(fp_class){ Interval *iv = find_iv(&L, in->dst);
                              if(iv) iv->force_spill = 1; }
                if(in->dst > maxv) maxv = in->dst;
            }
            /* call-crossing is marked in pass 3 below */
        }
        block_end[bi] = pos;
    }

    /* Pass 2: phi arg uses at ends of predecessor blocks. */
    for(uint32_t bi=0; bi<f->nblocks; bi++){
        IRBlock *b = &f->blocks[bi];
        for(uint32_t ii=0; ii<b->ninstrs; ii++){
            IRInstr *in = &b->instrs[ii];
            if(in->op != OP_PHI) continue;
            const char *pred_names[2] = { in->label, in->label2 };
            for(int s=0; s<2; s++){
                if(!pred_names[s] || in->kinds[s] != ARG_VREG) continue;
                for(uint32_t pj=0; pj<f->nblocks; pj++){
                    if(!strcmp(f->blocks[pj].name, pred_names[s])){
                        /* Use lives at the end of that pred block. */
                        int end_pos = block_end[pj] > 0 ? block_end[pj] : 0;
                        touch_use(&L, in->args[s], end_pos);
                        break;
                    }
                }
            }
        }
    }

    /* Params occupy vregs PARAM_BASE..PARAM_BASE+nparams-1, used from entry. */
    for(uint32_t p=0; p<f->nparams; p++){
        int v = 1000 + (int)p;   /* keep in sync with parser/backend PARAM_BASE */
        Interval *iv = find_iv(&L, v);
        if(iv){ iv->start = 0; }
        if(v > maxv) maxv = v;
    }

    /* Also handle params that are used but never defined: create a fake interval. */
    for(uint32_t p=0; p<f->nparams; p++){
        int v = 1000 + (int)p;
        if(!find_iv(&L, v)){
            Interval *i = add_iv(&L, v);
            i->start = 0;
            i->end = 0;
        }
    }

    /* Pass 3: now that all intervals have their final end, mark call-crossing. */
    {
        int p3 = 0;
        for(uint32_t bi=0; bi<f->nblocks; bi++){
            IRBlock *bb = &f->blocks[bi];
            for(uint32_t ii=0; ii<bb->ninstrs; ii++){
                IRInstr *in = &bb->instrs[ii];
                p3++;
                if(in->op == OP_CALL){
                    for(int k=0;k<L.n;k++){
                        if(L.iv[k].start <= p3 && L.iv[k].end >= p3)
                            L.iv[k].crosses_call = 1;
                    }
                }
            }
        }
    }

    /* Extend live ranges across loop back edges. A vreg that is live
       anywhere inside a loop must remain live until the end of the loop,
       because the last use is cyclically re-live on each iteration. */
    for(uint32_t bi=0; bi<f->nblocks; bi++){
        IRBlock *b = &f->blocks[bi];
        for(uint32_t ii=0; ii<b->ninstrs; ii++){
            IRInstr *in = &b->instrs[ii];
            const char *succs[2] = {NULL,NULL};
            if(in->op==OP_BR) succs[0]=in->label;
            else if(in->op==OP_CBR){succs[0]=in->label;succs[1]=in->label2;}
            for(int s=0;s<2;s++){
                if(!succs[s]) continue;
                for(uint32_t sj=0; sj<f->nblocks; sj++){
                    if(strcmp(f->blocks[sj].name, succs[s])) continue;
                    if(block_start[sj] <= block_start[bi]){
                        int lo = block_start[sj], hi = block_end[bi];
                        for(int k=0;k<L.n;k++){
                            Interval *iv = &L.iv[k];
                            if(iv->start <= hi && iv->end >= lo){
                                if(iv->end < hi) iv->end = hi;
                            }
                        }
                    }
                    break;
                }
            }
        }
    }

    *out_nvregs = maxv + 1;
    free(block_start);
    free(block_end);
    return L;
}

/* Sort intervals by start position. */
static int cmp_iv(const void *a, const void *b){
    const Interval *x = a, *y = b;
    return x->start - y->start;
}

/* Linear scan over the interval list. */
static RegAlloc regalloc_run_internal(IRFunc *f){
    int nvregs = 0;
    IvList L = compute_intervals(f, &nvregs);

    if(getenv("CC_NO_REGALLOC")){
        RegAlloc out;
        out.nvregs = nvregs;
        out.nspills = nvregs;
        out.nassigned = 0;
        out.info = g_target;
        out.reg_of = malloc(nvregs * sizeof(int));
        for(int i=0;i<nvregs;i++) out.reg_of[i] = -1;
        free(L.iv);
        return out;
    }

    /* Sort by start */
    qsort(L.iv, L.n, sizeof(Interval), cmp_iv);

    int nregs = g_target ? g_target->nregs : 9;
    if(nregs > 16) nregs = 16;
    int n_caller = g_target ? g_target->n_caller_saved : 4;

    /* Register availability: -1 free, else vreg holding it. */
    int reg_holder[16];
    for(int i=0;i<nregs;i++) reg_holder[i] = -1;
    int reg_free_until[16];
    for(int i=0;i<nregs;i++) reg_free_until[i] = 0;

    int nassigned = 0, nspills = 0;

    for(int k=0;k<L.n;k++){
        Interval *iv = &L.iv[k];

        /* Expire: release any register whose held interval ends before iv->start. */
        for(int r=0;r<nregs;r++){
            if(reg_holder[r] != -1){
                Interval *h = NULL;
                for(int q=0;q<L.n;q++){
                    if(L.iv[q].vreg == reg_holder[r]){ h = &L.iv[q]; break; }
                }
                if(h && h->end < iv->start){
                    reg_holder[r] = -1;
                    reg_free_until[r] = 0;
                }
            }
        }

        if(iv->force_spill){
            iv->assigned = -2;
            nspills++;
            continue;
        }

        /* If this interval crosses a call, it can only use callee-saved regs. */
        int pool_lo = 0, pool_hi = nregs - 1;
        if(iv->crosses_call){
            pool_lo = n_caller;
            pool_hi = nregs - 1;
        }

        /* Try to assign a register. */
        int pick = -1;
        for(int r=pool_lo; r<=pool_hi; r++){
            if(reg_holder[r] == -1){ pick = r; break; }
        }

        if(pick == -1){
            /* No free register in the allowed pool. Spill the interval with
               the furthest end in that pool. */
            int victim = -1, victim_end = iv->end;
            for(int r=pool_lo; r<=pool_hi; r++){
                Interval *h = NULL;
                for(int q=0;q<L.n;q++){
                    if(L.iv[q].vreg == reg_holder[r]){ h = &L.iv[q]; break; }
                }
                if(h && h->end > victim_end){
                    victim_end = h->end;
                    victim = r;
                }
            }
            if(victim != -1){
                Interval *h = NULL;
                for(int q=0;q<L.n;q++){
                    if(L.iv[q].vreg == reg_holder[victim]){ h = &L.iv[q]; break; }
                }
                if(h){ h->assigned = -2; nspills++; }
                reg_holder[victim] = iv->vreg;
                reg_free_until[victim] = iv->end;
                iv->assigned = victim;
                nassigned++;
            } else {
                iv->assigned = -2;
                nspills++;
            }
        } else {
            reg_holder[pick] = iv->vreg;
            reg_free_until[pick] = iv->end;
            iv->assigned = pick;
            nassigned++;
        }
    }

    /* Build the answer array. */
    RegAlloc out;
    out.nvregs   = nvregs;
    out.nassigned= nassigned;
    out.nspills  = nspills;
    out.reg_of   = malloc(nvregs * sizeof(int));
    for(int i=0;i<nvregs;i++) out.reg_of[i] = -1;
    for(int k=0;k<L.n;k++){
        int v = L.iv[k].vreg;
        if(v >= 0 && v < nvregs){
            out.reg_of[v] = L.iv[k].assigned == -2 ? -1 : L.iv[k].assigned;
        }
    }

    free(L.iv);
    return out;
}

RegAlloc regalloc_run(IRFunc *f){ return regalloc_run_internal(f); }

void regalloc_dump(const RegAlloc *ra, IRFunc *f, FILE *out){
    fprintf(out, "# regalloc for %s\n", f->name);
    fprintf(out, "# %d vregs, %d assigned, %d spilled\n",
            ra->nvregs, ra->nassigned, ra->nspills);
    for(int v=0; v<ra->nvregs; v++){
        int r = ra->reg_of[v];
        const char *name = r >= 0 ? regalloc_regname(r) : "(spill)";
        fprintf(out, "#   %%%d -> %s\n", v, name);
    }
    fprintf(out, "# ------------------------\n");
}
