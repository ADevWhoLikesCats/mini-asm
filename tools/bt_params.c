/* bt_params: build a function with two params via the builder. */
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
    /* raw ld, no libc — matches the runner */
    const char *ld = t->linker ? t->linker : "ld";
    snprintf(cmd, sizeof cmd, "%s %s -o /tmp/btp_bin_%d", ld, op, (int)getpid());
    if(system(cmd)){ fprintf(stderr, "link failed\n"); return 1; }
    const char *run = "";
    if(!strcmp(target,"arm64")) run = "qemu-aarch64 ";
    else if(!strcmp(target,"riscv")) run = "qemu-riscv64 ";
    else if(!strcmp(target,"arm")) run = "qemu-arm ";
    snprintf(cmd, sizeof cmd, "%s/tmp/btp_bin_%d", run, (int)getpid());
    int rc = system(cmd);
    int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : 128;
    printf("%-8s exit=%d want=%d  %s\n", target, code, expect,
           code==expect ? "PASS" : "FAIL");
    return code==expect ? 0 : 1;
}

static IRModule *build(void){
    IRModule  *m = ir_module_new();
    IRBuilder *b = ir_builder_new(m);
    IRType *i32 = ir_type_i32();

    /* func add(i32, i32) i32 { return arg0 + arg1; } */
    IRFunc *add = ir_builder_func(b, "add", i32);
    IRType *params[2] = { i32, i32 };
    ir_builder_params(b, add, params, 2);
    IRBlock *e = ir_builder_block(b, add, "entry");
    ir_set_insert(b, e);
    IRValue *sum = ir_add(b, i32, ir_arg(b,0), ir_arg(b,1));
    ir_ret(b, sum);

    /* func main() i32 { return add(10, 32); } */
    IRFunc *main_fn = ir_builder_func(b, "main", i32);
    IRBlock *me = ir_builder_block(b, main_fn, "entry");
    ir_set_insert(b, me);
    IRValue *args[2] = { ir_const_i(i32,10), ir_const_i(i32,32) };
    IRValue *call = ir_call(b, i32, "add", args, 2);
    ir_ret(b, call);
    return m;
}

int main(void){
    IRModule *m = build();
    int rc = 0;
    rc |= emit_run(m, "x86_64", 42);
    rc |= emit_run(m, "arm64", 42);
    rc |= emit_run(m, "riscv", 42);
    rc |= emit_run(m, "x86", 42);
    rc |= emit_run(m, "arm", 42);
    return rc;
}
