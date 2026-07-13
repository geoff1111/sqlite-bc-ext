#include "sc_sqlite.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

typedef enum RegistrationKind {
    REG_SCALAR = 1,
    REG_AGGREGATE,
    REG_WINDOW
} RegistrationKind;

typedef struct Registration Registration;
typedef struct AggregateContext AggregateContext;

struct Registration {
    struct ScSqlite *owner;
    char *sql_name;
    ScSymbolId step_symbol;
    ScSymbolId inverse_symbol;
    ScSymbolId value_symbol;
    ScSymbolId final_symbol;
    int argc;
    uint16_t state_count;
    uint32_t flags;
    RegistrationKind kind;
    Registration *next;
};

struct ScSqlite {
    sqlite3 *db;
    ScVm vm;
    ScSqliteConfig config;
    ScSqliteError error;
    ScSymbolId *user_symbols;
    size_t user_count;
    size_t user_capacity;
    Registration *registrations;
    int destroying;
};

struct AggregateContext {
#if SC_ENABLE_PERSISTENT_STATE
    ScPersistentState state;
#endif
    int initialized;
    int failed;
};

static void clear_error(ScSqlite *context)
{
    if (context == NULL) return;
    memset(&context->error, 0, sizeof(context->error));
    context->error.code = SC_SQLITE_OK;
    context->error.sqlite_code = SQLITE_OK;
    context->error.compile_code = SC_COMPILE_OK;
    context->error.vm_code = SC_VM_OK;
}

static ScSqliteResult set_error(ScSqlite *context, ScSqliteResult code,
                                int sqlite_code, const char *message)
{
    if (context != NULL) {
        clear_error(context);
        context->error.code = code;
        context->error.sqlite_code = sqlite_code;
        if (message != NULL) {
            snprintf(context->error.message, sizeof(context->error.message), "%s", message);
        }
    }
    return code;
}

static ScSqliteResult set_vm_error(ScSqlite *context, ScVmResult code)
{
    const ScVmError *error = sc_vm_error(&context->vm);
    clear_error(context);
    context->error.code = SC_SQLITE_RUNTIME_ERROR;
    context->error.sqlite_code = sc_sqlite_result_to_sqlite_code(context->error.code);
    context->error.vm_code = code;
    if (error != NULL) snprintf(context->error.message, sizeof(context->error.message), "%s", error->message);
    if (!sc_vm_error_is_fatal(code)) (void) sc_vm_reset_after_error(&context->vm);
    return context->error.code;
}

static ScSqliteResult set_compile_error(ScSqlite *context, ScCompileResult code,
                                        const ScCompileError *error)
{
    clear_error(context);
    context->error.code = SC_SQLITE_COMPILE_ERROR;
    context->error.sqlite_code = SQLITE_ERROR;
    context->error.compile_code = code;
    if (error != NULL) {
        context->error.source_pos = error->source_pos;
        context->error.line = error->line;
        context->error.column = error->column;
        snprintf(context->error.message, sizeof(context->error.message), "%s", error->message);
    }
    return context->error.code;
}

static int valid_struct(uint32_t supplied, size_t required)
{
    return supplied >= required && supplied <= UINT32_MAX;
}

static char *dup_string(const char *text)
{
    size_t n;
    char *copy;
    if (text == NULL) return NULL;
    n = strlen(text);
    copy = malloc(n + 1u);
    if (copy != NULL) memcpy(copy, text, n + 1u);
    return copy;
}

static int sqlite_flags(uint32_t flags)
{
    int result = SQLITE_UTF8;
    if ((flags & SC_SQLITE_FUNC_DETERMINISTIC) != 0u) result |= SQLITE_DETERMINISTIC;
#ifdef SQLITE_DIRECTONLY
    if ((flags & SC_SQLITE_FUNC_DIRECTONLY) != 0u) result |= SQLITE_DIRECTONLY;
#endif
#ifdef SQLITE_INNOCUOUS
    if ((flags & SC_SQLITE_FUNC_INNOCUOUS) != 0u) result |= SQLITE_INNOCUOUS;
#endif
    return result;
}

static int flags_valid(uint32_t flags)
{
    const uint32_t known = SC_SQLITE_FUNC_DETERMINISTIC |
                           SC_SQLITE_FUNC_DIRECTONLY |
                           SC_SQLITE_FUNC_INNOCUOUS;
    return (flags & ~known) == 0u;
}

static int is_private_name(const char *name)
{
    return name != NULL && isdigit((unsigned char)name[0]);
}

static int user_symbol_index(const ScSqlite *context, ScSymbolId symbol)
{
    size_t i;
    for (i = 0; i < context->user_count; ++i) {
        if (context->user_symbols[i] == symbol) return (int)i;
    }
    return -1;
}

static ScSqliteResult remember_user_symbols(ScSqlite *context, uint32_t first)
{
    uint32_t i;
    for (i = first; i < context->vm.symbols.count; ++i) {
        const ScSymbol *symbol = &context->vm.symbols.symbols[i];
        ScSymbolId *new_items;
        size_t new_capacity;
        if (symbol->kind != SC_SYM_PROC || symbol->decl_origin != SC_DECL_SOURCE) continue;
        if (user_symbol_index(context, i) >= 0) continue;
        if (context->user_count == context->user_capacity) {
            new_capacity = context->user_capacity == 0u ? 16u : context->user_capacity * 2u;
            new_items = realloc(context->user_symbols, new_capacity * sizeof(*new_items));
            if (new_items == NULL) return set_error(context, SC_SQLITE_NOMEM, SQLITE_NOMEM, "out of memory");
            context->user_symbols = new_items;
            context->user_capacity = new_capacity;
        }
        context->user_symbols[context->user_count++] = i;
    }
    return SC_SQLITE_OK;
}

static ScSqliteResult resolve_user_proc(ScSqlite *context, const char *name,
                                        int argc, ScSymbolId *out)
{
    ScSymbolId id;
    ScVmResult rc;
    const ScSymbol *symbol;
    if (name == NULL || out == NULL || argc < 0 || argc > (int)UINT16_MAX)
        return set_error(context, SC_SQLITE_MISUSE, SQLITE_MISUSE, "invalid procedure specification");
    rc = sc_symbol_lookup(&context->vm.symbols, sc_slice_make(name, strlen(name)), &id);
    if (rc != SC_VM_OK || id >= context->vm.symbols.count)
        return set_error(context, SC_SQLITE_NOT_FOUND, SQLITE_NOTFOUND, "user procedure not found");
    symbol = &context->vm.symbols.symbols[id];
    if (symbol->kind != SC_SYM_PROC || symbol->argc != (uint16_t)argc ||
        user_symbol_index(context, id) < 0 || is_private_name(symbol->name))
        return set_error(context, SC_SQLITE_NOT_FOUND, SQLITE_NOTFOUND,
                         "procedure is not an eligible public user procedure");
    *out = id;
    return SC_SQLITE_OK;
}

void sc_sqlite_config_init(ScSqliteConfig *config)
{
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    config->struct_size = (uint32_t)sizeof(*config);
    config->abi_version = SC_SQLITE_ABI_VERSION;
    config->numeric_text_prefix = SC_SQLITE_DEFAULT_PREFIX;
    config->input_policy = SC_SQLITE_INPUT_STRICT;
    config->null_policy = SC_SQLITE_NULL_ERROR;
    config->output_policy = SC_SQLITE_OUTPUT_INT64_OR_PREFIXED_TEXT;
    config->name_policy = SC_SQLITE_NAMES_EXACT;
    config->library_level = SC_SQLITE_LIBRARY_NONE;
    sc_security_limits_default(&config->limits);
}

ScSqliteResult sc_sqlite_create(sqlite3 *db, const ScSqliteConfig *config,
                                ScSqlite **out_context)
{
    ScSqliteConfig defaults;
    ScSqlite *context;
    ScVmResult vrc;
    ScCompileError compile_error;
    if (db == NULL || out_context == NULL) return SC_SQLITE_MISUSE;
    *out_context = NULL;
    sc_sqlite_config_init(&defaults);
    if (config == NULL) config = &defaults;
    if (!valid_struct(config->struct_size, offsetof(ScSqliteConfig, limits) + sizeof(config->limits)) ||
        config->abi_version != SC_SQLITE_ABI_VERSION) return SC_SQLITE_MISUSE;
    if (!sc_numeric_prefix_valid(config->numeric_text_prefix)) return SC_SQLITE_INVALID_PREFIX;
    if (config->library_level < SC_SQLITE_LIBRARY_NONE ||
        config->library_level > SC_SQLITE_LIBRARY_1_2_3) return SC_SQLITE_BAD_LIBRARY_FLAGS;
    context = calloc(1u, sizeof(*context));
    if (context == NULL) return SC_SQLITE_NOMEM;
    context->db = db;
    context->config = *config;
    clear_error(context);
    vrc = sc_vm_init(&context->vm, &config->limits);
    if (vrc != SC_VM_OK) { free(context); return SC_SQLITE_NOMEM; }
    vrc = sc_vm_set_numeric_text_prefix(&context->vm, config->numeric_text_prefix);
    if (vrc != SC_VM_OK) { sc_vm_destroy(&context->vm); free(context); return SC_SQLITE_INVALID_PREFIX; }
    vrc = sc_vm_register_core_builtins(&context->vm);
    if (vrc != SC_VM_OK) { sc_vm_destroy(&context->vm); free(context); return SC_SQLITE_ERROR; }
    memset(&compile_error, 0, sizeof(compile_error));
    if (sc_vm_load_libraries(&context->vm, (ScLibraryLevel)config->library_level, &compile_error) != SC_COMPILE_OK) {
        sc_vm_destroy(&context->vm); free(context); return SC_SQLITE_COMPILE_ERROR;
    }
    *out_context = context;
    return SC_SQLITE_OK;
}

static void registration_destructor(void *pointer)
{
    Registration *registration = pointer;
    ScSqlite *owner;
    Registration **link;
    if (registration == NULL) return;
    owner = registration->owner;
    if (owner != NULL) {
        link = &owner->registrations;
        while (*link != NULL && *link != registration) link = &(*link)->next;
        if (*link == registration) *link = registration->next;
    }
    free(registration->sql_name);
    free(registration);
}

void sc_sqlite_destroy(ScSqlite *context)
{
    Registration *registration;
    if (context == NULL) return;
    context->destroying = 1;
    while ((registration = context->registrations) != NULL) {
        int rc;
        if (registration->kind == REG_SCALAR)
            rc = sqlite3_create_function_v2(context->db, registration->sql_name,
                    registration->argc, SQLITE_UTF8, NULL, NULL, NULL, NULL, NULL);
        else if (registration->kind == REG_WINDOW)
            rc = sqlite3_create_window_function(context->db, registration->sql_name,
                    registration->argc, SQLITE_UTF8, NULL, NULL, NULL, NULL, NULL, NULL);
        else
            rc = sqlite3_create_function_v2(context->db, registration->sql_name,
                    registration->argc, SQLITE_UTF8, NULL, NULL, NULL, NULL, NULL);
        if (rc != SQLITE_OK && context->registrations == registration) {
            context->destroying = 0;
            return;
        }
    }
    free(context->user_symbols);
    sc_vm_destroy(&context->vm);
    free(context);
}

sqlite3 *sc_sqlite_db(ScSqlite *context) { return context == NULL ? NULL : context->db; }
const ScSqliteError *sc_sqlite_error(const ScSqlite *context) { return context == NULL ? NULL : &context->error; }
void sc_sqlite_clear_error(ScSqlite *context) { clear_error(context); }

ScSqliteResult sc_sqlite_set_numeric_text_prefix(ScSqlite *context, char prefix)
{
    ScVmResult rc;
    if (context == NULL) return SC_SQLITE_MISUSE;
    rc = sc_vm_set_numeric_text_prefix(&context->vm, prefix);
    if (rc != SC_VM_OK) return set_error(context, SC_SQLITE_INVALID_PREFIX, SQLITE_MISMATCH, "invalid numeric text prefix");
    context->config.numeric_text_prefix = prefix;
    return SC_SQLITE_OK;
}

char sc_sqlite_get_numeric_text_prefix(const ScSqlite *context)
{
    return context == NULL ? '\0' : sc_vm_get_numeric_text_prefix(&context->vm);
}

ScSqliteResult sc_sqlite_load_source(ScSqlite *context, const ScSqliteSource *source)
{
    ScCompiler compiler;
    ScCompileResult rc;
    uint32_t count = 0u;
    uint32_t first;
    if (context == NULL || source == NULL ||
        !valid_struct(source->struct_size, sizeof(*source)) || source->text == NULL)
        return SC_SQLITE_MISUSE;
    first = context->vm.symbols.count;
    sc_compiler_init(&compiler, &context->vm, source->text, source->text_len);
    rc = sc_compiler_set_constant_prefix(&compiler, context->config.numeric_text_prefix);
    if (rc == SC_COMPILE_OK) rc = sc_compile_source(&compiler, &count);
    if (rc != SC_COMPILE_OK) {
        ScSqliteResult result = set_compile_error(context, rc, sc_compiler_error(&compiler));
        sc_compiler_destroy(&compiler);
        return result;
    }
    sc_compiler_destroy(&compiler);
    return remember_user_symbols(context, first);
}

ScSqliteResult sc_sqlite_load_sources(ScSqlite *context, const ScSqliteSource *sources, size_t source_count)
{
    size_t i;
    if (context == NULL || (source_count != 0u && sources == NULL)) return SC_SQLITE_MISUSE;
    for (i = 0; i < source_count; ++i) {
        ScSqliteResult rc = sc_sqlite_load_source(context, &sources[i]);
        if (rc != SC_SQLITE_OK) return rc;
    }
    return SC_SQLITE_OK;
}

ScSqliteResult sc_sqlite_load_embedded_libraries(ScSqlite *context, ScSqliteLibraryLevel level)
{
    ScCompileError error;
    ScCompileResult rc;
    if (context == NULL) return SC_SQLITE_MISUSE;
    if (level < SC_SQLITE_LIBRARY_NONE || level > SC_SQLITE_LIBRARY_1_2_3)
        return set_error(context, SC_SQLITE_BAD_LIBRARY_FLAGS, SQLITE_MISUSE, "invalid cumulative library level");
    memset(&error, 0, sizeof(error));
    rc = sc_vm_load_libraries(&context->vm, (ScLibraryLevel)level, &error);
    if (rc != SC_COMPILE_OK) return set_compile_error(context, rc, &error);
    return SC_SQLITE_OK;
}

uint32_t sc_sqlite_loaded_library_level(const ScSqlite *context)
{
    return context == NULL ? 0u : (uint32_t)sc_vm_loaded_library_level(&context->vm);
}

ScSqliteResult sc_sqlite_value_import(ScSqlite *context, sqlite3_value *input, ScValue *out_value)
{
    int type;
    ScVmResult rc;
    if (context == NULL || input == NULL || out_value == NULL) return SC_SQLITE_MISUSE;
    type = sqlite3_value_type(input);
    if (type == SQLITE_NULL) {
        if (context->config.null_policy == SC_SQLITE_NULL_AS_ZERO) {
            rc = sc_vm_retain_value(&context->vm, context->vm.zero_value);
            if (rc == SC_VM_OK) *out_value = context->vm.zero_value;
        } else return set_error(context, SC_SQLITE_UNSUPPORTED_VALUE, SQLITE_MISMATCH, "NULL is not a numeric value");
    } else if (type == SQLITE_INTEGER) {
        rc = sc_vm_import_int64(&context->vm, sqlite3_value_int64(input), out_value);
    } else if (type == SQLITE_TEXT ||
               (type == SQLITE_FLOAT && context->config.input_policy == SC_SQLITE_INPUT_SQLITE_NUMERIC)) {
        const unsigned char *text = sqlite3_value_text(input);
        int bytes = sqlite3_value_bytes(input);
        if (bytes < 0 || text == NULL) return set_error(context, SC_SQLITE_UNSUPPORTED_VALUE, SQLITE_MISMATCH, "invalid numeric text");
        if (bytes > 0 && ((const char *)text)[0] == context->config.numeric_text_prefix) {
            rc = sc_vm_import_numeric_text(&context->vm, (const char *)text, (size_t)bytes, out_value);
        } else if (context->config.input_policy == SC_SQLITE_INPUT_SQLITE_NUMERIC) {
            char *prefixed = malloc((size_t)bytes + 2u);
            if (prefixed == NULL) return set_error(context, SC_SQLITE_NOMEM, SQLITE_NOMEM, "out of memory");
            prefixed[0] = context->config.numeric_text_prefix;
            memcpy(prefixed + 1, text, (size_t)bytes);
            prefixed[(size_t)bytes + 1u] = '\0';
            rc = sc_vm_import_numeric_text(&context->vm, prefixed, (size_t)bytes + 1u, out_value);
            free(prefixed);
        } else {
            return set_error(context, SC_SQLITE_UNSUPPORTED_VALUE, SQLITE_MISMATCH, "strict input requires INTEGER or prefixed exact numeric TEXT");
        }
    } else {
        return set_error(context, SC_SQLITE_UNSUPPORTED_VALUE, SQLITE_MISMATCH,
                         "unsupported SQLite value type");
    }
    if (rc != SC_VM_OK) return set_vm_error(context, rc);
    return SC_SQLITE_OK;
}

ScSqliteResult sc_sqlite_result_export(ScSqlite *context, sqlite3_context *sql_context, ScValue value)
{
    ScExternalValue external;
    ScVmResult rc;
    char *text;
    if (context == NULL || sql_context == NULL) return SC_SQLITE_MISUSE;
    if (context->config.output_policy == SC_SQLITE_OUTPUT_ALWAYS_PREFIXED_TEXT) {
        rc = sc_vm_value_to_string(&context->vm, value, &text);
        if (rc != SC_VM_OK) return set_vm_error(context, rc);
        {
            size_t n = strlen(text);
            char *prefixed = sqlite3_malloc64((sqlite3_uint64)n + 2u);
            if (prefixed == NULL) { free(text); sqlite3_result_error_nomem(sql_context); return SC_SQLITE_NOMEM; }
            prefixed[0] = context->config.numeric_text_prefix;
            memcpy(prefixed + 1, text, n + 1u);
            free(text);
            sqlite3_result_text(sql_context, prefixed, (int)n + 1, sqlite3_free);
        }
        return SC_SQLITE_OK;
    }
    rc = sc_vm_export_value(&context->vm, value, &external);
    if (rc != SC_VM_OK) return set_vm_error(context, rc);
    if (external.type == SC_EXTERNAL_INTEGER) sqlite3_result_int64(sql_context, external.value.integer);
    else {
        size_t n = strlen(external.value.text);
        char *prefixed = sqlite3_malloc64((sqlite3_uint64)n + 2u);
        if (prefixed == NULL) { free(external.value.text); sqlite3_result_error_nomem(sql_context); return SC_SQLITE_NOMEM; }
        prefixed[0] = context->config.numeric_text_prefix;
        memcpy(prefixed + 1, external.value.text, n + 1u);
        free(external.value.text);
        sqlite3_result_text(sql_context, prefixed, (int)n + 1, sqlite3_free);
    }
    return SC_SQLITE_OK;
}

static ScSqliteResult call_symbol(ScSqlite *context, ScSymbolId symbol, int argc,
                                  sqlite3_value **argv, sqlite3_context *sql_context)
{
    ScValue args[SC_MAX_ARGS] = {0};
    ScValue result = SC_INVALID_VALUE;
    int i;
    ScVmResult rc;
    if (argc < 0 || argc > (int)SC_MAX_ARGS) return SC_SQLITE_RANGE;
    if (context->config.null_policy == SC_SQLITE_NULL_PROPAGATE) {
        for (i = 0; i < argc; ++i) {
            if (sqlite3_value_type(argv[i]) == SQLITE_NULL) {
                sqlite3_result_null(sql_context);
                return SC_SQLITE_OK;
            }
        }
    }
    for (i = 0; i < argc; ++i) {
        ScSqliteResult irc = sc_sqlite_value_import(context, argv[i], &args[i]);
        if (irc != SC_SQLITE_OK) {
            while (--i >= 0) (void)sc_vm_release_value(&context->vm, args[i]);
            return irc;
        }
    }
    rc = sc_vm_call(&context->vm, symbol, args, (uint16_t)argc, &result);
    for (i = 0; i < argc; ++i) (void)sc_vm_release_value(&context->vm, args[i]);
    if (rc != SC_VM_OK) return set_vm_error(context, rc);
    {
        ScSqliteResult erc = sc_sqlite_result_export(context, sql_context, result);
        (void)sc_vm_release_value(&context->vm, result);
        return erc;
    }
}

ScSqliteResult sc_sqlite_call(ScSqlite *context, const char *proc_name, int argc,
                              sqlite3_value **argv, sqlite3_context *sql_context)
{
    ScSymbolId symbol;
    ScSqliteResult rc = resolve_user_proc(context, proc_name, argc, &symbol);
    if (rc != SC_SQLITE_OK) return rc;
    return call_symbol(context, symbol, argc, argv, sql_context);
}

static void report_sql_error(sqlite3_context *sql_context, ScSqlite *context, ScSqliteResult result)
{
    const char *message = context->error.message[0] != '\0' ? context->error.message : sc_sqlite_result_name(result);
    if (result == SC_SQLITE_NOMEM) sqlite3_result_error_nomem(sql_context);
    else sqlite3_result_error(sql_context, message, -1);
}

static void scalar_callback(sqlite3_context *sql_context, int argc, sqlite3_value **argv)
{
    Registration *registration = sqlite3_user_data(sql_context);
    ScSqliteResult rc = call_symbol(registration->owner, registration->step_symbol, argc, argv, sql_context);
    if (rc != SC_SQLITE_OK) report_sql_error(sql_context, registration->owner, rc);
}

static AggregateContext *aggregate_context(sqlite3_context *sql_context, Registration *registration)
{
    AggregateContext *aggregate = sqlite3_aggregate_context(sql_context, sizeof(*aggregate));
    if (aggregate == NULL) return NULL;
#if SC_ENABLE_PERSISTENT_STATE
    if (!aggregate->initialized) {
        if (sc_persistent_state_init(&registration->owner->vm, &aggregate->state,
                                     registration->state_count) != SC_VM_OK) return NULL;
        aggregate->initialized = 1;
    }
#else
    (void)registration;
#endif
    return aggregate;
}

static ScSqliteResult aggregate_call(Registration *registration, AggregateContext *aggregate,
                                     ScSymbolId symbol, int argc, sqlite3_value **argv,
                                     sqlite3_context *sql_context, ScStateAccess access, int emit)
{
#if SC_ENABLE_PERSISTENT_STATE
    ScSqlite *context = registration->owner;
    ScValue args[SC_MAX_ARGS] = {0};
    ScValue result = SC_INVALID_VALUE;
    int i;
    ScVmResult vrc;
    for (i = 0; i < argc; ++i) {
        ScSqliteResult irc = sc_sqlite_value_import(context, argv[i], &args[i]);
        if (irc != SC_SQLITE_OK) { while (--i >= 0) (void)sc_vm_release_value(&context->vm,args[i]); return irc; }
    }
    vrc = sc_vm_state_begin(&context->vm, &aggregate->state, access);
    if (vrc != SC_VM_OK) { for (i=0;i<argc;++i)(void)sc_vm_release_value(&context->vm,args[i]); return set_vm_error(context,vrc); }
    vrc = sc_vm_call(&context->vm, symbol, args, (uint16_t)argc, &result);
    for (i=0;i<argc;++i)(void)sc_vm_release_value(&context->vm,args[i]);
    if (vrc != SC_VM_OK) { sc_vm_state_rollback(&context->vm); return set_vm_error(context,vrc); }
    if (access == SC_STATE_ACCESS_READ_WRITE) vrc = sc_vm_state_commit(&context->vm);
    else sc_vm_state_rollback(&context->vm);
    if (vrc != SC_VM_OK) { (void)sc_vm_release_value(&context->vm,result); return set_vm_error(context,vrc); }
    if (emit) {
        ScSqliteResult erc = sc_sqlite_result_export(context, sql_context, result);
        (void)sc_vm_release_value(&context->vm,result);
        return erc;
    }
    (void)sc_vm_release_value(&context->vm,result);
    return SC_SQLITE_OK;
#else
    (void)registration; (void)aggregate; (void)symbol; (void)argc; (void)argv; (void)sql_context; (void)access; (void)emit;
    return SC_SQLITE_BAD_AGGREGATE_SPEC;
#endif
}

static void step_callback(sqlite3_context *sql_context, int argc, sqlite3_value **argv)
{
    Registration *r = sqlite3_user_data(sql_context);
    AggregateContext *a;
    int i;
    if (r->owner->config.null_policy == SC_SQLITE_NULL_PROPAGATE) {
        for (i = 0; i < argc; ++i) if (sqlite3_value_type(argv[i]) == SQLITE_NULL) return;
    }
    a = aggregate_context(sql_context, r);
    ScSqliteResult rc;
    if (a == NULL) { sqlite3_result_error_nomem(sql_context); return; }
    if (a->failed) return;
    rc = aggregate_call(r, a, r->step_symbol, argc, argv, sql_context, SC_STATE_ACCESS_READ_WRITE, 0);
    if (rc != SC_SQLITE_OK) { a->failed = 1; report_sql_error(sql_context, r->owner, rc); }
}

static void inverse_callback(sqlite3_context *sql_context, int argc, sqlite3_value **argv)
{
    Registration *r = sqlite3_user_data(sql_context);
    AggregateContext *a;
    int i;
    if (r->owner->config.null_policy == SC_SQLITE_NULL_PROPAGATE) {
        for (i = 0; i < argc; ++i) if (sqlite3_value_type(argv[i]) == SQLITE_NULL) return;
    }
    a = aggregate_context(sql_context, r);
    ScSqliteResult rc;
    if (a == NULL) { sqlite3_result_error_nomem(sql_context); return; }
    if (a->failed) return;
    rc = aggregate_call(r, a, r->inverse_symbol, argc, argv, sql_context, SC_STATE_ACCESS_READ_WRITE, 0);
    if (rc != SC_SQLITE_OK) { a->failed = 1; report_sql_error(sql_context, r->owner, rc); }
}

static void value_callback(sqlite3_context *sql_context)
{
    Registration *r = sqlite3_user_data(sql_context);
    AggregateContext *a = aggregate_context(sql_context, r);
    ScSqliteResult rc;
    ScSymbolId symbol = r->value_symbol != SC_INVALID_SYMBOL ? r->value_symbol : r->final_symbol;
    if (a == NULL) { sqlite3_result_error_nomem(sql_context); return; }
    if (a->failed) { sqlite3_result_null(sql_context); return; }
    rc = aggregate_call(r, a, symbol, 0, NULL, sql_context, SC_STATE_ACCESS_READ_ONLY, 1);
    if (rc != SC_SQLITE_OK) report_sql_error(sql_context, r->owner, rc);
}

static void final_callback(sqlite3_context *sql_context)
{
    Registration *r = sqlite3_user_data(sql_context);
    AggregateContext *a = aggregate_context(sql_context, r);
    if (a == NULL) { sqlite3_result_error_nomem(sql_context); return; }
    if (!a->failed) {
        ScSqliteResult rc = aggregate_call(r, a, r->final_symbol, 0, NULL, sql_context,
                                           SC_STATE_ACCESS_READ_ONLY, 1);
        if (rc != SC_SQLITE_OK) report_sql_error(sql_context, r->owner, rc);
    }
#if SC_ENABLE_PERSISTENT_STATE
    if (a->initialized) {
        sc_persistent_state_destroy(&r->owner->vm, &a->state);
        a->initialized = 0;
    }
#endif
}

static ScSqliteResult add_registration(ScSqlite *context, Registration *registration)
{
    registration->next = context->registrations;
    context->registrations = registration;
    return SC_SQLITE_OK;
}

ScSqliteResult sc_sqlite_register_scalar(ScSqlite *context, const ScSqliteScalarSpec *spec)
{
    Registration *registration;
    ScSqliteResult rc;
    int sqlrc;
    if (context == NULL || spec == NULL || !valid_struct(spec->struct_size, sizeof(*spec)) ||
        spec->sql_name == NULL || spec->proc_name == NULL || spec->argc < 0 ||
        !flags_valid(spec->function_flags)) return SC_SQLITE_MISUSE;
    registration = calloc(1u, sizeof(*registration));
    if (registration == NULL) return SC_SQLITE_NOMEM;
    registration->owner = context;
    registration->sql_name = dup_string(spec->sql_name);
    registration->argc = spec->argc;
    registration->flags = spec->function_flags;
    registration->kind = REG_SCALAR;
    if (registration->sql_name == NULL) { free(registration); return SC_SQLITE_NOMEM; }
    rc = resolve_user_proc(context, spec->proc_name, spec->argc, &registration->step_symbol);
    if (rc != SC_SQLITE_OK) { free(registration->sql_name); free(registration); return rc; }
    sqlrc = sqlite3_create_function_v2(context->db, registration->sql_name, registration->argc,
            sqlite_flags(registration->flags), registration, scalar_callback, NULL, NULL,
            registration_destructor);
    if (sqlrc != SQLITE_OK) { free(registration->sql_name); free(registration); return set_error(context,SC_SQLITE_ERROR,sqlrc,sqlite3_errmsg(context->db)); }
    return add_registration(context, registration);
}

ScSqliteResult sc_sqlite_register_aggregate(ScSqlite *context, const ScSqliteAggregateSpec *spec)
{
#if !SC_ENABLE_PERSISTENT_STATE
    (void)context; (void)spec;
    return SC_SQLITE_BAD_AGGREGATE_SPEC;
#else
    Registration *r;
    ScSqliteResult rc;
    int sqlrc;
    if (context == NULL || spec == NULL || !valid_struct(spec->struct_size,sizeof(*spec)) ||
        spec->sql_name == NULL || spec->step_proc == NULL || spec->final_proc == NULL ||
        spec->argc < 0 || spec->state_count < 1u || spec->state_count > SC_SQLITE_MAX_STATE_SLOTS ||
        !flags_valid(spec->function_flags) ||
        (spec->kind != SC_SQLITE_AGGREGATE && spec->kind != SC_SQLITE_WINDOW) ||
        (spec->kind == SC_SQLITE_WINDOW && spec->inverse_proc == NULL) ||
        (spec->kind == SC_SQLITE_AGGREGATE && (spec->inverse_proc != NULL || spec->value_proc != NULL)))
        return set_error(context,SC_SQLITE_BAD_AGGREGATE_SPEC,SQLITE_MISUSE,"invalid aggregate/window specification");
    r = calloc(1u,sizeof(*r));
    if (r == NULL) return SC_SQLITE_NOMEM;
    r->owner=context; r->sql_name=dup_string(spec->sql_name); r->argc=spec->argc;
    r->state_count=(uint16_t)spec->state_count; r->flags=spec->function_flags;
    r->kind=spec->kind==SC_SQLITE_WINDOW?REG_WINDOW:REG_AGGREGATE;
    r->inverse_symbol=r->value_symbol=SC_INVALID_SYMBOL;
    if (r->sql_name==NULL) { free(r); return SC_SQLITE_NOMEM; }
    rc=resolve_user_proc(context,spec->step_proc,spec->argc,&r->step_symbol);
    if(rc==SC_SQLITE_OK) rc=resolve_user_proc(context,spec->final_proc,0,&r->final_symbol);
    if(rc==SC_SQLITE_OK && spec->inverse_proc!=NULL) rc=resolve_user_proc(context,spec->inverse_proc,spec->argc,&r->inverse_symbol);
    if(rc==SC_SQLITE_OK && spec->value_proc!=NULL) rc=resolve_user_proc(context,spec->value_proc,0,&r->value_symbol);
    if(rc!=SC_SQLITE_OK){free(r->sql_name);free(r);return rc;}
    if(r->kind==REG_WINDOW)
        sqlrc=sqlite3_create_window_function(context->db,r->sql_name,r->argc,sqlite_flags(r->flags),r,
                step_callback,final_callback,value_callback,inverse_callback,registration_destructor);
    else
        sqlrc=sqlite3_create_function_v2(context->db,r->sql_name,r->argc,sqlite_flags(r->flags),r,
                NULL,step_callback,final_callback,registration_destructor);
    if(sqlrc!=SQLITE_OK){free(r->sql_name);free(r);return set_error(context,SC_SQLITE_ERROR,sqlrc,sqlite3_errmsg(context->db));}
    return add_registration(context,r);
#endif
}

ScSqliteResult sc_sqlite_register_all(ScSqlite *context)
{
    size_t i;
    if (context == NULL) return SC_SQLITE_MISUSE;
    for (i=0;i<context->user_count;++i) {
        ScSymbol *s=&context->vm.symbols.symbols[context->user_symbols[i]];
        ScSqliteScalarSpec spec;
        char *name;
        size_t a=0,b=strlen(s->name);
        if(is_private_name(s->name)) continue;
        if(context->config.name_policy==SC_SQLITE_NAMES_WITH_PREFIX && context->config.sql_name_prefix!=NULL) a=strlen(context->config.sql_name_prefix);
        name=malloc(a+b+1u); if(name==NULL)return SC_SQLITE_NOMEM;
        if (a != 0u) memcpy(name, context->config.sql_name_prefix, a);
        memcpy(name + a, s->name, b + 1u);
        memset(&spec,0,sizeof(spec)); spec.struct_size=sizeof(spec); spec.sql_name=name; spec.proc_name=s->name;
        spec.argc=s->argc; spec.function_flags=context->config.default_function_flags;
        { ScSqliteResult rc=sc_sqlite_register_scalar(context,&spec); free(name); if(rc!=SC_SQLITE_OK)return rc; }
    }
    return SC_SQLITE_OK;
}

ScSqliteResult sc_sqlite_unregister(ScSqlite *context, const char *sql_name, int argc)
{
    Registration *r;
    int rc;
    if(context==NULL||sql_name==NULL)return SC_SQLITE_MISUSE;
    for(r=context->registrations;r!=NULL;r=r->next) if(r->argc==argc&&strcmp(r->sql_name,sql_name)==0)break;
    if(r==NULL)return SC_SQLITE_NOT_FOUND;
    if(r->kind==REG_WINDOW) rc=sqlite3_create_window_function(context->db,sql_name,argc,SQLITE_UTF8,NULL,NULL,NULL,NULL,NULL,NULL);
    else rc=sqlite3_create_function_v2(context->db,sql_name,argc,SQLITE_UTF8,NULL,NULL,NULL,NULL,NULL);
    return rc==SQLITE_OK?SC_SQLITE_OK:set_error(context,SC_SQLITE_ERROR,rc,sqlite3_errmsg(context->db));
}

int sc_sqlite_result_to_sqlite_code(ScSqliteResult result)
{
    switch(result){
    case SC_SQLITE_OK:return SQLITE_OK; case SC_SQLITE_NOMEM:return SQLITE_NOMEM;
    case SC_SQLITE_MISUSE:return SQLITE_MISUSE; case SC_SQLITE_RANGE:return SQLITE_RANGE;
    case SC_SQLITE_NOT_FOUND:return SQLITE_NOTFOUND; case SC_SQLITE_UNSUPPORTED_VALUE:return SQLITE_MISMATCH;
    default:return SQLITE_ERROR;
    }
}

const char *sc_sqlite_result_name(ScSqliteResult result)
{
    switch(result){
    case SC_SQLITE_OK:return "ok"; case SC_SQLITE_ERROR:return "sqlite error"; case SC_SQLITE_NOMEM:return "out of memory";
    case SC_SQLITE_MISUSE:return "misuse"; case SC_SQLITE_RANGE:return "range error";
    case SC_SQLITE_COMPILE_ERROR:return "compile error"; case SC_SQLITE_RUNTIME_ERROR:return "runtime error";
    case SC_SQLITE_INVALID_PREFIX:return "invalid prefix"; case SC_SQLITE_DUPLICATE_FUNCTION:return "duplicate function";
    case SC_SQLITE_UNSUPPORTED_VALUE:return "unsupported value"; case SC_SQLITE_NOT_FOUND:return "not found";
    case SC_SQLITE_BAD_AGGREGATE_SPEC:return "bad aggregate specification";
    case SC_SQLITE_BAD_LIBRARY_FLAGS:return "bad library level"; default:return "unknown sqlite adapter result";
    }
}


typedef struct SqlBootstrap {
    ScSqlite *context;
    unsigned references;
    unsigned source_number;
} SqlBootstrap;

static void sql_bootstrap_destroy(void *opaque)
{
    SqlBootstrap *bootstrap = opaque;
    if (bootstrap == NULL) return;
    if (--bootstrap->references == 0u) {
        sc_sqlite_destroy(bootstrap->context);
        sqlite3_free(bootstrap);
    }
}

static void sql_adapter_error(sqlite3_context *sql_context, ScSqlite *context,
                              ScSqliteResult result)
{
    const ScSqliteError *error = sc_sqlite_error(context);
    const char *message = error != NULL && error->message[0] != '\0'
        ? error->message : sc_sqlite_result_name(result);
    sqlite3_result_error(sql_context, message, -1);
    sqlite3_result_error_code(sql_context, sc_sqlite_result_to_sqlite_code(result));
}

static int sql_text_arg(sqlite3_context *sql_context, sqlite3_value *value,
                        const char **out)
{
    const unsigned char *text;
    if (sqlite3_value_type(value) == SQLITE_NULL) {
        sqlite3_result_error(sql_context, "argument must not be NULL", -1);
        return 0;
    }
    text = sqlite3_value_text(value);
    if (text == NULL) {
        sqlite3_result_error_nomem(sql_context);
        return 0;
    }
    *out = (const char *)text;
    return 1;
}

static void sql_bclite_load(sqlite3_context *sql_context, int argc,
                            sqlite3_value **argv)
{
    SqlBootstrap *bootstrap = sqlite3_user_data(sql_context);
    ScSqliteSource source;
    ScSqliteResult result;
    const char *text;
    char name[48];
    (void)argc;
    if (!sql_text_arg(sql_context, argv[0], &text)) return;
    memset(&source, 0, sizeof(source));
    source.struct_size = sizeof(source);
    snprintf(name, sizeof(name), "sql-source-%u", ++bootstrap->source_number);
    source.name = name;
    source.text = text;
    source.text_len = (size_t)sqlite3_value_bytes(argv[0]);
    result = sc_sqlite_load_source(bootstrap->context, &source);
    if (result != SC_SQLITE_OK) { sql_adapter_error(sql_context, bootstrap->context, result); return; }
    sqlite3_result_int(sql_context, 1);
}

static void sql_bclite_register_all(sqlite3_context *sql_context, int argc,
                                    sqlite3_value **argv)
{
    SqlBootstrap *bootstrap = sqlite3_user_data(sql_context);
    ScSqliteResult result;
    (void)argc; (void)argv;
    result = sc_sqlite_register_all(bootstrap->context);
    if (result != SC_SQLITE_OK) { sql_adapter_error(sql_context, bootstrap->context, result); return; }
    sqlite3_result_int(sql_context, 1);
}

static void sql_bclite_register_scalar(sqlite3_context *sql_context, int argc,
                                       sqlite3_value **argv)
{
    SqlBootstrap *bootstrap = sqlite3_user_data(sql_context);
    ScSqliteScalarSpec spec;
    ScSqliteResult result;
    const char *sql_name, *proc_name;
    (void)argc;
    if (!sql_text_arg(sql_context, argv[0], &sql_name) ||
        !sql_text_arg(sql_context, argv[1], &proc_name)) return;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.sql_name = sql_name;
    spec.proc_name = proc_name;
    spec.argc = sqlite3_value_int(argv[2]);
    spec.function_flags = SC_SQLITE_FUNC_DETERMINISTIC | SC_SQLITE_FUNC_DIRECTONLY;
    result = sc_sqlite_register_scalar(bootstrap->context, &spec);
    if (result != SC_SQLITE_OK) { sql_adapter_error(sql_context, bootstrap->context, result); return; }
    sqlite3_result_int(sql_context, 1);
}

static void sql_bclite_register_aggregate(sqlite3_context *sql_context, int argc,
                                          sqlite3_value **argv)
{
    SqlBootstrap *bootstrap = sqlite3_user_data(sql_context);
    ScSqliteAggregateSpec spec;
    ScSqliteResult result;
    const char *sql_name, *step_proc, *final_proc;
    (void)argc;
    if (!sql_text_arg(sql_context, argv[0], &sql_name) ||
        !sql_text_arg(sql_context, argv[3], &step_proc) ||
        !sql_text_arg(sql_context, argv[4], &final_proc)) return;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.sql_name = sql_name;
    spec.argc = sqlite3_value_int(argv[1]);
    spec.state_count = (size_t)sqlite3_value_int64(argv[2]);
    spec.step_proc = step_proc;
    spec.final_proc = final_proc;
    spec.function_flags = SC_SQLITE_FUNC_DETERMINISTIC | SC_SQLITE_FUNC_DIRECTONLY;
    spec.kind = SC_SQLITE_AGGREGATE;
    result = sc_sqlite_register_aggregate(bootstrap->context, &spec);
    if (result != SC_SQLITE_OK) { sql_adapter_error(sql_context, bootstrap->context, result); return; }
    sqlite3_result_int(sql_context, 1);
}

static void sql_bclite_register_window(sqlite3_context *sql_context, int argc,
                                       sqlite3_value **argv)
{
    SqlBootstrap *bootstrap = sqlite3_user_data(sql_context);
    ScSqliteAggregateSpec spec;
    ScSqliteResult result;
    const char *sql_name, *step_proc, *inverse_proc, *value_proc, *final_proc;
    (void)argc;
    if (!sql_text_arg(sql_context, argv[0], &sql_name) ||
        !sql_text_arg(sql_context, argv[3], &step_proc) ||
        !sql_text_arg(sql_context, argv[4], &inverse_proc) ||
        !sql_text_arg(sql_context, argv[5], &value_proc) ||
        !sql_text_arg(sql_context, argv[6], &final_proc)) return;
    memset(&spec, 0, sizeof(spec));
    spec.struct_size = sizeof(spec);
    spec.sql_name = sql_name;
    spec.argc = sqlite3_value_int(argv[1]);
    spec.state_count = (size_t)sqlite3_value_int64(argv[2]);
    spec.step_proc = step_proc;
    spec.inverse_proc = inverse_proc;
    spec.value_proc = value_proc;
    spec.final_proc = final_proc;
    spec.function_flags = SC_SQLITE_FUNC_DETERMINISTIC | SC_SQLITE_FUNC_DIRECTONLY;
    spec.kind = SC_SQLITE_WINDOW;
    result = sc_sqlite_register_aggregate(bootstrap->context, &spec);
    if (result != SC_SQLITE_OK) { sql_adapter_error(sql_context, bootstrap->context, result); return; }
    sqlite3_result_int(sql_context, 1);
}

static void sql_bclite_prefix(sqlite3_context *sql_context, int argc,
                              sqlite3_value **argv)
{
    SqlBootstrap *bootstrap = sqlite3_user_data(sql_context);
    ScSqliteResult result;
    const char *text;
    if (argc == 0) {
        char result_text[2] = { sc_sqlite_get_numeric_text_prefix(bootstrap->context), '\0' };
        sqlite3_result_text(sql_context, result_text, 1, SQLITE_TRANSIENT);
        return;
    }
    if (!sql_text_arg(sql_context, argv[0], &text)) return;
    if (sqlite3_value_bytes(argv[0]) != 1) {
        sqlite3_result_error(sql_context, "numeric prefix must be exactly one byte", -1);
        return;
    }
    result = sc_sqlite_set_numeric_text_prefix(bootstrap->context, text[0]);
    if (result != SC_SQLITE_OK) { sql_adapter_error(sql_context, bootstrap->context, result); return; }
    sqlite3_result_text(sql_context, text, 1, SQLITE_TRANSIENT);
}

static int register_sql_admin(sqlite3 *db, SqlBootstrap *bootstrap, const char *name,
                              int argc, void (*callback)(sqlite3_context *, int, sqlite3_value **))
{
    int flags = SQLITE_UTF8 | SQLITE_DIRECTONLY;
    int rc;
    bootstrap->references++;
    rc = sqlite3_create_function_v2(db, name, argc, flags, bootstrap, callback,
                                    NULL, NULL, sql_bootstrap_destroy);
    if (rc != SQLITE_OK) bootstrap->references--;
    return rc;
}

int sqlite3_scbclite_init(sqlite3 *db, char **error_message, const sqlite3_api_routines *api)
{
    SqlBootstrap *bootstrap;
    ScSqliteConfig config;
    ScSqliteResult result;
    int rc = SQLITE_OK;
    SQLITE_EXTENSION_INIT2(api);
    bootstrap = sqlite3_malloc64(sizeof(*bootstrap));
    if (bootstrap == NULL) return SQLITE_NOMEM;
    memset(bootstrap, 0, sizeof(*bootstrap));
    sc_sqlite_config_init(&config);
    config.library_level = SC_SQLITE_LIBRARY_1_2_3;
    result = sc_sqlite_create(db, &config, &bootstrap->context);
    if (result != SC_SQLITE_OK) {
        sqlite3_free(bootstrap);
        if (error_message != NULL) *error_message = sqlite3_mprintf("BC-Lite initialization failed: %s", sc_sqlite_result_name(result));
        return sc_sqlite_result_to_sqlite_code(result);
    }
    rc = register_sql_admin(db, bootstrap, "sc_bclite_load", 1, sql_bclite_load);
    if (rc == SQLITE_OK) rc = register_sql_admin(db, bootstrap, "sc_bclite_register_all", 0, sql_bclite_register_all);
    if (rc == SQLITE_OK) rc = register_sql_admin(db, bootstrap, "sc_bclite_register_scalar", 3, sql_bclite_register_scalar);
    if (rc == SQLITE_OK) rc = register_sql_admin(db, bootstrap, "sc_bclite_register_aggregate", 5, sql_bclite_register_aggregate);
    if (rc == SQLITE_OK) rc = register_sql_admin(db, bootstrap, "sc_bclite_register_window", 7, sql_bclite_register_window);
    if (rc == SQLITE_OK) rc = register_sql_admin(db, bootstrap, "sc_bclite_prefix", 0, sql_bclite_prefix);
    if (rc == SQLITE_OK) rc = register_sql_admin(db, bootstrap, "sc_bclite_prefix", 1, sql_bclite_prefix);
    if (rc != SQLITE_OK) {
        if (bootstrap->references == 0u) { sc_sqlite_destroy(bootstrap->context); sqlite3_free(bootstrap); }
        if (error_message != NULL) *error_message = sqlite3_mprintf("BC-Lite SQL API registration failed: %s", sqlite3_errmsg(db));
        return rc;
    }
    return SQLITE_OK;
}

#ifdef SC_SQLITE_DEFAULT_ENTRYPOINT
int sqlite3_extension_init(sqlite3 *db, char **error_message, const sqlite3_api_routines *api)
{
    return sqlite3_scbclite_init(db,error_message,api);
}
#endif
