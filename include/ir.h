#ifndef IR_H
#define IR_H
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

typedef enum{TY_VOID,TY_I1,TY_I8,TY_I16,TY_I32,TY_I64,TY_F32,TY_F64,TY_PTR,TY_ARRAY,TY_STRUCT,TY_FUNC}IRTypeKind;
struct IRType;
typedef struct IRType{
    IRTypeKind kind;
    const char *name;
    union {
        struct { struct IRType *elem; } ptr;
        struct { struct IRType *elem; uint64_t count; uint32_t size; } array;
        struct { struct IRType **fields; uint32_t nfields; uint32_t size; uint32_t align; uint32_t *offsets; } struct_;
    } u;
} IRType;

typedef enum{ICMP_EQ,ICMP_NE,ICMP_SLT,ICMP_SLE,ICMP_SGT,ICMP_SGE,ICMP_ULT,ICMP_ULE,ICMP_UGT,ICMP_UGE}IcmpPred;
typedef enum{FCMP_OEQ,FCMP_ONE,FCMP_OLT,FCMP_OLE,FCMP_OGT,FCMP_OGE,FCMP_ORD,FCMP_UNO}FcmpPred;

typedef enum{OP_ADD,OP_SUB,OP_MUL,OP_SDIV,OP_UDIV,OP_SREM,OP_UREM,OP_AND,OP_OR,OP_XOR,OP_SHL,OP_LSHR,OP_ASHR,OP_FADD,OP_FSUB,OP_FMUL,OP_FDIV,OP_FREM,OP_FNEG,OP_NEG,OP_NOT,OP_ICMP,OP_FCMP,OP_LOAD,OP_STORE,OP_GEP,OP_GEP_FIELD,OP_ALLOCA,OP_CALL,OP_PHI,OP_SELECT,OP_ZEXT,OP_SEXT,OP_TRUNC,OP_BITCAST,OP_PTRTOINT,OP_INTTOPTR,OP_STR,OP_SITOFP,OP_UITOFP,OP_FPTOSI,OP_FPTOUI,OP_FPEXT,OP_FPTRUNC,OP_BR,OP_CBR,OP_SWITCH,OP_RET,OP_UNREACHABLE}IROpcode;

typedef enum{ARG_NONE,ARG_VREG,ARG_IMM,ARG_LABEL,ARG_FP}IRArgKind;

typedef struct IRInstr{
    IROpcode op;
    IRType *type;
    int dst;
    int args[16];
    uint8_t kinds[16];
    uint32_t nargs;
    uint32_t pred;
    const char *callee;
    const char *label;
    const char *label2;
} IRInstr;

typedef struct IRBlock{
    const char *name;
    IRInstr *instrs;
    uint32_t ninstrs, cap;
    int nvregs;
} IRBlock;

typedef struct IRFunc{
    const char *name;
    IRType *ret;
    IRType **params;
    uint32_t nparams;
    IRBlock *blocks;
    uint32_t nblocks, cap;
    int nvregs;
    int nlocals;
} IRFunc;

typedef struct IRString{
    const char *label;
    char *bytes;
    uint32_t len;
} IRString;

typedef struct IRModule{
    IRFunc *funcs;
    uint32_t nfuncs, cap;
    IRString *strings;
    uint32_t nstrings, strcap;
} IRModule;

IRModule *ir_module_new(void);
IRFunc   *ir_func_new(IRModule*, const char*, IRType*);
IRBlock  *ir_block_new(IRFunc*, const char*);
int       ir_emit(IRBlock*, IROpcode, IRType*, int a0, int a1, int a2, int a3, int n);
int       ir_add_string(IRModule*, const char *bytes, uint32_t len);
#endif
