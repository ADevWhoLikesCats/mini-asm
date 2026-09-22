#include "ir.h"
#include "ir_parse.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#define PARAM_BASE 1000
#define MAXCALLARGS 6

static IRModule *M;
static IRFunc *F;
static IRBlock *B;
static IRType *I32;
static IRType *Tvoid;
static int gnextv;

static char *dupn(const char *s, size_t n){char*p=malloc(n+1);memcpy(p,s,n);p[n]=0;return p;}

static int peekc(FILE *f){
    int c;
    while((c=fgetc(f))!=EOF){
        if(c=='#'||c==';'){while((c=fgetc(f))!=EOF&&c!='\n');continue;}
        if(!isspace(c)){ungetc(c,f);return c;}
    }
    return EOF;
}

static int word(FILE *f, char *buf, int cap){
    int c=peekc(f);
    if(c==EOF)return 0;
    if(c=='('||c==')'||c=='{'||c=='}'||c==','||c=='='){
        fgetc(f);
        if(cap>1){buf[0]=(char)c;buf[1]=0;}
        return 1;
    }
    int n=0;
    while((c=fgetc(f))!=EOF){
        if(isspace(c)||c=='('||c==')'||c=='{'||c=='}'||c==','||c=='='){
            ungetc(c,f);break;
        }
        if(n<cap-1)buf[n++]=(char)c;
    }
    buf[n]=0;
    return n;
}

static IROpcode lookup(const char *n){
    static const struct {const char *n; IROpcode o;} T[]={
        {"add",OP_ADD},{"sub",OP_SUB},{"mul",OP_MUL},{"sdiv",OP_SDIV},{"udiv",OP_UDIV},
        {"srem",OP_SREM},{"urem",OP_UREM},{"and",OP_AND},{"or",OP_OR},{"xor",OP_XOR},
        {"shl",OP_SHL},{"lshr",OP_LSHR},{"ashr",OP_ASHR},
        {"fadd",OP_FADD},{"fsub",OP_FSUB},{"fmul",OP_FMUL},{"fdiv",OP_FDIV},{"frem",OP_FREM},
        {"neg",OP_NEG},{"not",OP_NOT},{"icmp",OP_ICMP},{"fcmp",OP_FCMP},
        {"ret",OP_RET},{"br",OP_BR},{"cbr",OP_CBR},{"call",OP_CALL},
        {"load",OP_LOAD},{"store",OP_STORE},{"alloca",OP_ALLOCA},{"phi",OP_PHI},{"select",OP_SELECT},
        {"zext",OP_ZEXT},{"sext",OP_SEXT},{"trunc",OP_TRUNC},
    };
    for(size_t i=0;i<sizeof T/sizeof *T;i++)if(!strcmp(n,T[i].n))return T[i].o;
    return (IROpcode)-1;
}

static const char *icmp_names[]={"eq","ne","slt","sle","sgt","sge","ult","ule","ugt","uge"};
static const char *fcmp_names[]={"oeq","one","olt","ole","ogt","oge","ord","uno"};

static int lookup_pred(const char *n, int is_fcmp){
    if(is_fcmp){for(int i=0;i<8;i++)if(!strcmp(n,fcmp_names[i]))return i;}
    else{for(int i=0;i<10;i++)if(!strcmp(n,icmp_names[i]))return i;}
    return 0;
}

static IRType *lookup_ty(const char *n){
    if(!strcmp(n,"void"))return Tvoid;
    IRTypeKind k;
    if(!strcmp(n,"i1"))k=TY_I1;
    else if(!strcmp(n,"i8"))k=TY_I8;
    else if(!strcmp(n,"i16"))k=TY_I16;
    else if(!strcmp(n,"i32"))k=TY_I32;
    else if(!strcmp(n,"i64"))k=TY_I64;
    else if(!strcmp(n,"f32"))k=TY_F32;
    else if(!strcmp(n,"f64"))k=TY_F64;
    else if(!strcmp(n,"ptr"))k=TY_PTR;
    else k=TY_I32;
    IRType *t=calloc(1,sizeof *t);
    t->kind=k;
    return t;
}

static int parse_operand(FILE *f, IRInstr *in, int slot){
    char b[64];
    if(!word(f,b,64))return 0;
    if(!strcmp(b,",")){if(!word(f,b,64))return 0;}
    if(b[0]=='%'){
        in->kinds[slot]=ARG_VREG;
        if(!strncmp(b+1,"arg",3))in->args[slot]=PARAM_BASE+atoi(b+4);
        else in->args[slot]=atoi(b+1);
        return 1;
    }
    if(b[0]=='-'||(b[0]>='0'&&b[0]<='9')){
        in->kinds[slot]=ARG_IMM;
        in->args[slot]=(int)strtoll(b,NULL,0);
        return 1;
    }
    in->kinds[slot]=ARG_IMM;
    in->args[slot]=0;
    return 1;
}

static int emit_op(IROpcode op, IRType *ty, IRInstr *tmp, int dst, int nargs){
    int a[6]; for(int i=0;i<6;i++)a[i]=tmp?tmp->args[i]:-1;
    int ix=ir_emit(B, op, ty, a[0],a[1],a[2],a[3],nargs);
    IRInstr *e=&B->instrs[ix];
    for(int i=4;i<6;i++){e->args[i]=a[i];}
    e->dst=dst;
    if(tmp)for(int i=0;i<6;i++)e->kinds[i]=tmp->kinds[i];
    return ix;
}

static IRModule *parse(FILE *f){
    M=ir_module_new();
    Tvoid=calloc(1,sizeof *Tvoid);Tvoid->kind=TY_VOID;
    I32=calloc(1,sizeof *I32);I32->kind=TY_I32;
    gnextv=0;
    char w[128];

    while(word(f,w,128)){
        if(!strcmp(w,"{"))continue;
        if(!strcmp(w,"}")){F=NULL;B=NULL;continue;}
        if(!strcmp(w,"(")||!strcmp(w,")")||!strcmp(w,",")||!strcmp(w,"="))continue;

        if(!strcmp(w,"func")){
            char name[64],ret[32];
            word(f,name,64);
            word(f,ret,32);
            F=ir_func_new(M,dupn(name,strlen(name)),lookup_ty(ret));
            gnextv=0;
            if(peekc(f)=='('){
                fgetc(f);
                while(1){
                    int c=peekc(f);
                    if(c==')'||c==EOF){if(c==')')fgetc(f);break;}
                    if(c==','){fgetc(f);continue;}
                    char pt[32];
                    word(f,pt,32);
                    F->nparams++;
                }
            }
            while(1){char t[64];if(!word(f,t,64))break;if(!strcmp(t,"{"))break;}
            continue;
        }
        if(!strcmp(w,"block")){
            char name[64];
            word(f,name,64);
            B=ir_block_new(F,dupn(name,strlen(name)));
            continue;
        }

        int dst=-1;
        char w2[128];
        if(w[0]=='%'){
            dst=atoi(w+1);
            if(dst>=gnextv)gnextv=dst+1;
            word(f,w2,128);
            if(!strcmp(w2,"=")){word(f,w2,128);}
            strcpy(w,w2);
        }

        IROpcode op=lookup(w);
        if((int)op<0)continue;

        IRInstr tmp;
        memset(&tmp,0,sizeof tmp);
        tmp.op=op;
        tmp.dst=dst;

        if(op==OP_RET){
            char t[32];
            if(!word(f,t,32)){emit_op(OP_RET,Tvoid,NULL,-1,0);continue;}
            if(!strcmp(t,"void")){emit_op(OP_RET,Tvoid,NULL,-1,0);continue;}
            tmp.type=lookup_ty(t);
            parse_operand(f,&tmp,0);
            emit_op(OP_RET,tmp.type,&tmp,-1,1);
            continue;
        }

        if(op==OP_CALL){
            char t[32],cal[64];
            word(f,t,32);
            word(f,cal,64);
            IRInstr ctmp;
            memset(&ctmp,0,sizeof ctmp);
            ctmp.type=lookup_ty(t);
            int p=peekc(f);
            int nargs=0;
            if(p=='('){
                fgetc(f);
                while(1){
                    int q=peekc(f);
                    if(q==')'||q==EOF){if(q==')')fgetc(f);break;}
                    if(q==','){fgetc(f);continue;}
                    if(nargs>=MAXCALLARGS){char junk[64];word(f,junk,64);nargs++;continue;}
                    parse_operand(f,&ctmp,nargs);
                    nargs++;
                }
            }
            if(nargs>MAXCALLARGS)nargs=MAXCALLARGS;
            int ix=ir_emit(B,OP_CALL,ctmp.type,
                           ctmp.args[0],ctmp.args[1],ctmp.args[2],ctmp.args[3],
                           nargs>3?nargs:0);
            for(int i=0;i<MAXCALLARGS;i++)B->instrs[ix].args[i]=ctmp.args[i];
            for(int i=0;i<MAXCALLARGS;i++)B->instrs[ix].kinds[i]=ctmp.kinds[i];
            B->instrs[ix].callee=dupn(cal,strlen(cal));
            B->instrs[ix].dst=dst;
            B->instrs[ix].nargs=nargs;
            continue;
        }

        if(op==OP_ICMP||op==OP_FCMP){
            char t[32],pr[16];
            word(f,t,32);
            word(f,pr,16);
            tmp.type=lookup_ty(t);
            tmp.pred=lookup_pred(pr,op==OP_FCMP);
            parse_operand(f,&tmp,0);
            parse_operand(f,&tmp,1);
            int ix=emit_op(op,tmp.type,&tmp,dst,2);
            B->instrs[ix].pred=tmp.pred;
            continue;
        }

        if(op==OP_BR){
            char l[64];
            word(f,l,64);
            int ix=emit_op(OP_BR,Tvoid,NULL,-1,0);
            B->instrs[ix].label=dupn(l,strlen(l));
            continue;
        }

        if(op==OP_CBR){
            char l1[64],l2[64];
            parse_operand(f,&tmp,0);
            word(f,l1,64);
            word(f,l2,64);
            int ix=emit_op(OP_CBR,Tvoid,&tmp,-1,1);
            B->instrs[ix].label=dupn(l1,strlen(l1));
            B->instrs[ix].label2=dupn(l2,strlen(l2));
            continue;
        }

        if(op==OP_UNREACHABLE){emit_op(OP_UNREACHABLE,Tvoid,NULL,-1,0);continue;}

        if(op==OP_PHI){
            char t[32];
            word(f,t,32);
            tmp.type=lookup_ty(t);
            parse_operand(f,&tmp,0);
            char l1[64];
            word(f,l1,64);
            parse_operand(f,&tmp,1);
            char l2[64];
            word(f,l2,64);
            int ix=emit_op(OP_PHI,tmp.type,&tmp,dst,2);
            B->instrs[ix].label=dupn(l1,strlen(l1));
            B->instrs[ix].label2=dupn(l2,strlen(l2));
            continue;
        }

        if(op==OP_ALLOCA){
            char t[32];
            word(f,t,32);
            tmp.type=lookup_ty(t);
            /* peek: if next token is a digit, it's a byte count */
            int pk=peekc(f);
            if(pk>='0'&&pk<='9'){
                int64_t n=0;
                char nb[32]; word(f,nb,32); n=strtoll(nb,NULL,0);
                tmp.args[0]=(int)n; tmp.kinds[0]=ARG_IMM;
                emit_op(OP_ALLOCA,tmp.type,&tmp,dst,1);
                continue;
            }
            tmp.args[0]=4; tmp.kinds[0]=ARG_IMM;
            if(tmp.type->kind==TY_I8)tmp.args[0]=1;
            else if(tmp.type->kind==TY_I16)tmp.args[0]=2;
            else if(tmp.type->kind==TY_I32)tmp.args[0]=4;
            else if(tmp.type->kind==TY_I64||tmp.type->kind==TY_PTR)tmp.args[0]=8;
            emit_op(OP_ALLOCA,tmp.type,&tmp,dst,1);
            continue;
        }

        if(op==OP_LOAD){
            char t[32];
            word(f,t,32);
            tmp.type=lookup_ty(t);
            parse_operand(f,&tmp,0);
            emit_op(OP_LOAD,tmp.type,&tmp,dst,1);
            continue;
        }

        if(op==OP_STORE){
            char t[32];
            word(f,t,32);
            tmp.type=lookup_ty(t);
            parse_operand(f,&tmp,0);
            parse_operand(f,&tmp,1);
            emit_op(OP_STORE,tmp.type,&tmp,-1,2);
            continue;
        }

        if(op==OP_NEG||op==OP_NOT||op==OP_FNEG){
            char t[32];
            word(f,t,32);
            tmp.type=lookup_ty(t);
            parse_operand(f,&tmp,0);
            emit_op(op,tmp.type,&tmp,dst,1);
            continue;
        }

        /* zext/sext/trunc: syntax  %d = zext i32 %src  (dst type = i32) */
        if(op==OP_ZEXT||op==OP_SEXT||op==OP_TRUNC){
            char dt[32],st[32];
            word(f,dt,32);
            word(f,st,32);
            tmp.type=lookup_ty(dt);
            parse_operand(f,&tmp,0);
            emit_op(op,tmp.type,&tmp,dst,1);
            continue;
        }

        {
            char t[32];
            word(f,t,32);
            tmp.type=lookup_ty(t);
            parse_operand(f,&tmp,0);
            parse_operand(f,&tmp,1);
            emit_op(op,tmp.type,&tmp,dst,2);
        }
    }
    return M;
}

IRModule *ir_parse_stream(FILE *f){return parse(f);}
IRModule *ir_parse_file(const char *p){
    FILE *f=fopen(p,"r");
    if(!f)return NULL;
    IRModule *m=parse(f);
    fclose(f);
    return m;
}
