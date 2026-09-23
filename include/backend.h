#ifndef BACKEND_H
#define BACKEND_H
#include <stdio.h>
#include "ir.h"
typedef struct TargetDesc TargetDesc;
struct TargetDesc { const char *name,*triple,*assembler,*obj_ext,*linker; uint32_t ptr_size,stack_align; bool little; };
const TargetDesc *backend_lookup(const char *name);
int target_emit_dispatch(IRModule*,FILE*,const TargetDesc*);
int emit_icmp(IRInstr*,FILE*,const TargetDesc*);
int emit_fcmp(IRInstr*,FILE*,const TargetDesc*);
int emit_int_math(IRInstr*,FILE*,const TargetDesc*);
int emit_fp_math(IRInstr*,FILE*,const TargetDesc*);
struct TargetRegInfo;
typedef struct RegAlloc {
    int *reg_of;
    int  nvregs;
    int  nspills;
    int  nassigned;
    const struct TargetRegInfo *info;
} RegAlloc;

RegAlloc regalloc_run(IRFunc *f);
void     regalloc_dump(const RegAlloc *ra, IRFunc *f, FILE *out);
typedef struct TargetRegInfo {
    const char **names;
    int          nregs;
    int          n_caller_saved;
} TargetRegInfo;

extern const TargetRegInfo x86_64_reginfo;
extern const TargetRegInfo arm64_reginfo;
extern const TargetRegInfo riscv_reginfo;
extern const TargetRegInfo x86_reginfo;
extern const TargetRegInfo arm_reginfo;

RegAlloc regalloc_run_target(IRFunc *f, const TargetRegInfo *info);
const char *regalloc_regname_for(int idx, const TargetRegInfo *info);
const char *regalloc_regname(int idx);
const char *regalloc_name(const RegAlloc *ra, int idx);
int      regalloc_nregs(void);

extern int cc_libc_mode;

#endif
