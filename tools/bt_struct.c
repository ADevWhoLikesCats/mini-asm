/* bt_struct: build a Point struct via the builder, store/load fields,
   return sum. Exits 42. */
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
    snprintf(sp, sizeof sp, "/tmp/bts_%d.s", (int)getpid());
    snprintf(op, sizeof op, "/tmp/bts_%d.o", (int)getpid());
    FILE *f = fopen(sp, "w");
    if(!f){ perror("fopen"); return 1; }
    target_emit_dispatch(m, f, t);
    fclose(f);
    snprintf(cmd, sizeof cmd, "%s %s -o %s", t->assembler, sp, op);
    if(system(cmd)){ fprintf(stderr, "as failed\n"); return 1; }
    snprintf(cmd, sizeof cmd, "gcc %s -o /tmp/bts_bin_%d", op, (int)getpid());
    if(system(cmd)){ fprintf(stderr, "link failed\n"); return 1; }
    snprintf(cmd, sizeof cmd, "/tmp/bts_bin_%d", (int)getpid());
    int rc = system(cmd);
    int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : 128;
    printf("%-8s exit=%d want=%d  %s\n", target, code, expect,
           code==expect ? "PASS" : "FAIL");
    return code==expect ? 0 : 1;
}

int main(void){
    IRModule  *m   = ir_module_new();
    IRBuilder *b   = ir_builder_new(m);
    IRType    *i32 = ir_type_i32();

    IRType *fields[2] = { i32, i32 };
    IRType *Point = ir_type_struct("Point", fields, 2);

    IRFunc  *f = ir_builder_func(b, "main", i32);
    IRBlock *e = ir_builder_block(b, f, "entry");
    ir_set_insert(b, e);

    IRValue *p   = ir_alloca(b, Point, Point->u.struct_.size);
    IRValue *f0  = ir_gep_field(b, Point, p, 0);
    IRValue *f1  = ir_gep_field(b, Point, p, 1);
    ir_store(b, i32, ir_const_i(i32, 7),  f0);
    ir_store(b, i32, ir_const_i(i32, 35), f1);
    IRValue *a = ir_load(b, i32, f0);
    IRValue *c = ir_load(b, i32, f1);
    IRValue *s = ir_add(b, i32, a, c);
    ir_ret(b, s);

    return emit_run(m, "x86_64", 42);
}
