#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void die(sqlite3 *db,const char *what){fprintf(stderr,"%s: %s\n",what,db?sqlite3_errmsg(db):"error");exit(1);} 
static void exec_ok(sqlite3 *db,const char *sql){char *e=0;if(sqlite3_exec(db,sql,0,0,&e)!=SQLITE_OK){fprintf(stderr,"%s: %s\n",sql,e?e:sqlite3_errmsg(db));sqlite3_free(e);exit(1);}}
static void expect_error(sqlite3 *db,const char *sql){char *e=0;int rc=sqlite3_exec(db,sql,0,0,&e);sqlite3_free(e);if(rc==SQLITE_OK){fprintf(stderr,"expected failure: %s\n",sql);exit(1);}}
int main(int argc,char **argv){sqlite3 *db=0;char *e=0;if(argc!=2){fprintf(stderr,"usage: %s extension.so\n",argv[0]);return 2;}if(sqlite3_open(":memory:",&db)!=SQLITE_OK)die(db,"open");sqlite3_enable_load_extension(db,1);if(sqlite3_load_extension(db,argv[1],0,&e)!=SQLITE_OK){fprintf(stderr,"load: %s\n",e?e:sqlite3_errmsg(db));sqlite3_free(e);return 1;}
exec_ok(db,"select sc_bclite_prefix()");
exec_ok(db,"select sc_bclite_load('proc twice {x} { return [mul $x #2] } proc add_step {x} { state_set #0 [add [state_get #0] $x]; return [state_get #0] } proc sub_inverse {x} { state_set #0 [sub [state_get #0] $x]; return [state_get #0] } proc state_value {} { return [state_get #0] }')");
exec_ok(db,"select sc_bclite_register_all(); select twice(21),twice('#1.25'); select sc_bclite_register_scalar('double_exact','twice',1); select double_exact('#9.5');");
exec_ok(db,"select sc_bclite_register_aggregate('bc_sum',1,1,'add_step','state_value'); select sc_bclite_register_window('bc_wsum',1,1,'add_step','sub_inverse','state_value','state_value'); create table t(x); insert into t values(1),(2),(3),(4); select bc_sum(x) from t; select bc_wsum(x) over(order by x rows between 1 preceding and current row) from t;");
exec_ok(db,"select sc_bclite_prefix('@'); select twice('@1.25');");expect_error(db,"select twice('bad')");expect_error(db,"select sc_bclite_register_scalar('bad','s',1)");sqlite3_close(db);puts("loadable extension tests passed");return 0;}
