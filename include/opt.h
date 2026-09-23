#ifndef OPT_H
#define OPT_H
#include "ir.h"

/* Run constant folding on a function. Rewrites operands in place:
   vreg references to known-constant values become immediates. */
void ir_constfold_func(IRFunc *f);

/* Run dead code elimination on a function. Removes instructions whose
   results are never used and that have no side effects. */
void ir_dce_func(IRFunc *f);

/* Convenience: run both passes on every function in a module. */
void ir_optimize(IRModule *m);

/* Global toggle. When 0, ir_optimize does nothing. Set by -O0/-O1 flags. */
extern int ir_opt_level;

#endif
