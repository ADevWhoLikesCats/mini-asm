#ifndef IR_BUILDER_H
#define IR_BUILDER_H
#include "ir.h"
#include <stdint.h>

typedef struct IRValue IRValue;
typedef struct IRBuilder IRBuilder;

/* --- Type singletons --- */
IRType *ir_type_void(void);
IRType *ir_type_i1(void);
IRType *ir_type_i8(void);
IRType *ir_type_i16(void);
IRType *ir_type_i32(void);
IRType *ir_type_i64(void);
IRType *ir_type_f32(void);
IRType *ir_type_f64(void);
IRType *ir_type_ptr(void);

/* --- Aggregate / named types --- */
IRType *ir_type_struct(const char *name, IRType **fields, uint32_t nfields);
IRType *ir_type_array(IRType *elem, uint64_t count);
void    ir_type_register(const char *name, IRType *t);
IRType *ir_type_named(const char *name);

/* --- Values --- */
IRValue *ir_const_i(IRType *t, int64_t v);
IRValue *ir_const_f(IRType *t, double d);

/* --- Placeholders (for phi cycles and forward references) --- */
/* Create a placeholder. Use it in instructions now; define it later with
   ir_value_define(). At finalize time, every use of the placeholder is
   rewritten to the real value. */
IRValue *ir_value_placeholder(IRBuilder *b, IRType *t);
void     ir_value_define(IRBuilder *b, IRValue *placeholder, IRValue *actual);

/* Finalize the current function: resolve placeholders and apply aliases.
   Safe to call multiple times; idempotent. */
void     ir_builder_finalize(IRBuilder *b);

/* --- Builder lifecycle --- */
IRBuilder *ir_builder_new(IRModule *m);
void       ir_builder_free(IRBuilder *b);

IRFunc  *ir_builder_func(IRBuilder *b, const char *name, IRType *ret);
IRFunc  *ir_builder_func(IRBuilder *b, const char *name, IRType *ret);
void     ir_builder_params(IRBuilder *b, IRFunc *f, IRType **types, int n);
/* Returns an IRValue* that refers to parameter i of the current function
   (matches the `%arg0, %arg1, ...` convention used by the text parser). */
IRValue *ir_arg(IRBuilder *b, int i);
IRBlock *ir_builder_block(IRBuilder *b, IRFunc *f, const char *name);
void     ir_set_insert(IRBuilder *b, IRBlock *blk);
IRBlock *ir_current_block(IRBuilder *b);
IRFunc  *ir_current_func(IRBuilder *b);

/* --- Arithmetic --- */
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

/* --- Float --- */
IRValue *ir_fadd(IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_fsub(IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_fmul(IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_fdiv(IRBuilder *b, IRType *t, IRValue *x, IRValue *y);
IRValue *ir_fneg(IRBuilder *b, IRType *t, IRValue *x);

/* --- Comparisons --- */
IRValue *ir_icmp(IRBuilder *b, int pred, IRValue *x, IRValue *y);
IRValue *ir_fcmp(IRBuilder *b, IRType *t, int pred, IRValue *x, IRValue *y);

/* --- Memory --- */
IRValue *ir_alloca(IRBuilder *b, IRType *t, uint32_t bytes);
IRValue *ir_load  (IRBuilder *b, IRType *t, IRValue *ptr);
void     ir_store (IRBuilder *b, IRType *t, IRValue *val, IRValue *ptr);
void     ir_memcpy(IRBuilder *b, IRValue *dst, IRValue *src, uint32_t size);
IRValue *ir_gep   (IRBuilder *b, IRType *elem, IRValue *base, IRValue *idx);
IRValue *ir_gep_field(IRBuilder *b, IRType *struct_ty, IRValue *base, uint32_t field_index);

/* --- Strings --- */
IRValue *ir_alloca_type(IRBuilder *b, IRType *t);
IRValue *ir_str(IRBuilder *b, const char *bytes, uint32_t len);
/* Convenience: string literal from a NUL-terminated C string (len computed). */
IRValue *ir_str_cstr(IRBuilder *b, const char *s);

/* --- Conversions --- */
IRValue *ir_zext   (IRBuilder *b, IRType *from, IRType *to, IRValue *x);
IRValue *ir_sext   (IRBuilder *b, IRType *from, IRType *to, IRValue *x);
IRValue *ir_trunc  (IRBuilder *b, IRType *from, IRType *to, IRValue *x);
IRValue *ir_sitofp (IRBuilder *b, IRType *from, IRType *to, IRValue *x);
IRValue *ir_uitofp (IRBuilder *b, IRType *from, IRType *to, IRValue *x);
IRValue *ir_fptosi (IRBuilder *b, IRType *from, IRType *to, IRValue *x);
IRValue *ir_fptoui (IRBuilder *b, IRType *from, IRType *to, IRValue *x);
IRValue *ir_fpext  (IRBuilder *b, IRType *from, IRType *to, IRValue *x);
IRValue *ir_fptrunc(IRBuilder *b, IRType *from, IRType *to, IRValue *x);

/* --- Control flow --- */
void     ir_br (IRBuilder *b, IRBlock *dest);
void     ir_cbr(IRBuilder *b, IRValue *cond, IRBlock *t, IRBlock *f);
IRValue *ir_phi(IRBuilder *b, IRType *t, IRValue *v0, IRBlock *b0, IRValue *v1, IRBlock *b1);
/* Arbitrary-predecessor phi. Actually only the first 2 preds are used by the
   current backends, but this is here for future extension. */
IRValue *ir_phi_n(IRBuilder *b, IRType *t, int n, IRValue **vals, IRBlock **blocks);
void     ir_ret(IRBuilder *b, IRValue *v);
void     ir_unreachable(IRBuilder *b);

/* --- Calls --- */
IRValue *ir_call  (IRBuilder *b, IRType *ret, const char *callee, IRValue **args, int nargs);
IRValue *ir_call_n(IRBuilder *b, IRType *ret, const char *callee, IRValue **args, int nargs);

#endif
