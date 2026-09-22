#include "ir.h"
#include "ir_parse.h"
#include "backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const TargetDesc *backend_lookup(const char*);
extern int target_emit_dispatch(IRModule*,FILE*,const TargetDesc*);
extern void ir_print(IRModule*,FILE*);
int main(int argc,char**argv){
  if(argc<4){fprintf(stderr,"usage: %s <target> <in.ir> <out.o>\n",argv[0]);return 1;}
  const TargetDesc*t=backend_lookup(argv[1]);
  if(!t){fprintf(stderr,"unknown target: %s\n",argv[1]);return 1;}
  IRModule*m=ir_parse_file(argv[2]);
  if(!m){fprintf(stderr,"parse failed\n");return 1;}
  ir_print(m,stderr);
  char spath[1024];snprintf(spath,sizeof spath,"%s.s",argv[3]);
  FILE*s=fopen(spath,"w");if(!s){perror("fopen");return 1;}
  target_emit_dispatch(m,s,t);
  fclose(s);
  char cmd[2048];
  snprintf(cmd,sizeof cmd,"%s %s -o %s",t->assembler,spath,argv[3]);
  return system(cmd);
}
