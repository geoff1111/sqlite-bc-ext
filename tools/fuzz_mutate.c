#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
typedef struct { unsigned char *p; size_t n; } Seed;
static uint64_t state=1; static uint32_t rnd(void){state=state*6364136223846793005ULL+1;return (uint32_t)(state>>32);} 
static int run_case(const char *runner,const unsigned char *p,size_t n){int fd[2];pid_t pid;int st;if(pipe(fd))return 2;pid=fork();if(pid<0)return 2;if(pid==0){dup2(fd[0],0);close(fd[0]);close(fd[1]);execl(runner,runner,(char*)0);_exit(127);}close(fd[0]);while(n){ssize_t w=write(fd[1],p,n);if(w<0){if(errno==EINTR)continue;break;}p+=w;n-=(size_t)w;}close(fd[1]);if(waitpid(pid,&st,0)<0)return 2;return WIFEXITED(st)?WEXITSTATUS(st):128+WTERMSIG(st);} 
int main(int argc,char **argv){const char *runner="build/fuzz_runner",*dir="build/fuzz-corpus";long runs=500;if(argc>1)runner=argv[1];if(argc>2)dir=argv[2];if(argc>3)runs=strtol(argv[3],0,10);if(argc>4)state=strtoull(argv[4],0,10);DIR *d=opendir(dir);struct dirent *de;Seed *s=0;size_t ns=0,cap=0;if(!d){perror(dir);return 2;}while((de=readdir(d))){char path[1024];FILE *f;long z;if(de->d_name[0]=='.')continue;snprintf(path,sizeof(path),"%s/%s",dir,de->d_name);f=fopen(path,"rb");if(!f)continue;fseek(f,0,SEEK_END);z=ftell(f);rewind(f);if(z<0||z>65536){fclose(f);continue;}if(ns==cap){cap=cap?cap*2:16;s=realloc(s,cap*sizeof(*s));if(!s)return 2;}s[ns].n=(size_t)z;s[ns].p=malloc((size_t)z+1);if(!s[ns].p)return 2;if(fread(s[ns].p,1,(size_t)z,f)!=(size_t)z)return 2;fclose(f);ns++;}closedir(d);if(!ns)return 2;
for(long k=0;k<runs;k++){Seed *q=&s[rnd()%ns];size_t n=q->n;unsigned char *b=malloc(65536);if(!b)return 2;memcpy(b,q->p,n);int edits=1+(int)(rnd()%8);while(edits--){int op=(int)(rnd()%4);if(op==0&&n)b[rnd()%n]^=(unsigned char)(1u<<(rnd()%8));else if(op==1&&n<65536){size_t p=rnd()%(n+1);memmove(b+p+1,b+p,n-p);b[p]=(unsigned char)rnd();n++;}else if(op==2&&n){size_t p=rnd()%n;memmove(b+p,b+p+1,n-p-1);n--;}else if(n){size_t a=rnd()%n,z=1+rnd()%64;if(a+z>n)z=n-a;for(size_t i=0;i<z/2;i++){unsigned char t=b[a+i];b[a+i]=b[a+z-1-i];b[a+z-1-i]=t;}}}int rc=run_case(runner,b,n);if(rc){FILE *f=fopen("fuzz-crash-input","wb");if(f){fwrite(b,1,n,f);fclose(f);}fprintf(stderr,"fuzz failure at iteration %ld rc=%d\n",k,rc);free(b);return rc;}free(b);}printf("fuzz passed: %ld deterministic mutations\n",runs);for(size_t i=0;i<ns;i++)free(s[i].p);free(s);return 0;}
