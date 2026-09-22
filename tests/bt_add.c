/* bt_add: builds (10+20)*3 via the IR builder, emits x86_64, assembles,
   links, runs, checks exit code == 90. */
#include "ir.h"
#include "ir_builder.h"
#include "backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int emit_run(IRModule *m, const char *target, int expect){
    const TargetDesc *t = backend_lookup(target);
    if(!t){ fprintf(stderr, "unknown target %s\n", target); return 1; }
    char sp[256], op[256], cmd[1024];
    snprintf(sp, sizeof sp, "/tmp/bt_%d.s",  (int)getpid());
    snprintf(op, sizeof op, "/tmp/bt_%d.o",  (int)getpid());
    FILE *f = fopen(sp, "w");
    if(!f){ perror("fopen"); return 1; }
    target_emit_dispatch(m, f, t);
    fclose(f);
    snprintf(cmd, sizeof cmd, "%s %s -o %s", t->assembler, sp, op);
    if(system(cmd)){ fprintf(stderr, "assembler failed\n"); return 1; }
    snprintf(cmd, sizeof cmd, "gcc %s -o /tmp/bt_bin_%d", op, (int)getpid());
    if(system(cmd)){ fprintf(stderr, "link failed\n"); return 1; }
    snprintf(cmd, sizeof cmd, "/tmp/bt_bin_%d", (int)getpid());
    int rc = system(cmd);
    int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : 128;
    printf("%-8s exit=%d want=%d  %s\n", target, code, expect,
           code==expect ? "PASS" : "FAIL");
    return code==expect ? 0 : 1;
}

static IRModule *build_prog(void){
    IRModule  *m = ir_module_new();
    IRBuilder *b = ir_builder_new(m);
    IRType    *i32 = ir_type_i32();
    IRFunc    *f = ir_builder_func(b, "main", i32);
    IRBlock   *e = ir_builder_block(b, f, "entry");
    ir_set_insert(b, e);

    IRValue *t10 = ir_const_i(i32, 10);
    IRValue *t20 = ir_const_i(i32, 20);
    IRValue *sum = ir_add(b, i32, t10, t20);
    IRValue *t3  = ir_const_i(i32, 3);
    IRValue *prod= ir_mul(b, i32, sum, t3);
    ir_ret(b, prod);
    return m;
}

int main(void){
    IRModule *m = build_prog();
    printf("built %u func, %u block, %u instrs\n",
           m->nfuncs, m->funcs[0].nblocks, m->funcs[0].blocks[0].ninstrs);
    int rc = emit_run(m, "x86_64", 90);
    return rc;
}
