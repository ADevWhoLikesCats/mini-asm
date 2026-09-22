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

/* --- FP immediate table (per-process; fine for our single-module case) --- */
static double g_fptab[1024];
static int    g_fpn = 0;
static int    fp_add(double d){
    int i = g_fpn++;
    if(i >= 1024) i = 1023;
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

/* --- Named types --- */
#define IB_NTYPE_MAX 128
static struct { char name[64]; IRType *ty; } g_named[IB_NTYPE_MAX];
static int g_named_n = 0;
void ir_type_register(const char *name, IRType *t){
    if(g_named_n >= IB_NTYPE_MAX) return;
    snprintf(g_named[g_named_n].name, 64, "%s", name);
    g_named[g_named_n].ty = t;
    g_named_n++;
    if(!t->name) t->name = name;
}
IRType *ir_type_named(const char *name){
    for(int i=0;i<g_named_n;i++) if(!strcmp(g_named[i].name, name)) return g_named[i].ty;
    return NULL;
}

static uint32_t type_size_align(IRType *t, uint32_t *align){
    uint32_t a;
    switch(t->kind){
        case TY_I8:  a=1; break;
        case TY_I16: a=2; break;
        case TY_I32: case TY_F32: a=4; break;
        case TY_I64: case TY_F64: case TY_PTR: a=8; break;
        case TY_STRUCT: a=t->u.struct_.align; break;
        case TY_ARRAY:  a=1; break;
        default: a=4; break;
    }
    if(align) *align = a;
    switch(t->kind){
        case TY_I8:  return 1;
        case TY_I16: return 2;
        case TY_I32: case TY_F32: return 4;
        case TY_I64: case TY_F64: case TY_PTR: return 8;
        case TY_STRUCT: return t->u.struct_.size;
        case TY_ARRAY:  return t->u.array.size;
        default: return 4;
    }
}

IRType *ir_type_struct(const char *name, IRType **fields, uint32_t nfields){
    IRType *t = calloc(1,sizeof *t);
    t->kind = TY_STRUCT;
    t->u.struct_.fields  = fields;
    t->u.struct_.nfields = nfields;
    t->u.struct_.offsets = calloc(nfields, sizeof(uint32_t));
    uint32_t off = 0, max_align = 1;
    for(uint32_t i=0;i<nfields;i++){
        uint32_t fa; uint32_t fs = type_size_align(fields[i], &fa);
        off = (off + fa - 1) & ~(fa - 1);
        t->u.struct_.offsets[i] = off;
        off += fs;
        if(fa > max_align) max_align = fa;
    }
    off = (off + max_align - 1) & ~(max_align - 1);
    t->u.struct_.size  = off;
    t->u.struct_.align = max_align;
    if(name) ir_type_register(name, t);
    return t;
}
IRType *ir_type_array(IRType *elem, uint64_t count){
    IRType *t = calloc(1,sizeof *t);
    t->kind = TY_ARRAY;
    t->u.array.elem  = elem;
    t->u.array.count = count;
    uint32_t a; uint32_t esz = type_size_align(elem, &a);
    t->u.array.size = (uint32_t)count * esz;
    return t;
}

/* --- Values --- */
IRValue *ir_const_i(IRType *t, int64_t v){
    IRValue *x = calloc(1,sizeof *x);
    x->is_const = 1; x->i = v; x->type = t;
    return x;
}
IRValue *ir_const_f(IRType *t, double d){
    IRValue *x = calloc(1,sizeof *x);
    x->is_const = 2; x->f = d; x->type = t;
    return x;
}

/* --- Builder lifecycle --- */
IRBuilder *ir_builder_new(IRModule *m){
    IRBuilder *b = calloc(1,sizeof *b);
    b->m = m; b->next_vreg = 0;
    return b;
}
void ir_builder_free(IRBuilder *b){ free(b); }

IRValue *ir_arg(IRBuilder *b, int i){
    (void)b;
    IRValue *v = calloc(1,sizeof *v);
    v->is_const = 0;
    v->vreg = 1000 + i;   /* PARAM_BASE */
    return v;
}
IRFunc *ir_builder_func(IRBuilder *b, const char *name, IRType *ret){
    b->cur_func = ir_func_new(b->m, name, ret);
    b->next_vreg = 0;
    return b->cur_func;
}
void ir_builder_params(IRBuilder *b, IRFunc *f, IRType **types, int n){
    (void)b; f->nparams = (uint32_t)n; f->params = types;
}
IRBlock *ir_builder_block(IRBuilder *b, IRFunc *f, const char *name){
    (void)b; return ir_block_new(f, name);
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
    if(!v){ e->kinds[i] = ARG_NONE; return; }
    if(v->is_const == 0){ e->kinds[i] = ARG_VREG; e->args[i] = v->vreg; }
    else if(v->is_const == 1){ e->kinds[i] = ARG_IMM; e->args[i] = (int)v->i; }
    else if(v->is_const == 2){ e->kinds[i] = ARG_FP;  e->args[i] = fp_add(v->f); }
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
        case TY_I8: esz=1; break;
        case TY_I16: esz=2; break;
        case TY_I32: case TY_F32: esz=4; break;
        case TY_I64: case TY_F64: case TY_PTR: esz=8; break;
        case TY_STRUCT: esz=(int)elem->u.struct_.size; break;
        case TY_ARRAY:  esz=(int)elem->u.array.size; break;
        default: esz=4; break;
    }
    e->pred = (uint32_t)esz;
    e->type = elem;
    return r;
}
IRValue *ir_gep_field(IRBuilder *b, IRType *st, IRValue *base, uint32_t fld){
    if(st->kind != TY_STRUCT || fld >= st->u.struct_.nfields) return base;
    int ix = ir_emit(b->cur_block, OP_GEP_FIELD, ir_type_i32(),
                     base->is_const==0 ? base->vreg : 0, -1, -1, -1, 1);
    IRInstr *e = &b->cur_block->instrs[ix];
    IRValue *d = new_vreg_val(b);
    e->dst = d->vreg;
    set_arg(e, 0, base);
    e->pred = st->u.struct_.offsets[fld];
    e->type = st;
    return d;
}

/* --- Strings --- */
IRValue *ir_alloca_type(IRBuilder *b, IRType *t){
    uint32_t sz = 4;
    switch(t->kind){
        case TY_I8: sz=1; break;
        case TY_I16: sz=2; break;
        case TY_I32: case TY_F32: sz=4; break;
        case TY_I64: case TY_F64: case TY_PTR: sz=8; break;
        case TY_STRUCT: sz=t->u.struct_.size; break;
        case TY_ARRAY:  sz=t->u.array.size; break;
        default: sz=4; break;
    }
    return ir_alloca(b, t, sz);
}
IRValue *ir_str_cstr(IRBuilder *b, const char *s){
    return ir_str(b, s, (uint32_t)strlen(s));
}
IRValue *ir_str(IRBuilder *b, const char *bytes, uint32_t len){
    int idx = ir_add_string(b->m, bytes, len);
    int ix = ir_emit(b->cur_block, OP_STR, ir_type_i32(), idx, -1, -1, -1, 1);
    IRInstr *e = &b->cur_block->instrs[ix];
    IRValue *d = new_vreg_val(b);
    e->dst = d->vreg;
    e->kinds[0] = ARG_IMM;
    e->args[0]  = idx;
    return d;
}

/* --- Conversions --- */
static IRValue *emit_conv(IRBuilder *b, IROpcode op, IRType *from, IRType *to, IRValue *x){
    int ix = ir_emit(b->cur_block, op, to, 0, 0, 0, 0, 1);
    IRInstr *e = &b->cur_block->instrs[ix];
    IRValue *d = new_vreg_val(b);
    e->dst = d->vreg;
    set_arg(e, 0, x);
    e->pred = (uint32_t)from->kind;
    return d;
}
IRValue *ir_zext   (IRBuilder *b, IRType *f, IRType *t, IRValue *x){ return emit_conv(b,OP_ZEXT   ,f,t,x); }
IRValue *ir_sext   (IRBuilder *b, IRType *f, IRType *t, IRValue *x){ return emit_conv(b,OP_SEXT   ,f,t,x); }
IRValue *ir_trunc  (IRBuilder *b, IRType *f, IRType *t, IRValue *x){ return emit_conv(b,OP_TRUNC  ,f,t,x); }
IRValue *ir_sitofp (IRBuilder *b, IRType *f, IRType *t, IRValue *x){ return emit_conv(b,OP_SITOFP ,f,t,x); }
IRValue *ir_uitofp (IRBuilder *b, IRType *f, IRType *t, IRValue *x){ return emit_conv(b,OP_UITOFP ,f,t,x); }
IRValue *ir_fptosi (IRBuilder *b, IRType *f, IRType *t, IRValue *x){ return emit_conv(b,OP_FPTOSI ,f,t,x); }
IRValue *ir_fptoui (IRBuilder *b, IRType *f, IRType *t, IRValue *x){ return emit_conv(b,OP_FPTOUI ,f,t,x); }
IRValue *ir_fpext  (IRBuilder *b, IRType *f, IRType *t, IRValue *x){ return emit_conv(b,OP_FPEXT  ,f,t,x); }
IRValue *ir_fptrunc(IRBuilder *b, IRType *f, IRType *t, IRValue *x){ return emit_conv(b,OP_FPTRUNC,f,t,x); }

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
IRValue *ir_phi_n(IRBuilder *b, IRType *t, int n, IRValue **vals, IRBlock **blocks){
    if(n <= 2) return ir_phi(b, t,
                             n>0?vals[0]:NULL, n>0?blocks[0]:NULL,
                             n>1?vals[1]:NULL, n>1?blocks[1]:NULL);
    /* For now, only 2 preds supported by backends — fall back to first 2. */
    return ir_phi(b, t, vals[0], blocks[0], vals[1], blocks[1]);
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
IRValue *ir_call_n(IRBuilder *b, IRType *ret, const char *callee, IRValue **args, int nargs){
    if(nargs > 16) nargs = 16;
    int ix = ir_emit(b->cur_block, OP_CALL, ret, 0, 0, 0, 0, nargs);
    IRInstr *e = &b->cur_block->instrs[ix];
    IRValue *d = new_vreg_val(b);
    e->dst = d->vreg;
    e->callee = callee;
    for(int i=0;i<nargs;i++) set_arg(e, i, args[i]);
    return d;
}
IRValue *ir_call(IRBuilder *b, IRType *ret, const char *callee, IRValue **args, int nargs){
    return ir_call_n(b, ret, callee, args, nargs);
}
