#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int write_bytes(const char *dir, const char *name, const void *p, size_t n) {
    char path[1024]; FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name); f = fopen(path, "wb");
    if (!f) return 0;
    if (fwrite(p, 1, n, f) != n) { fclose(f); return 0; }
    return fclose(f) == 0;
}
static int nested(const char *dir,const char *name,const char *pre,const char *open,int d,const char *mid,const char *close,int closings,const char *post){
    char path[1024]; FILE *f; snprintf(path,sizeof(path),"%s/%s",dir,name); f=fopen(path,"wb"); if(!f)return 0;
    fputs(pre,f); for(int i=0;i<d;i++)fputs(open,f); fputs(mid,f); for(int i=0;i<closings;i++)fputs(close,f); fputs(post,f); return fclose(f)==0;
}
int main(int argc,char **argv){ const char *d=argc>1?argv[1]:"build/fuzz-corpus"; if(mkdir(d,0777)&&errno!=EEXIST){perror(d);return 1;}
#define W(n,s) do{if(!write_bytes(d,n,s,sizeof(s)-1))return 1;}while(0)
W("basic.bcl","proc f {} { return #1 }\n"); W("decimal.bcl","proc f {} { return [add #234.456 #0.544] }\n"); W("div.bcl","proc f {} { return [div #17 #5] }\n"); W("mod.bcl","proc f {} { return [mod #17 #5] }\n"); W("control.bcl","proc f {x} { if $x { return [pow $x #2] } { return #0 } }\n"); W("truncated.bcl","proc {\n"); W("unknown.bcl","proc f {} { return [unknown #1] }\n");
{ const unsigned char x[] = "proc f {} {return [mod #17 #517 #\0"; if (!write_bytes(d, "embedded-nul.bcl", x, sizeof(x) - 1u)) return 1; }
if(!nested(d,"deep-expression.bcl","proc f {} {return ","[neg ",64,"#1","]",64,"}\n"))return 1;
if(!nested(d,"deep-loops.bcl","proc f {} {","loop {",32,"break","}",32,";return #1}\n"))return 1;
if(!nested(d,"deep-if.bcl","proc f {} {","if #1 {",64,"return #1","}",64,"}\n"))return 1;
if(!nested(d,"truncated-deep.bcl","proc f {} {return ","[neg ",64,"#1","]",32,""))return 1;
return 0; }
