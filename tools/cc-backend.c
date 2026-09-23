#include "ir.h"
#include "ir_parse.h"
#include "opt.h"
#include "backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

extern const TargetDesc *backend_lookup(const char*);
extern int target_emit_dispatch(IRModule*,FILE*,const TargetDesc*);
extern void ir_print(IRModule*,FILE*);

static void mkdir_p(const char *path){
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for(char *p = tmp + 1; *p; p++){
        if(*p == '/'){
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

int main(int argc, char **argv){
    const char *outdir = getenv("CC_BACKEND_OUT");
    if(!outdir) outdir = "build";

    if(argc < 4){
        fprintf(stderr,
            "usage: %s <target> <input.ir> <output.o>\n"
            "  or: %s -o <outdir> <target> <input.ir> <basename>\n"
            "\n"
            "targets: x86, x86_64, arm, arm64, riscv\n"
            "env:     CC_BACKEND_OUT=<dir>  (default: build)\n",
            argv[0], argv[0]);
        return 1;
    }

    const char *target_name, *input, *output;
    if(!strcmp(argv[1], "-o") && argc >= 6){
        outdir = argv[2];
        target_name = argv[3];
        input = argv[4];
        output = argv[5];
    } else {
        target_name = argv[1];
        input = argv[2];
        output = argv[3];
    }

    const TargetDesc *t = backend_lookup(target_name);
    if(!t){ fprintf(stderr, "unknown target: %s\n", target_name); return 1; }

    mkdir_p(outdir);

    /* Compute output paths: <outdir>/<basename>.s and <outdir>/<basename>.o */
    char base[512];
    const char *b = strrchr(output, '/');
    b = b ? b + 1 : output;
    snprintf(base, sizeof base, "%s", b);
    char *dot = strrchr(base, '.');
    if(dot && !strcmp(dot, ".o")) *dot = 0;

    char spath[1024], opath[1024];
    snprintf(spath, sizeof spath, "%s/%s.s", outdir, base);
    snprintf(opath, sizeof opath, "%s/%s.o", outdir, base);

    IRModule *m = ir_parse_file(input);
    if(!m){ fprintf(stderr, "parse failed: %s\n", input); return 1; }

    FILE *s = fopen(spath, "w");
    if(!s){ perror("fopen"); return 1; }
    target_emit_dispatch(m, s, t);
    fclose(s);

    char cmd[4096];
    snprintf(cmd, sizeof cmd, "%s %s -o %s", t->assembler, spath, opath);
    int rc = system(cmd);
    if(rc){ fprintf(stderr, "assembler failed\n"); return rc; }

    printf("%s -> %s\n", input, opath);
    return 0;
}
