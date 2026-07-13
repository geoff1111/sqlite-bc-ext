#include "sc_sqlite.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void exec_ok(sqlite3 *db, const char *sql)
{
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "%s: %s\n", sql, error ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
    }
    assert(rc == SQLITE_OK);
}

static void expect_text(sqlite3 *db, const char *sql, const char *expected)
{
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), expected) == 0);
    assert(sqlite3_finalize(stmt) == SQLITE_OK);
}

static void expect_int(sqlite3 *db, const char *sql, sqlite3_int64 expected)
{
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int64(stmt, 0) == expected);
    assert(sqlite3_finalize(stmt) == SQLITE_OK);
}

int main(void)
{
    sqlite3 *db = NULL;
    ScSqlite *context = NULL;
    ScSqliteConfig config;
    ScSqliteSource source;
    ScSqliteAggregateSpec aggregate;
    const char code[] =
        "proc twice {x} {return [mul $x #2]}\n"
        "proc add_step {x} {state_set #0 [add [state_get #0] $x];return [state_get #0]}\n"
        "proc sub_inverse {x} {state_set #0 [sub [state_get #0] $x];return [state_get #0]}\n"
        "proc state_value {} {return [state_get #0]}\n";

    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    sc_sqlite_config_init(&config);
    assert(sc_sqlite_create(db, &config, &context) == SC_SQLITE_OK);
    memset(&source, 0, sizeof(source));
    source.struct_size = sizeof(source);
    source.name = "test";
    source.text = code;
    source.text_len = sizeof(code) - 1u;
    assert(sc_sqlite_load_source(context, &source) == SC_SQLITE_OK);
    assert(sc_sqlite_register_all(context) == SC_SQLITE_OK);
    expect_int(db, "SELECT twice(21)", 42);
    expect_text(db, "SELECT twice('#1.25')", "#2.50");

    memset(&aggregate, 0, sizeof(aggregate));
    aggregate.struct_size = sizeof(aggregate);
    aggregate.sql_name = "bcsum";
    aggregate.argc = 1;
    aggregate.state_count = 1;
    aggregate.step_proc = "add_step";
    aggregate.final_proc = "state_value";
    aggregate.kind = SC_SQLITE_AGGREGATE;
    assert(sc_sqlite_register_aggregate(context, &aggregate) == SC_SQLITE_OK);
    exec_ok(db, "CREATE TABLE t(x INTEGER)");
    exec_ok(db, "INSERT INTO t VALUES(1),(2),(3),(4)");
    expect_int(db, "SELECT bcsum(x) FROM t", 10);

    aggregate.sql_name = "bcwinsum";
    aggregate.inverse_proc = "sub_inverse";
    aggregate.value_proc = "state_value";
    aggregate.kind = SC_SQLITE_WINDOW;
    assert(sc_sqlite_register_aggregate(context, &aggregate) == SC_SQLITE_OK);
    expect_int(db, "SELECT v FROM (SELECT bcwinsum(x) OVER (ORDER BY x ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS v FROM t) ORDER BY v DESC LIMIT 1", 7);

    assert(sc_sqlite_unregister(context, "twice", 1) == SC_SQLITE_OK);
    sc_sqlite_destroy(context);
    assert(sqlite3_close(db) == SQLITE_OK);
    puts("sqlite adapter tests passed");
    return 0;
}
