#include <stdint.h>
#include <string.h>

/* Global FP immediate table. Shared between the parser, the builder, and
   every target backend. Index is a small int stored in IRInstr.args[i] when
   kinds[i] == ARG_FP. */
#define IR_FPIMM_MAX 4096

static double g_tab[IR_FPIMM_MAX];
static int    g_n = 0;

int ir_fpimm_add(double d){
    int i = g_n++;
    if(i >= IR_FPIMM_MAX) i = IR_FPIMM_MAX - 1;
    g_tab[i] = d;
    return i;
}
double ir_fpimm_get(int idx){
    if(idx < 0 || idx >= IR_FPIMM_MAX) return 0.0;
    return g_tab[idx];
}
int ir_fpimm_count(void){ return g_n; }
