/* bt_printf: use the builder to call printf("sum=%d\n", 40+2). */
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
    snprintf(sp, sizeof sp, "/tmp/btp_%d.s", (int)getpid());
    snprintf(op, sizeof op, "/tmp/btp_%d.o", (int)getpid());
    FILE *f = fopen(sp, "w");
    if(!f){ perror("fopen"); return 1; }
    target_emit_dispatch(m, f, t);
    fclose(f);
    snprintf(cmd, sizeof cmd, "%s %s -o %s", t->assembler, sp, op);
    if(system(cmd)){ fprintf(stderr, "as failed\n"); return 1; }
    snprintf(cmd, sizeof cmd, "gcc %s -o /tmp/btp_bin_%d", op, (int)getpid());
    if(system(cmd)){ fprintf(stderr, "link failed\n"); return 1; }
    snprintf(cmd, sizeof cmd, "/tmp/btp_bin_%d", (int)getpid());
    int rc = system(cmd);
    int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : 128;
    printf("%-8s exit=%d want=%d  %s\n", target, code, expect,
           code==expect ? "PASS" : "FAIL");
    return code==expect ? 0 : 1;
}

int main(void){
    IRModule  *m = ir_module_new();
    IRBuilder *b = ir_builder_new(m);
    IRType    *i32 = ir_type_i32();
    IRFunc    *f = ir_builder_func(b, "main", i32);
    IRBlock   *e = ir_builder_block(b, f, "entry");
    ir_set_insert(b, e);

    IRValue *fmt = ir_str(b, "sum=%d\n", 7);
    IRValue *a = ir_const_i(i32, 40);
    IRValue *c = ir_const_i(i32, 2);
    IRValue *s = ir_add(b, i32, a, c);
    IRValue *args[2] = { fmt, s };
    ir_call(b, i32, "printf", args, 2);

    ir_ret(b, ir_const_i(i32, 0));
    return emit_run(m, "x86_64", 0);
}
