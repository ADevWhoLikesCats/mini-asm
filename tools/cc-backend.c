#include "ir.h"
#include "ir_parse.h"
#include "backend.h"
#include "opt.h"
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

static void usage(const char *prog){
    fprintf(stderr,
        "usage: %s [--libc] [-O0|-O1] [-o <dir>] <target> <input.ir> <output.o>\n"
        "\n"
        "targets: x86, x86_64, arm, arm64, riscv\n"
        "flags:\n"
        "  --libc      link with libc (uses cc driver; enables printf, malloc)\n"
        "              without --libc, uses raw ld and needs no libc\n"
        "  -O0         disable optimization passes\n"
        "  -O1         enable constant folding + DCE (default)\n"
        "  -o <dir>    output directory for .s and .o (default: build)\n"
        "\n"
        "env:\n"
        "  CC_BACKEND_OUT=<dir>  same as -o\n"
        "  CC_DUMP_IR=1          print IR to stderr before emitting\n",
        prog);
}

int main(int argc, char **argv){
    const char *outdir = getenv("CC_BACKEND_OUT");
    if(!outdir) outdir = "build";

    int libc = 0;
    int argi = 1;

    /* Parse flags before the target name. */
    while(argi < argc){
        const char *a = argv[argi];
        if(!strcmp(a, "--libc")){
            libc = 1;
            argi++;
        } else if(!strcmp(a, "-O0")){
            ir_opt_level = 0;
            argi++;
        } else if(!strcmp(a, "-O1")){
            ir_opt_level = 1;
            argi++;
        } else if(!strcmp(a, "-o") && argi+1 < argc){
            outdir = argv[argi+1];
            argi += 2;
        } else if(!strcmp(a, "-h") || !strcmp(a, "--help")){
            usage(argv[0]);
            return 0;
        } else if(a[0] == '-' && a[1] != 0){
            fprintf(stderr, "unknown flag: %s\n", a);
            usage(argv[0]);
            return 1;
        } else {
            break;   /* first non-flag arg: the target */
        }
    }

    if(argc - argi < 3){
        usage(argv[0]);
        return 1;
    }

    const char *target_name = argv[argi];
    const char *input       = argv[argi+1];
    const char *output      = argv[argi+2];

    const TargetDesc *t = backend_lookup(target_name);
    if(!t){ fprintf(stderr, "unknown target: %s\n", target_name); return 1; }

    cc_libc_mode = libc;

    mkdir_p(outdir);

    /* Derive output basename (strip directory and .o suffix). */
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
    if(getenv("CC_DUMP_IR")) ir_print(m, stderr);

    FILE *s = fopen(spath, "w");
    if(!s){ perror("fopen"); return 1; }
    target_emit_dispatch(m, s, t);
    fclose(s);

    char cmd[4096];
    snprintf(cmd, sizeof cmd, "%s %s -o %s", t->assembler, spath, opath);
    int rc = system(cmd);
    if(rc){ fprintf(stderr, "assembler failed\n"); return rc; }

    printf("%s -> %s%s\n", input, opath, libc ? " (libc mode)" : "");
    return 0;
}
