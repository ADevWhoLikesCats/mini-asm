#include "ir_builder.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct IRValue {
    int      is_const;   /* 0=vreg, 1=int imm, 2=fp imm, 3=block */
    int64_t  i;
    double   f;
    IRBlock *block;
    IRType  *type;
    int      vreg;
};

struct IRBuilder {
    IRModule *m;
    IRFunc   *cur_func;
    IRBlock  *cur_block;
    int       next_vreg;
};

/* --- FP immediate side table (shared by all emits) --- */
static double g_fptab[256];
static int    g_fpn = 0;
static int    fp_add(double d){
    int i = g_fpn++;
    if(i >= 256) i = 255;
    g_fptab[i] = d;
    return i;
}

/* --- Type singletons --- */
static IRType *mk(IRTypeKind k){ IRType *t = calloc(1,sizeof *t); t->kind = k; return t; }
static IRType *g_void,*g_i1,*g_i8,*g_i16,*g_i32,*g_i64,*g_f32,*g_f64,*g_ptr;
IRType *ir_type_void(void){ if(!g_void) g_void=mk(TY_VOID); return g_void; }
IRType *ir_type_i1  (void){ if(!g_i1  ) g_i1  =mk(TY_I1  ); return g_i1;   }
IRType *ir_type_i8  (void){ if(!g_i8  ) g_i8  =mk(TY_I8  ); return g_i8;   }
IRType *ir_type_i16 (void){ if(!g_i16 ) g_i16 =mk(TY_I16 ); return g_i16;  }
IRType *ir_type_i32 (void){ if(!g_i32 ) g_i32 =mk(TY_I32 ); return g_i32;  }
IRType *ir_type_i64 (void){ if(!g_i64 ) g_i64 =mk(TY_I64 ); return g_i64;  }
IRType *ir_type_f32 (void){ if(!g_f32 ) g_f32 =mk(TY_F32 ); return g_f32;  }
IRType *ir_type_f64 (void){ if(!g_f64 ) g_f64 =mk(TY_F64 ); return g_f64;  }
IRType *ir_type_ptr (void){ if(!g_ptr ) g_ptr =mk(TY_PTR ); return g_ptr;  }

/* --- Values --- */
IRValue *ir_const_i(IRType *t, int64_t v){
    IRValue *x = calloc(1,sizeof *x);
    x->is_const=1; x->i=v; x->type=t;
    return x;
}
IRValue *ir_const_f(IRType *t, double d){
    IRValue *x = calloc(1,sizeof *x);
    x->is_const=2; x->f=d; x->type=t;
    return x;
}

/* --- Builder lifecycle --- */
IRBuilder *ir_builder_new(IRModule *m){
    IRBuilder *b = calloc(1,sizeof *b);
    b->m=m; b->next_vreg=0;
    return b;
}
void ir_builder_free(IRBuilder *b){ free(b); }

IRFunc *ir_builder_func(IRBuilder *b, const char *name, IRType *ret){
    b->cur_func = ir_func_new(b->m, name, ret);
    b->next_vreg = 0;
    return b->cur_func;
}
void ir_builder_params(IRBuilder *b, IRFunc *f, IRType **types, int n){
    (void)b;
    f->nparams = (uint32_t)n;
    f->params = types;
}
IRBlock *ir_builder_block(IRBuilder *b, IRFunc *f, const char *name){
    (void)b;
    return ir_block_new(f, name);
}
void ir_set_insert(IRBuilder *b, IRBlock *blk){ b->cur_block = blk; }
IRBlock *ir_current_block(IRBuilder *b){ return b->cur_block; }
IRFunc  *ir_current_func (IRBuilder *b){ return b->cur_func; }

/* --- Internal helpers --- */
static IRValue *new_vreg_val(IRBuilder *b){
    IRValue *v = calloc(1,sizeof *v);
    v->is_const = 0;
    v->vreg = b->next_vreg++;
    return v;
}
static void set_arg(IRInstr *e, int i, IRValue *v){
    if(!v){ e->kinds[i]=ARG_NONE; return; }
    if(v->is_const==0){ e->kinds[i]=ARG_VREG; e->args[i]=v->vreg; }
    else if(v->is_const==1){ e->kinds[i]=ARG_IMM; e->args[i]=(int)v->i; }
    else if(v->is_const==2){ e->kinds[i]=ARG_FP;  e->args[i]=fp_add(v->f); }
}

static IRValue *emit1(IRBuilder *b, IROpcode op, IRType *t, IRValue *a){
    int ix = ir_emit(b->cur_block, op, t, 0, 0, 0, 0, 1);
    IRInstr *e = &b->cur_block->instrs[ix];
    IRValue *d = new_vreg_val(b);
    e->dst = d->vreg;
    set_arg(e, 0, a);
    return d;
}
static IRValue *emit2(IRBuilder *b, IROpcode op, IRType *t, IRValue *a, IRValue *c){
    int ix = ir_emit(b->cur_block, op, t, 0, 0, 0, 0, 2);
    IRInstr *e = &b->cur_block->instrs[ix];
    IRValue *d = new_vreg_val(b);
    e->dst = d->vreg;
    set_arg(e, 0, a);
    set_arg(e, 1, c);
    return d;
}

/* --- Arithmetic --- */
IRValue *ir_add (IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_ADD ,t,x,y); }
IRValue *ir_sub (IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_SUB ,t,x,y); }
IRValue *ir_mul (IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_MUL ,t,x,y); }
IRValue *ir_sdiv(IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_SDIV,t,x,y); }
IRValue *ir_udiv(IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_UDIV,t,x,y); }
IRValue *ir_srem(IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_SREM,t,x,y); }
IRValue *ir_urem(IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_UREM,t,x,y); }
IRValue *ir_and (IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_AND ,t,x,y); }
IRValue *ir_or  (IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_OR  ,t,x,y); }
IRValue *ir_xor (IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_XOR ,t,x,y); }
IRValue *ir_shl (IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_SHL ,t,x,y); }
IRValue *ir_lshr(IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_LSHR,t,x,y); }
IRValue *ir_ashr(IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_ASHR,t,x,y); }
IRValue *ir_neg (IRBuilder *b, IRType *t, IRValue *x){ return emit1(b,OP_NEG ,t,x); }
IRValue *ir_not (IRBuilder *b, IRType *t, IRValue *x){ return emit1(b,OP_NOT ,t,x); }

/* --- Float --- */
IRValue *ir_fadd(IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_FADD,t,x,y); }
IRValue *ir_fsub(IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_FSUB,t,x,y); }
IRValue *ir_fmul(IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_FMUL,t,x,y); }
IRValue *ir_fdiv(IRBuilder *b, IRType *t, IRValue *x, IRValue *y){ return emit2(b,OP_FDIV,t,x,y); }
IRValue *ir_fneg(IRBuilder *b, IRType *t, IRValue *x){ return emit1(b,OP_FNEG,t,x); }

/* --- Comparisons --- */
IRValue *ir_icmp(IRBuilder *b, int pred, IRValue *x, IRValue *y){
    IRValue *r = emit2(b, OP_ICMP, ir_type_i1(), x, y);
    IRInstr *e = &b->cur_block->instrs[b->cur_block->ninstrs-1];
    e->pred = (uint32_t)pred;
    return r;
}
IRValue *ir_fcmp(IRBuilder *b, IRType *t, int pred, IRValue *x, IRValue *y){
    IRValue *r = emit2(b, OP_FCMP, t, x, y);
    IRInstr *e = &b->cur_block->instrs[b->cur_block->ninstrs-1];
    e->pred = (uint32_t)pred;
    return r;
}

/* --- Memory --- */
IRValue *ir_alloca(IRBuilder *b, IRType *t, uint32_t bytes){
    int ix = ir_emit(b->cur_block, OP_ALLOCA, t, (int)bytes, 0, 0, 0, 1);
    IRInstr *e = &b->cur_block->instrs[ix];
    IRValue *d = new_vreg_val(b);
    e->dst = d->vreg;
    e->kinds[0] = ARG_IMM;
    e->args[0]  = (int)bytes;
    return d;
}
IRValue *ir_load(IRBuilder *b, IRType *t, IRValue *ptr){ return emit1(b,OP_LOAD,t,ptr); }
void ir_store(IRBuilder *b, IRType *t, IRValue *val, IRValue *ptr){
    int ix = ir_emit(b->cur_block, OP_STORE, t, 0, 0, 0, 0, 2);
    IRInstr *e = &b->cur_block->instrs[ix];
    set_arg(e, 0, val);
    set_arg(e, 1, ptr);
}
IRValue *ir_gep(IRBuilder *b, IRType *elem, IRValue *base, IRValue *idx){
    IRValue *r = emit2(b, OP_GEP, ir_type_i32(), base, idx);
    IRInstr *e = &b->cur_block->instrs[b->cur_block->ninstrs-1];
    int esz = 4;
    switch(elem->kind){
        case TY_I8:  esz=1; break;
        case TY_I16: esz=2; break;
        case TY_I32: case TY_F32: esz=4; break;
        case TY_I64: case TY_F64: case TY_PTR: esz=8; break;
        default: esz=4; break;
    }
    e->pred = (uint32_t)esz;
    e->type = elem;
    return r;
}

/* --- Control flow --- */
void ir_br(IRBuilder *b, IRBlock *dest){
    int ix = ir_emit(b->cur_block, OP_BR, NULL, -1,-1,-1,-1, 0);
    b->cur_block->instrs[ix].label = dest->name;
}
void ir_cbr(IRBuilder *b, IRValue *cond, IRBlock *t, IRBlock *f){
    int ix = ir_emit(b->cur_block, OP_CBR, NULL, -1,-1,-1,-1, 1);
    IRInstr *e = &b->cur_block->instrs[ix];
    set_arg(e, 0, cond);
    e->label  = t->name;
    e->label2 = f->name;
}
IRValue *ir_phi(IRBuilder *b, IRType *t, IRValue *v0, IRBlock *b0, IRValue *v1, IRBlock *b1){
    int ix = ir_emit(b->cur_block, OP_PHI, t, 0, 0, 0, 0, 2);
    IRInstr *e = &b->cur_block->instrs[ix];
    IRValue *d = new_vreg_val(b);
    e->dst = d->vreg;
    set_arg(e, 0, v0);
    set_arg(e, 1, v1);
    e->label  = b0 ? b0->name : "entry";
    e->label2 = b1 ? b1->name : "entry";
    return d;
}
void ir_ret(IRBuilder *b, IRValue *v){
    if(!v){
        ir_emit(b->cur_block, OP_RET, ir_type_void(), -1,-1,-1,-1, 0);
        return;
    }
    int ix = ir_emit(b->cur_block, OP_RET, v->type, 0, 0, 0, 0, 1);
    IRInstr *e = &b->cur_block->instrs[ix];
    set_arg(e, 0, v);
}
void ir_unreachable(IRBuilder *b){
    ir_emit(b->cur_block, OP_UNREACHABLE, NULL, -1,-1,-1,-1, 0);
}

/* --- Calls --- */
IRValue *ir_call(IRBuilder *b, IRType *ret, const char *callee, IRValue **args, int nargs){
    int ix = ir_emit(b->cur_block, OP_CALL, ret, 0, 0, 0, 0, nargs);
    IRInstr *e = &b->cur_block->instrs[ix];
    IRValue *d = new_vreg_val(b);
    e->dst = d->vreg;
    e->callee = callee;
    for(int i=0;i<nargs && i<6;i++) set_arg(e, i, args[i]);
    return d;
}
