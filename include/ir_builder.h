#ifndef IR_BUILDER_H
#define IR_BUILDER_H
#include "ir.h"
#include <stdint.h>

typedef struct IRValue IRValue;
typedef struct IRBuilder IRBuilder;

/* Type singletons */
IRType *ir_type_void(void);
IRType *ir_type_i1(void);
IRType *ir_type_i8(void);
IRType *ir_type_i16(void);
IRType *ir_type_i32(void);
IRType *ir_type_i64(void);
IRType *ir_type_f32(void);
IRType *ir_type_f64(void);
IRType *ir_type_ptr(void);

/* Values */
IRValue *ir_const_i(IRType *t, int64_t v);
IRValue *ir_const_f(IRType *t, double d);

/* Builder */
IRBuilder *ir_builder_new(IRModule *m);
void       ir_builder_free(IRBuilder *b);

IRFunc  *ir_builder_func(IRBuilder *b, const char *name, IRType *ret);
void     ir_builder_params(IRBuilder *b, IRFunc *f, IRType **types, int n);
IRBlock *ir_builder_block(IRBuilder *b, IRFunc *f, const char *name);
void     ir_set_insert(IRBuilder *b, IRBlock *blk);
IRBlock *ir_current_block(IRBuilder *b);
IRFunc  *ir_current_func(IRBuilder *b);

/* Arithmetic */
IRValue *ir_add (IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_sub (IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_mul (IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_sdiv(IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_udiv(IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_srem(IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_urem(IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_and (IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_or  (IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_xor (IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_shl (IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_lshr(IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_ashr(IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_neg (IRBuilder *b, IRType *t, IRValue *x);
IRValue *ir_not (IRBuilder *b, IRType *t, IRValue *x);

/* Float */
IRValue *ir_fadd(IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_fsub(IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_fmul(IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_fdiv(IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_fneg(IRBuilder *b, IRType *t, IRValue *x);

/* Comparisons (result i1) */
IRValue *ir_icmp(IRBuilder *b, int pred, IRValue *x, IRValue *y);
IRValue *ir_fcmp(IRBuilder *b, IRType *t, int pred, IRValue *x, IRValue *y);

/* Memory */
IRValue *ir_alloca(IRBuilder *b, IRType *t, uint32_t bytes);
IRValue *ir_load  (IRBuilder *b, IRType *t, IRValue *ptr);
void     ir_store (IRBuilder *b, IRType *t, IRValue *val, IRValue *ptr);
IRValue *ir_gep   (IRBuilder *b, IRType *elem, IRValue *base, IRValue *idx);

/* Control flow */
void     ir_br (IRBuilder *b, IRBlock *dest);
void     ir_cbr(IRBuilder *b, IRValue *cond, IRBlock *t, IRBlock *f);
IRValue *ir_phi(IRBuilder *b, IRType *t, IRValue *v0, IRBlock *b0, IRValue *v1, IRBlock *b1);
void     ir_ret(IRBuilder *b, IRValue *v);
void     ir_unreachable(IRBuilder *b);

/* Calls */
IRValue *ir_call(IRBuilder *b, IRType *ret, const char *callee, IRValue **args, int nargs);

#endif
