#ifndef SC_SQLITE_H
#define SC_SQLITE_H

#include <stddef.h>
#include <stdint.h>
#include <sqlite3.h>

#include "compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) && !defined(SC_SQLITE_STATIC)
# define SC_SQLITE_API __attribute__((visibility("default")))
#else
# define SC_SQLITE_API
#endif

#define SC_SQLITE_ABI_VERSION 4u
#define SC_SQLITE_DEFAULT_PREFIX '#'
#define SC_SQLITE_MAX_ERROR 512u
#if SC_ENABLE_PERSISTENT_STATE
# define SC_SQLITE_MAX_STATE_SLOTS SC_PERSISTENT_STATE_SLOTS
#else
# define SC_SQLITE_MAX_STATE_SLOTS 0u
#endif

typedef struct ScSqlite ScSqlite;

typedef enum ScSqliteResult {
    SC_SQLITE_OK = 0,
    SC_SQLITE_ERROR = 1,
    SC_SQLITE_NOMEM = 7,
    SC_SQLITE_MISUSE = 21,
    SC_SQLITE_RANGE = 25,
    SC_SQLITE_COMPILE_ERROR = 1000,
    SC_SQLITE_RUNTIME_ERROR,
    SC_SQLITE_INVALID_PREFIX,
    SC_SQLITE_DUPLICATE_FUNCTION,
    SC_SQLITE_UNSUPPORTED_VALUE,
    SC_SQLITE_NOT_FOUND,
    SC_SQLITE_BAD_AGGREGATE_SPEC,
    SC_SQLITE_BAD_LIBRARY_FLAGS
} ScSqliteResult;

typedef enum ScSqliteInputPolicy {
    SC_SQLITE_INPUT_STRICT = 0,
    SC_SQLITE_INPUT_SQLITE_NUMERIC
} ScSqliteInputPolicy;

typedef enum ScSqliteNullPolicy {
    SC_SQLITE_NULL_ERROR = 0,
    SC_SQLITE_NULL_PROPAGATE,
    SC_SQLITE_NULL_AS_ZERO
} ScSqliteNullPolicy;

typedef enum ScSqliteOutputPolicy {
    SC_SQLITE_OUTPUT_INT64_OR_PREFIXED_TEXT = 0,
    SC_SQLITE_OUTPUT_ALWAYS_PREFIXED_TEXT
} ScSqliteOutputPolicy;

typedef enum ScSqliteNamePolicy {
    SC_SQLITE_NAMES_EXACT = 0,
    SC_SQLITE_NAMES_WITH_PREFIX
} ScSqliteNamePolicy;

/*
 * Embedded library source is compiled into the VM when enabled, but library
 * procedures are never exposed as SQLite functions. They are available only
 * to user procedures compiled into the same context.
 */
typedef enum ScSqliteLibraryLevel {
    SC_SQLITE_LIBRARY_NONE = 0,
    SC_SQLITE_LIBRARY_1 = 1,
    SC_SQLITE_LIBRARY_1_2 = 2,
    SC_SQLITE_LIBRARY_1_2_3 = 3
} ScSqliteLibraryLevel;

/*
 * Automatic registration always considers user procedures only. Builtins and
 * embedded-library procedures are never registered as SQLite functions.
 */
typedef enum ScSqliteRegisterFlags {
    SC_SQLITE_REGISTER_NONE = 0,
    SC_SQLITE_REGISTER_REPLACE_EXISTING = 1u << 0
} ScSqliteRegisterFlags;

typedef enum ScSqliteFunctionFlags {
    SC_SQLITE_FUNC_DETERMINISTIC = 1u << 0,
    SC_SQLITE_FUNC_DIRECTONLY = 1u << 1,
    SC_SQLITE_FUNC_INNOCUOUS = 1u << 2
} ScSqliteFunctionFlags;

typedef struct ScSqliteError {
    ScSqliteResult code;
    int sqlite_code;
    ScCompileResult compile_code;
    ScVmResult vm_code;
    size_t source_pos;
    uint32_t line;
    uint32_t column;
    char message[SC_SQLITE_MAX_ERROR];
} ScSqliteError;

typedef struct ScSqliteConfig {
    uint32_t struct_size;
    uint32_t abi_version;

    char numeric_text_prefix;
    ScSqliteInputPolicy input_policy;
    ScSqliteNullPolicy null_policy;
    ScSqliteOutputPolicy output_policy;
    ScSqliteNamePolicy name_policy;

    const char *sql_name_prefix;

    ScSqliteLibraryLevel library_level;

    /* Bitwise combination of ScSqliteRegisterFlags. */
    uint32_t register_flags;

    uint32_t default_function_flags;
    ScSecurityLimits limits;
} ScSqliteConfig;

typedef struct ScSqliteSource {
    uint32_t struct_size;
    const char *name;
    const char *text;
    size_t text_len;
    uint32_t function_flags;
} ScSqliteSource;

typedef struct ScSqliteScalarSpec {
    uint32_t struct_size;
    const char *sql_name;
    const char *proc_name;
    int argc;
    uint32_t function_flags;
} ScSqliteScalarSpec;

/*
 * Aggregate and window procedures use adapter-managed numeric state slots.
 * Each aggregate instance has state_count slots, numbered from zero.
 * Declared slots are initialized to numeric zero before the first callback.
 *
 * state_get(index) -> value
 * state_set(index, value) -> value
 *
 * State access is valid only while an aggregate or window callback is active.
 * The adapter must reject out-of-range access and access outside an active
 * callback. State mutations made by step_proc or inverse_proc are committed
 * only if the procedure completes successfully.
 *
 * step_proc(sql_arg_1, ..., sql_arg_N)
 *     May read and update state slots.
 *
 * inverse_proc(sql_arg_1, ..., sql_arg_N)
 *     May read and update state slots. Required for a true window function.
 *
 * value_proc()
 *     Reads state and returns one result. It may be omitted, in which case
 *     final_proc is also used for xValue. State is read-only during xValue.
 *
 * final_proc()
 *     Reads state and returns the final result.
 */
typedef enum ScSqliteAggregateKind {
    SC_SQLITE_AGGREGATE = 0,
    SC_SQLITE_WINDOW = 1
} ScSqliteAggregateKind;

typedef struct ScSqliteAggregateSpec {
    uint32_t struct_size;
    const char *sql_name;
    int argc;

    /* Must be in the range 1..SC_SQLITE_MAX_STATE_SLOTS. */
    size_t state_count;

    const char *step_proc;
    const char *inverse_proc;
    const char *value_proc;
    const char *final_proc;

    uint32_t function_flags;
    ScSqliteAggregateKind kind;
} ScSqliteAggregateSpec;

SC_SQLITE_API void
sc_sqlite_config_init(ScSqliteConfig *config);

SC_SQLITE_API ScSqliteResult
sc_sqlite_create(
    sqlite3 *db,
    const ScSqliteConfig *config,
    ScSqlite **out_context
);

SC_SQLITE_API void
sc_sqlite_destroy(ScSqlite *context);

SC_SQLITE_API sqlite3 *
sc_sqlite_db(ScSqlite *context);

SC_SQLITE_API const ScSqliteError *
sc_sqlite_error(const ScSqlite *context);

SC_SQLITE_API void
sc_sqlite_clear_error(ScSqlite *context);

SC_SQLITE_API ScSqliteResult
sc_sqlite_set_numeric_text_prefix(
    ScSqlite *context,
    char prefix
);

SC_SQLITE_API char
sc_sqlite_get_numeric_text_prefix(const ScSqlite *context);

/*
 * Loads user source. Procedures declared by this source are eligible for
 * SQLite registration. Builtins and embedded-library procedures are not.
 */
SC_SQLITE_API ScSqliteResult
sc_sqlite_load_source(
    ScSqlite *context,
    const ScSqliteSource *source
);

SC_SQLITE_API ScSqliteResult
sc_sqlite_load_sources(
    ScSqlite *context,
    const ScSqliteSource *sources,
    size_t source_count
);

/*
 * Loads the selected cumulative embedded library level. Normally this is performed by
 * sc_sqlite_create() using config.library_level. Repeated requests for an
 * already-loaded library must succeed without recompiling it.
 */
SC_SQLITE_API ScSqliteResult
sc_sqlite_load_embedded_libraries(
    ScSqlite *context,
    ScSqliteLibraryLevel library_level
);

SC_SQLITE_API uint32_t
sc_sqlite_loaded_library_level(const ScSqlite *context);

/* Registers all eligible public user procedures and no other symbols. */
SC_SQLITE_API ScSqliteResult
sc_sqlite_register_all(ScSqlite *context);

/*
 * Registers one user procedure under an explicit SQLite name. proc_name must
 * resolve to a user-source procedure; builtins and library procedures are
 * rejected.
 */
SC_SQLITE_API ScSqliteResult
sc_sqlite_register_scalar(
    ScSqlite *context,
    const ScSqliteScalarSpec *spec
);

/*
 * Registers an aggregate or window function whose callback procedures must
 * all resolve to user-source procedures. Library procedures may be called by
 * those procedures but cannot be registered directly.
 */
SC_SQLITE_API ScSqliteResult
sc_sqlite_register_aggregate(
    ScSqlite *context,
    const ScSqliteAggregateSpec *spec
);

SC_SQLITE_API ScSqliteResult
sc_sqlite_unregister(
    ScSqlite *context,
    const char *sql_name,
    int argc
);

SC_SQLITE_API ScSqliteResult
sc_sqlite_value_import(
    ScSqlite *context,
    sqlite3_value *input,
    ScValue *out_value
);

SC_SQLITE_API ScSqliteResult
sc_sqlite_result_export(
    ScSqlite *context,
    sqlite3_context *sql_context,
    ScValue value
);

/*
 * Calls a user-source procedure. Builtin and embedded-library procedure names
 * are rejected as direct call targets.
 */
SC_SQLITE_API ScSqliteResult
sc_sqlite_call(
    ScSqlite *context,
    const char *proc_name,
    int argc,
    sqlite3_value **argv,
    sqlite3_context *sql_context
);

SC_SQLITE_API int
sc_sqlite_result_to_sqlite_code(ScSqliteResult result);

SC_SQLITE_API const char *
sc_sqlite_result_name(ScSqliteResult result);

SC_SQLITE_API int
sqlite3_scbclite_init(
    sqlite3 *db,
    char **error_message,
    const sqlite3_api_routines *api
);

#ifdef __cplusplus
}
#endif

#endif
