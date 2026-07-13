#ifndef SC_COMPILER_H
#define SC_COMPILER_H

/*
 * Direct BC-Lite source-to-bytecode compiler and minimal stack VM interface.
 *
 * Language special forms:
 *   proc name {args...} {body}
 *   set name value
 *   if condition {then-body} {else-body}
 *   loop {body}
 *   break
 *   return value
 *
 * All other commands are fixed-arity builtin or user-procedure calls.
 *
 * Runtime values are ScValue identifiers indexing ScVm.slots[].
 */

#include <sys/types.h>
#include <bcl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Limits and initial capacities                                              */
/* ------------------------------------------------------------------------- */

#define SC_COMPILER_CODE_CAP          128u
#define SC_COMPILER_LOCAL_CAP          32u
#define SC_COMPILER_PATCH_CAP          32u

#define SC_MAX_SOURCE_LEN       (1024u * 1024u)
#define SC_MAX_BYTECODE_BYTES   (1024u * 1024u)
#define SC_MAX_PROCS                  1024u
#define SC_MAX_SYMBOLS                4096u
#define SC_MAX_CONSTANTS              4096u

#define SC_MAX_ARGS                     64u
#define SC_MAX_LOCALS                  256u
#define SC_MAX_SLOTS   (SC_MAX_ARGS + SC_MAX_LOCALS)
#define SC_MAX_STACK                  1024u
#define SC_MAX_FRAMES                   64u
#define SC_MAX_LOOP_DEPTH               32u
#define SC_MAX_PARSE_DEPTH              256u
#define SC_MAX_BREAKS_PER_LOOP          64u

#define SC_MAX_INSTRUCTIONS        1000000u
#define SC_MAX_NAME_LEN                 128u
#define SC_MAX_ERROR_MSG_LEN            256u

#ifndef SC_ENABLE_PERSISTENT_STATE
#define SC_ENABLE_PERSISTENT_STATE 1
#endif

#if SC_ENABLE_PERSISTENT_STATE
#ifndef SC_PERSISTENT_STATE_SLOTS
#define SC_PERSISTENT_STATE_SLOTS 10u
#endif
#if SC_PERSISTENT_STATE_SLOTS < 1u
#error "SC_PERSISTENT_STATE_SLOTS must be at least 1"
#endif
#if SC_PERSISTENT_STATE_SLOTS > UINT16_MAX
#error "SC_PERSISTENT_STATE_SLOTS exceeds supported range"
#endif
#endif

#define SC_INVALID_VALUE          UINT32_MAX
#define SC_INVALID_SYMBOL         UINT32_MAX
#define SC_INVALID_INDEX          UINT32_MAX
#define SC_NO_FREE_SLOT           UINT32_MAX

/* ------------------------------------------------------------------------- */
/* Fundamental identifiers and source slices                                 */
/* ------------------------------------------------------------------------- */

typedef uint32_t ScValue;       /* Index into ScVm.slots[]. */
typedef uint32_t ScSymbolId;    /* Index into ScSymbolTable.symbols[]. */
typedef uint32_t ScProcId;      /* Index into ScVm.procs[]. */
typedef uint32_t ScCFuncId;     /* Index into ScVm.cfuncs[]. */
typedef uint32_t ScInstrIndex;  /* Instruction index within one procedure. */

typedef struct ScSlice {
    const char *p;
    size_t len;
} ScSlice;

#define SC_SLICE_LITERAL(text) ((ScSlice){ (text), sizeof(text) - 1u })

static inline ScSlice
sc_slice_make(const char *p, size_t len)
{
    ScSlice slice = { p, len };
    return slice;
}

/* ------------------------------------------------------------------------- */
/* Errors                                                                     */
/* ------------------------------------------------------------------------- */

typedef enum ScCompileResult {
    SC_COMPILE_OK = 0,

    SC_ERR_COMPILE_SYNTAX = 200,
    SC_ERR_COMPILE_UNEXPECTED_EOF,
    SC_ERR_COMPILE_UNKNOWN_VAR,
    SC_ERR_COMPILE_UNKNOWN_FUNC,
    SC_ERR_COMPILE_ARG_COUNT_MISMATCH,
    SC_ERR_COMPILE_DUPLICATE_VAR,
    SC_ERR_COMPILE_DUPLICATE_PROC,
    SC_ERR_COMPILE_NAME_TOO_LONG,
    SC_ERR_COMPILE_MAX_ARGS,
    SC_ERR_COMPILE_MAX_LOCALS,
    SC_ERR_COMPILE_MAX_STACK,
    SC_ERR_COMPILE_MAX_LOOP_DEPTH,
    SC_ERR_COMPILE_MAX_NESTING,
    SC_ERR_COMPILE_MAX_BREAKS,
    SC_ERR_COMPILE_MAX_CONSTANTS,
    SC_ERR_COMPILE_MAX_SYMBOLS,
    SC_ERR_COMPILE_MAX_BYTECODE,
    SC_ERR_COMPILE_INVALID_CONSTANT,
    SC_ERR_COMPILE_INVALID_EXPR,
    SC_ERR_COMPILE_BREAK_OUTSIDE_LOOP,
    SC_ERR_COMPILE_RETURN_OUTSIDE_PROC,
    SC_ERR_COMPILE_MISSING_BRACE,
    SC_ERR_COMPILE_MISSING_BRACKET,
    SC_ERR_COMPILE_INVALID_PROC_DEF,
    SC_ERR_COMPILE_NO_RETURN_VALUE,
    SC_ERR_COMPILE_TRAILING_TEXT,
    SC_ERR_COMPILE_NO_MEM,
    SC_ERR_COMPILE_INVALID_PREFIX,
    SC_ERR_COMPILE_INVALID_LIBRARY_LEVEL,
    SC_ERR_COMPILE_LIBRARY_UNAVAILABLE,
    SC_ERR_COMPILE_INTERNAL
} ScCompileResult;

typedef enum ScVmResult {
    SC_VM_OK = 0,

    SC_ERR_RUNTIME = 100,
    SC_ERR_CONTEXT_CREATE,
    SC_ERR_CONTEXT_PUSH,
    SC_ERR_ARITHMETIC,
    SC_ERR_DIVZERO,
    SC_ERR_TYPE,
    SC_ERR_INVALID_VALUE,
    SC_ERR_INVALID_OPCODE,
    SC_ERR_INVALID_SLOT,
    SC_ERR_INVALID_SYMBOL,
    SC_ERR_INVALID_PROC,
    SC_ERR_EMPTY_PROC,
    SC_ERR_STACK_UNDERFLOW,
    SC_ERR_STACK_OVERFLOW,
    SC_ERR_FRAME_OVERFLOW,
    SC_ERR_SLOT_LIMIT,
    SC_ERR_LOCAL_LIMIT,
    SC_ERR_ARG_COUNT,
    SC_ERR_NO_RETURN_VALUE,
    SC_ERR_MAX_INSTRUCTIONS,
    SC_ERR_NO_MEM,
    SC_ERR_INVALID_PREFIX,
#if SC_ENABLE_PERSISTENT_STATE
    SC_ERR_STATE_NOT_ACTIVE,
    SC_ERR_STATE_ALREADY_ACTIVE,
    SC_ERR_STATE_READ_ONLY,
    SC_ERR_STATE_RANGE,
    SC_ERR_STATE_INVALID_COUNT,
#endif
    SC_ERR_INTERNAL
} ScVmResult;

typedef enum ScVmHealth {
    SC_VM_HEALTH_READY = 0,
    SC_VM_HEALTH_RECOVERABLE_ERROR,
    SC_VM_HEALTH_FATAL_ERROR
} ScVmHealth;

typedef struct ScCompileError {
    ScCompileResult code;
    size_t source_pos;
    uint32_t line;
    uint32_t column;
    char message[SC_MAX_ERROR_MSG_LEN];
} ScCompileError;

typedef struct ScVmError {
    ScVmResult code;
    ScSymbolId symbol_id;
    ScInstrIndex ip;
    char message[SC_MAX_ERROR_MSG_LEN];
} ScVmError;

/* ------------------------------------------------------------------------- */
/* Security/runtime limits                                                    */
/* ------------------------------------------------------------------------- */

typedef struct ScSecurityLimits {
    uint32_t max_source_len;
    uint32_t max_bytecode_bytes;
    uint32_t max_procs;
    uint32_t max_symbols;
    uint32_t max_constants;

    uint16_t max_args;
    uint16_t max_locals;
    uint16_t max_stack;
    uint16_t max_frames;
    uint16_t max_loop_depth;
    uint16_t max_parse_depth;

    uint64_t max_instructions;
} ScSecurityLimits;

void sc_security_limits_default(ScSecurityLimits *limits);

#ifdef SC_TESTING
/* Deterministic allocator fault injection for the test suite only. */
void sc_test_alloc_reset(void);
void sc_test_alloc_fail_at(size_t allocation_number);
size_t sc_test_alloc_count(void);
#endif

/* ------------------------------------------------------------------------- */
/* Bytecode                                                                   */
/* ------------------------------------------------------------------------- */

typedef enum ScVmOpCode {
    SC_OP_CALL = 1,
    SC_OP_CONST,
    SC_OP_JMP,
    SC_OP_JNZ,
    SC_OP_JZ,
    SC_OP_LOAD,
    SC_OP_POP,
    SC_OP_RET,
    SC_OP_STORE
} ScVmOpCode;

/*
 * Hints do not change execution semantics. They preserve the source construct
 * that generated a jump so disassembly remains readable.
 */
typedef enum ScJumpHint {
    SC_JUMP_PLAIN = 0,
    SC_JUMP_IF_FALSE,
    SC_JUMP_IF_END,
    SC_JUMP_LOOP_BACK,
    SC_JUMP_LOOP_BREAK
} ScJumpHint;

/*
 * Logical operand usage:
 *
 *   CONST: a unused,       b = constant ScValue
 *   LOAD:  a = frame slot, b unused
 *   STORE: a = frame slot, b unused
 *   CALL:  a = argc,       b = ScSymbolId
 *   JMP:   a unused,       b = target instruction index
 *   JZ:    a unused,       b = target instruction index
 *   JNZ:   a unused,       b = target instruction index
 *   RET:   operands unused
 *   POP:   operands unused
 */
typedef struct ScInstr {
    uint8_t op;
    uint8_t hint;
    uint16_t a;
    uint32_t b;
} ScInstr;

/* ------------------------------------------------------------------------- */
/* Symbols and procedures                                                     */
/* ------------------------------------------------------------------------- */

typedef enum ScSymbolKind {
    SC_SYM_EMPTY = 0,  /* Forward declaration only. */
    SC_SYM_CFUNC,
    SC_SYM_PROC
} ScSymbolKind;

typedef enum ScDeclOrigin {
    SC_DECL_EXTERNAL = 0,
    SC_DECL_SOURCE
} ScDeclOrigin;

typedef struct ScSymbol {
    char *name;
    uint16_t name_len;
    uint16_t argc;          /* All calls are fixed-arity. */
    uint32_t hash;
    uint32_t next;          /* Hash-chain index or SC_INVALID_INDEX. */

    ScSymbolKind kind;
    ScDeclOrigin decl_origin;
    uint32_t compilation_id;

    union {
        ScProcId proc_id;
        ScCFuncId cfunc_id;
    } target;
} ScSymbol;

typedef struct ScSymbolTable {
    uint32_t *buckets;
    uint32_t bucket_count;

    ScSymbol *symbols;
    uint32_t count;
    uint32_t capacity;
} ScSymbolTable;

typedef struct ScProc {
    ScSymbolId symbol_id;

    uint16_t argc;
    uint16_t slot_count;    /* Arguments first, followed by locals. */
    uint16_t max_stack;
    uint16_t flags;

    ScInstr *code;
    uint32_t code_count;
    uint32_t code_capacity;

    uint32_t source_line;
} ScProc;

/* ------------------------------------------------------------------------- */
/* Runtime values, frames and VM                                              */
/* ------------------------------------------------------------------------- */

/*
 * refcnt == -1 means a connection-owned permanent value, such as a constant,
 * zero or one. Otherwise refcnt is the number of owning VM references.
 */
typedef struct ScSlot {
    BclNumber num;
    int32_t refcnt;
    uint32_t next_free;
} ScSlot;


#if SC_ENABLE_PERSISTENT_STATE
typedef enum ScStateAccess {
    SC_STATE_ACCESS_READ_ONLY = 1,
    SC_STATE_ACCESS_READ_WRITE = 2
} ScStateAccess;

typedef struct ScPersistentState {
    ScValue values[SC_PERSISTENT_STATE_SLOTS];
    uint16_t count;
    bool initialized;
} ScPersistentState;

typedef struct ScStateBinding {
    ScPersistentState *state;
    ScValue working[SC_PERSISTENT_STATE_SLOTS];
    uint16_t count;
    ScStateAccess access;
    bool active;
} ScStateBinding;
#endif

typedef struct ScFrame {
    ScProc *proc;
    ScInstrIndex ip;

    /*
     * values[0 .. slot_count-1] are argument/local slots.
     * values[slot_count .. sp-1] are the operand stack.
     */
    ScValue *values;
    uint16_t slot_count;
    uint16_t sp;
    uint16_t capacity;      /* slot_count + proc->max_stack */
} ScFrame;

struct ScVm;
typedef ScVmResult (*ScCFunc)(struct ScVm *vm);

typedef enum ScLibraryLevel {
    SC_LIBRARY_NONE = 0,
    SC_LIBRARY_1 = 1,
    SC_LIBRARY_1_2 = 2,
    SC_LIBRARY_1_2_3 = 3
} ScLibraryLevel;

typedef struct ScVm {
    BclContext ctx;

    /* Staged BCL lifecycle; permits complete, idempotent partial teardown. */
    bool bcl_started;
    bool bcl_initialized;
    bool context_created;
    bool context_pushed;
    bool symbols_initialized;

    ScFrame *frames;
    uint16_t frame_count;
    uint16_t frame_capacity;

    uint64_t instruction_count;

    ScSlot *slots;
    uint32_t slot_count;
    uint32_t slot_capacity;
    uint32_t free_head;

    ScValue zero_value;
    ScValue one_value;

    ScProc *procs;
    uint32_t proc_count;
    uint32_t proc_capacity;

    ScCFunc *cfuncs;
    uint32_t cfunc_count;
    uint32_t cfunc_capacity;

    ScSymbolTable symbols;
    ScSecurityLimits limits;
    ScVmError error;
    ScVmHealth health;
    uint32_t next_compilation_id;
    char numeric_text_prefix;
    ScLibraryLevel loaded_library_level;

    /*
     * Set while a C builtin is running. These helpers avoid exposing frame
     * layout to each builtin.
     */
    const ScValue *call_args;
    uint16_t call_argc;
    ScValue call_result;
    bool call_result_set;
#if SC_ENABLE_PERSISTENT_STATE
    ScStateBinding state_binding;
#endif
} ScVm;

/* ------------------------------------------------------------------------- */
/* Compiler state                                                             */
/* ------------------------------------------------------------------------- */

typedef struct ScLocal {
    ScSlice name;
    uint16_t slot;
    bool is_arg;
} ScLocal;

typedef struct ScScope {
    ScLocal *locals;
    uint16_t count;
    uint16_t capacity;
} ScScope;

typedef struct ScPatch {
    ScInstrIndex instruction;
} ScPatch;

typedef struct ScLoopCtx {
    ScInstrIndex start_ip;
    uint32_t first_break_patch;
} ScLoopCtx;

typedef struct ScCompiler {
    ScVm *vm;

    ScSlice source;
    size_t pos;
    uint32_t line;
    uint32_t column;

    ScProc *current_proc;
    ScSymbolId current_proc_symbol;
    bool in_proc;
    bool has_return;

    ScScope scope;

    ScLoopCtx loops[SC_MAX_LOOP_DEPTH];
    uint16_t loop_depth;
    uint16_t parse_depth;

    ScPatch *break_patches;
    uint32_t break_patch_count;
    uint32_t break_patch_capacity;

    uint16_t stack_depth;
    uint16_t max_stack;

    /* Prefix marking numeric literals in source (default: #). Set to \0 to disable. */
    char constant_prefix;

    /* Nonzero while compiling one complete source string. */
    uint32_t compilation_id;

    ScCompileError error;
} ScCompiler;

/* ------------------------------------------------------------------------- */
/* Public compiler API                                                        */
/* ------------------------------------------------------------------------- */

/*
 * Initialise/reset a compiler object. The compiler borrows vm and source.
 * sc_compiler_destroy() frees compiler-owned temporary arrays only.
 */
void sc_compiler_init_slice(
    ScCompiler *compiler,
    ScVm *vm,
    ScSlice source
);

void sc_compiler_init(
    ScCompiler *compiler,
    ScVm *vm,
    const char *source,
    size_t source_len
);

void sc_compiler_reset_source_slice(
    ScCompiler *compiler,
    ScSlice source,
    size_t source_pos
);

void sc_compiler_reset_source(
    ScCompiler *compiler,
    const char *source,
    size_t source_len,
    size_t source_pos
);

void sc_compiler_destroy(ScCompiler *compiler);

/* Accept ASCII letters or one of # @ % ! ? : ^ ~. Default is '#'. */
bool sc_numeric_prefix_valid(char prefix);
ScCompileResult sc_compiler_set_constant_prefix(ScCompiler *compiler, char prefix);
char sc_compiler_get_constant_prefix(const ScCompiler *compiler);

/*
 * Compile one proc beginning at compiler->pos. On success, compiler->pos is
 * immediately after that proc and *out_proc_id identifies the installed proc.
 */
ScCompileResult sc_compile_proc(
    ScCompiler *compiler,
    ScProcId *out_proc_id
);

/* Compile all proc definitions until end of source. */
ScCompileResult sc_compile_source(
    ScCompiler *compiler,
    uint32_t *out_proc_count
);

const ScCompileError *sc_compiler_error(const ScCompiler *compiler);

/* ------------------------------------------------------------------------- */
/* Direct recursive compiler functions                                       */
/* ------------------------------------------------------------------------- */

/* Top-level and command/body compilation. */
ScCompileResult sc_compile_body(
    ScCompiler *compiler,
    int terminator
);

ScCompileResult sc_compile_command(ScCompiler *compiler);

ScCompileResult sc_compile_expression(ScCompiler *compiler);
ScCompileResult sc_compile_bracketed(ScCompiler *compiler);
ScCompileResult sc_compile_braced_body(ScCompiler *compiler);

/* Special forms. Command name has already been consumed. */
ScCompileResult sc_compile_set(ScCompiler *compiler);
ScCompileResult sc_compile_if(ScCompiler *compiler);
ScCompileResult sc_compile_loop(ScCompiler *compiler);
ScCompileResult sc_compile_break(ScCompiler *compiler);
ScCompileResult sc_compile_return(ScCompiler *compiler);

/* Ordinary fixed-arity call. */
ScCompileResult sc_compile_call(
    ScCompiler *compiler,
    ScSlice command_name,
    int terminator
);

/* Atomic value forms. */
ScCompileResult sc_compile_variable(ScCompiler *compiler);
ScCompileResult sc_compile_constant(
    ScCompiler *compiler,
    ScSlice literal
);

/* ------------------------------------------------------------------------- */
/* Character parsing                                                         */
/* ------------------------------------------------------------------------- */

int sc_peek(const ScCompiler *compiler);
int sc_next(ScCompiler *compiler);
bool sc_match(ScCompiler *compiler, int expected);

ScCompileResult sc_expect(
    ScCompiler *compiler,
    int expected,
    const char *message
);

void sc_skip_inline_whitespace(ScCompiler *compiler);
void sc_skip_command_separators(ScCompiler *compiler);

bool sc_at_command_end(
    const ScCompiler *compiler,
    int terminator
);

/* ------------------------------------------------------------------------- */
/* Atomic token parsing                                                       */
/* ------------------------------------------------------------------------- */

/* These functions return non-owning token slices into compiler->source.
 * parse_variable consumes the leading '$'. Structured braced and bracketed
 * forms are compiled directly from the main source cursor. */
ScCompileResult sc_parse_bareword(
    ScCompiler *compiler,
    ScSlice *out_word
);

ScCompileResult sc_parse_variable(
    ScCompiler *compiler,
    ScSlice *out_name
);

/* ------------------------------------------------------------------------- */
/* Compiler symbol/local helpers                                              */
/* ------------------------------------------------------------------------- */

bool sc_slice_equal_cstr(ScSlice slice, const char *text);
uint32_t sc_hash_bytes(const char *p, size_t len);

ScCompileResult sc_find_local(
    const ScCompiler *compiler,
    ScSlice name,
    uint16_t *out_slot
);

ScCompileResult sc_find_or_create_local(
    ScCompiler *compiler,
    ScSlice name,
    uint16_t *out_slot
);

ScCompileResult sc_add_argument(
    ScCompiler *compiler,
    ScSlice name,
    uint16_t *out_slot
);

ScCompileResult sc_find_symbol(
    const ScCompiler *compiler,
    ScSlice name,
    ScSymbolId *out_symbol
);

/* ------------------------------------------------------------------------- */
/* Bytecode emission and patching                                             */
/* ------------------------------------------------------------------------- */

ScInstrIndex sc_current_ip(const ScCompiler *compiler);

ScCompileResult sc_emit_instruction(
    ScCompiler *compiler,
    ScVmOpCode op,
    ScJumpHint hint,
    uint16_t a,
    uint32_t b,
    ScInstrIndex *out_index
);

ScCompileResult sc_emit_const(
    ScCompiler *compiler,
    ScValue value
);

ScCompileResult sc_emit_load(
    ScCompiler *compiler,
    uint16_t local_slot
);

ScCompileResult sc_emit_store(
    ScCompiler *compiler,
    uint16_t local_slot
);

ScCompileResult sc_emit_call(
    ScCompiler *compiler,
    ScSymbolId symbol_id,
    uint16_t argc
);

ScCompileResult sc_emit_jump_placeholder(
    ScCompiler *compiler,
    ScVmOpCode op,
    ScJumpHint hint,
    ScInstrIndex *out_instruction
);

ScCompileResult sc_patch_jump(
    ScCompiler *compiler,
    ScInstrIndex instruction,
    ScInstrIndex target
);

ScCompileResult sc_emit_pop(ScCompiler *compiler);
ScCompileResult sc_emit_ret(ScCompiler *compiler);

/* Tracks stack effects and calculates ScProc.max_stack. */
ScCompileResult sc_adjust_compile_stack(
    ScCompiler *compiler,
    int delta
);

/* Loop and break patch support. */
ScCompileResult sc_push_loop(
    ScCompiler *compiler,
    ScInstrIndex start_ip
);

ScCompileResult sc_record_break(
    ScCompiler *compiler,
    ScInstrIndex jump_instruction
);

ScCompileResult sc_finish_loop(
    ScCompiler *compiler,
    ScInstrIndex end_ip
);

/* ------------------------------------------------------------------------- */
/* Constant creation                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Validate/canonicalise literal through BCL, intern it as a permanent slot,
 * and return its ScValue. The implementation may deduplicate constants.
 */
ScCompileResult sc_intern_constant(
    ScCompiler *compiler,
    ScSlice literal,
    ScValue *out_value
);

/* ------------------------------------------------------------------------- */
/* Compiler error helpers                                                     */
/* ------------------------------------------------------------------------- */

ScCompileResult sc_compile_error(
    ScCompiler *compiler,
    ScCompileResult code,
    const char *message
);

ScCompileResult sc_compile_error_at(
    ScCompiler *compiler,
    size_t source_pos,
    uint32_t line,
    uint32_t column,
    ScCompileResult code,
    const char *message
);

/* ------------------------------------------------------------------------- */
/* Minimal VM public API                                                      */
/* ------------------------------------------------------------------------- */

ScVmResult sc_vm_init(
    ScVm *vm,
    const ScSecurityLimits *limits
);

void sc_vm_destroy(ScVm *vm);
/* Accept ASCII letters or one of # @ % ! ? : ^ ~. Default is '#'. */
ScVmResult sc_vm_set_numeric_text_prefix(ScVm *vm, char prefix);
char sc_vm_get_numeric_text_prefix(const ScVm *vm);

/*
 * Compile embedded standard libraries into vm in dependency order. Loading is
 * monotonic and idempotent: requesting an already-loaded or lower level is a
 * successful no-op. The build-time SC_EMBEDDED_LIBRARY_LEVEL macro controls
 * the highest level physically included in the binary. Core builtins must be
 * registered before loading libraries.
 */
ScCompileResult sc_vm_load_libraries(
    ScVm *vm,
    ScLibraryLevel level,
    ScCompileError *out_error
);

ScLibraryLevel sc_vm_loaded_library_level(const ScVm *vm);
ScLibraryLevel sc_vm_embedded_library_level(void);

const ScVmError *sc_vm_error(const ScVm *vm);
void sc_vm_clear_error(ScVm *vm);
bool sc_vm_error_is_fatal(ScVmResult code);
bool sc_compile_error_is_fatal(ScCompileResult code);
ScVmHealth sc_vm_health(const ScVm *vm);
bool sc_vm_is_usable(const ScVm *vm);
ScVmResult sc_vm_reset_after_error(ScVm *vm);


/* Register fixed-arity functions. */
ScVmResult sc_vm_register_cfunc(
    ScVm *vm,
    const char *name,
    uint16_t argc,
    ScCFunc function,
    ScSymbolId *out_symbol
);

ScVmResult sc_vm_declare_proc(
    ScVm *vm,
    const char *name,
    uint16_t argc,
    ScSymbolId *out_symbol
);

ScVmResult sc_vm_install_proc(
    ScVm *vm,
    ScSymbolId symbol_id,
    ScProc *proc,
    ScProcId *out_proc_id
);

/*
 * Invoke a symbol from C/SQLite. Arguments and result are ScValue handles.
 * The VM borrows args for the call and returns an owned result reference.
 */
ScVmResult sc_vm_call(
    ScVm *vm,
    ScSymbolId symbol_id,
    const ScValue *args,
    uint16_t argc,
    ScValue *out_result
);

ScVmResult sc_vm_call_slice(
    ScVm *vm,
    ScSlice name,
    const ScValue *args,
    uint16_t argc,
    ScValue *out_result
);

ScVmResult sc_vm_call_name(
    ScVm *vm,
    const char *name,
    size_t name_len,
    const ScValue *args,
    uint16_t argc,
    ScValue *out_result
);

/* Execute until the frame stack becomes empty or an error occurs. */
ScVmResult sc_vm_run(
    ScVm *vm,
    ScValue *out_result
);

/* Execute one bytecode instruction. Primarily useful for tests/debugging. */
ScVmResult sc_vm_step(ScVm *vm);

/* ------------------------------------------------------------------------- */
/* Minimal VM internals                                                       */
/* ------------------------------------------------------------------------- */

/* Frame management. */
ScVmResult sc_vm_push_frame(
    ScVm *vm,
    ScProc *proc,
    const ScValue *args,
    uint16_t argc
);

ScVmResult sc_vm_pop_frame(
    ScVm *vm,
    ScValue result
);

ScFrame *sc_vm_current_frame(ScVm *vm);
const ScFrame *sc_vm_current_frame_const(const ScVm *vm);

/* Operand stack. */
ScVmResult sc_vm_push_value(ScVm *vm, ScValue value);
ScVmResult sc_vm_pop_value(ScVm *vm, ScValue *out_value);
ScVmResult sc_vm_peek_value(const ScVm *vm, ScValue *out_value);

/* Local slots. */
ScVmResult sc_vm_load_local(
    ScVm *vm,
    uint16_t local_slot,
    ScValue *out_value
);

ScVmResult sc_vm_store_local(
    ScVm *vm,
    uint16_t local_slot,
    ScValue value
);

/* CALL dispatch. */
ScVmResult sc_vm_dispatch_call(
    ScVm *vm,
    ScSymbolId symbol_id,
    uint16_t argc
);

/* Truth testing used by JZ/JNZ. */
ScVmResult sc_vm_value_is_zero(
    ScVm *vm,
    ScValue value,
    bool *out_is_zero
);

/* ------------------------------------------------------------------------- */
/* Slot/value ownership                                                       */
/* ------------------------------------------------------------------------- */

/* Allocate a transient slot wrapping num with refcount 1. */
ScVmResult sc_vm_new_value(
    ScVm *vm,
    BclNumber num,
    ScValue *out_value
);

/* Allocate a permanent slot (refcnt == -1), for constants/zero/one. */
ScVmResult sc_vm_new_permanent_value(
    ScVm *vm,
    BclNumber num,
    ScValue *out_value
);

ScVmResult sc_vm_retain_value(ScVm *vm, ScValue value);
ScVmResult sc_vm_release_value(ScVm *vm, ScValue value);

bool sc_vm_value_valid(const ScVm *vm, ScValue value);
BclNumber sc_vm_value_number(const ScVm *vm, ScValue value);


#if SC_ENABLE_PERSISTENT_STATE
ScVmResult sc_persistent_state_init(ScVm *vm, ScPersistentState *state, uint16_t count);
void sc_persistent_state_destroy(ScVm *vm, ScPersistentState *state);
ScVmResult sc_vm_state_begin(ScVm *vm, ScPersistentState *state, ScStateAccess access);
ScVmResult sc_vm_state_commit(ScVm *vm);
void sc_vm_state_rollback(ScVm *vm);
bool sc_vm_state_active(const ScVm *vm);
uint16_t sc_vm_state_count(const ScVm *vm);
ScVmResult sc_vm_state_get(ScVm *vm, uint16_t index, ScValue *out_value);
ScVmResult sc_vm_state_set(ScVm *vm, uint16_t index, ScValue value);
#endif

/* ------------------------------------------------------------------------- */
/* External value conversion                                                  */
/* ------------------------------------------------------------------------- */

typedef enum ScExternalValueType {
    SC_EXTERNAL_INTEGER = 1,
    SC_EXTERNAL_TEXT
} ScExternalValueType;

typedef struct ScExternalValue {
    ScExternalValueType type;
    union {
        int64_t integer;
        char *text;
    } value;
} ScExternalValue;

/*
 * Convert a VM value to a newly allocated decimal string.  The conversion is
 * performed in vm's BCL context.  On success, the caller owns *out_text and
 * must release it with free().
 */
ScVmResult sc_vm_value_to_string(
    ScVm *vm,
    ScValue value,
    char **out_text
);

/*
 * Export an exact scale-zero value as a signed 64-bit integer when possible;
 * otherwise export its exact decimal representation as caller-owned text.
 * On SC_EXTERNAL_TEXT, the receiver must call free(result->value.text).
 */
ScVmResult sc_vm_export_value(
    ScVm *vm,
    ScValue value,
    ScExternalValue *result
);

/*
 * Import exact external numeric values into owned VM value handles.
 * Integer import covers the full signed 64-bit range without a text round-trip.
 * Numeric text must be a length-delimited decimal using the VM's configured
 * prefix, which defaults to '#'. A zero prefix accepts unprefixed decimal text.
 * Embedded NUL bytes and non-decimal syntax are rejected.
 */
ScVmResult sc_vm_import_int64(
    ScVm *vm,
    int64_t integer,
    ScValue *out_value
);

ScVmResult sc_vm_import_numeric_text(
    ScVm *vm,
    const char *text,
    size_t text_len,
    ScValue *out_value
);

/* ------------------------------------------------------------------------- */
/* C builtin argument/result helpers                                          */
/* ------------------------------------------------------------------------- */

uint16_t sc_vm_argc(const ScVm *vm);

ScVmResult sc_vm_arg(
    const ScVm *vm,
    uint16_t index,
    ScValue *out_value
);

ScVmResult sc_vm_set_result(
    ScVm *vm,
    ScValue value
);

/* ------------------------------------------------------------------------- */
/* Symbol table                                                               */
/* ------------------------------------------------------------------------- */

ScVmResult sc_symbol_table_init(
    ScSymbolTable *table,
    uint32_t bucket_count
);

void sc_symbol_table_destroy(ScSymbolTable *table);

ScVmResult sc_symbol_lookup(
    const ScSymbolTable *table,
    ScSlice name,
    ScSymbolId *out_symbol
);

ScVmResult sc_symbol_add(
    ScSymbolTable *table,
    ScSlice name,
    uint16_t argc,
    ScSymbolKind kind,
    ScSymbolId *out_symbol
);

/* ------------------------------------------------------------------------- */
/* Procedure cleanup and optional verification/disassembly                    */
/* ------------------------------------------------------------------------- */

void sc_proc_destroy(ScProc *proc);

ScCompileResult sc_verify_proc(
    const ScVm *vm,
    const ScProc *proc,
    ScCompileError *out_error
);

/*
 * Callback-based disassembly avoids imposing a FILE* or dynamic-string type.
 */
typedef bool (*ScDisasmWriteFn)(
    void *context,
    const char *text,
    size_t text_len
);

ScVmResult sc_disassemble_proc(
    const ScVm *vm,
    const ScProc *proc,
    ScDisasmWriteFn write_fn,
    void *write_context
);

/* True only when the current runtime error identifies installed bytecode. */
bool sc_vm_error_has_disassembly(const ScVm *vm);

/* Disassemble only the procedure implicated by the current runtime error. */
ScVmResult sc_vm_disassemble_error(
    const ScVm *vm,
    ScDisasmWriteFn write_fn,
    void *write_context
);

/* Register get_scale, set_scale, add, sub, mul and gt. */
ScVmResult sc_vm_register_core_builtins(ScVm *vm);

#ifdef __cplusplus
}
#endif

#endif /* SC_COMPILER_H */
