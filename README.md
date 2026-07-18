<img src="bc-lite-ext.png" alt="bc-lite-ext-logo" width="25%">

# sqlite_bc_ext

`sqlite_bc_ext` is a Unix/Linux SQLite loadable extension that embeds a small,
direct compiler and virtual machine for **BC-Lite**, an exact-decimal language
utilizing the `bcl` numeric library from Gavin Howard's `bc` project.

It lets an application define exact numeric procedures in BC-Lite and expose
selected user procedures to SQLite as:

- scalar SQL functions;
- aggregate SQL functions; and
- true SQLite window functions with inverse callbacks.

The project also provides a public C API for applications that want to embed
the compiler, VM, or SQLite adapter directly, plus an interactive JimTcl and
`jsqlite` tutorial.

This extension was written with assistance from LLM's.

## Quick start

The default Makefile expects four sibling directories:

```text
work/
├── bc/
├── jimtcl/
├── jsqlite/
└── sqlite_bc_ext/
```

From `sqlite_bc_ext`:

```sh
make test
make tutorial
```

`make test` builds `bc`'s `libbcl`, the compiler/VM tests, SQLite adapter tests,
the loadable extension, and loadability tests. `make tutorial` additionally
builds JimTcl and `jsqlite`, then starts the interactive tutorial. See
[INSTALL.md](INSTALL.md) for dependency setup and installation details.

The extension is produced as:

```text
build/scbclite.so
```

A minimal SQLite session is:

```sql
$ sqlite3
sqlite> .load ./build/scbclite
sqlite> select sc_bclite_load('
   ...> proc twice {x} {
   ...>   return [mul $x #2]
   ...> }
   ...> ');
1
sqlite> select sc_bclite_register_all();
1
sqlite> select twice(21);
42
sqlite> select twice('#1.25');
#2.50
sqlite> .exit
```

Another example SQLite session calculating π to 100 decimal places is:

```sql
$ sqlite3
sqlite> .load ./build/scbclite
sqlite> select sc_bclite_load('
   ...> proc pi_scale {x} {
   ...>   return [pi $x]
   ...> }
   ...> ');
1
sqlite> select sc_bclite_register_all();
1
sqlite> select pi_scale(100);
#3.1415926535897932384626433832795028841971693993751058209749445923078164062862089986280348253421170679
sqlite> .exit
```

## What BC-Lite is

BC-Lite is a deliberately small procedure language. It compiles directly to
bytecode for the included stack VM. Runtime values are exact `BclNumber`
objects; strings are source syntax or external representations, not VM values.

### Special forms

```text
proc name {arg ...} { body }
set variable value
if condition { then-body } { else-body }
if condition { then-body }
loop { body }
break
return value
```

All other commands are fixed-arity calls to a builtin or compiled procedure.
Expressions use Tcl-like command substitution:

```tcl
proc hypotenuse_squared {a b} {
    return [add [mul $a $a] [mul $b $b]]
}
```

Variables are referenced with `$name`. Numeric literals must use the configured
prefix, `#` by default:

```tcl
set x #12.50
return [div $x #4]
```

The prefix prevents JimTcl, SQLite, or host-language conversion from silently
turning an exact decimal into a binary floating-point value. Valid prefixes are
restricted by `sc_numeric_prefix_valid()`; the shipped default is `#`, and the
tutorial also demonstrates `@`.

### Procedures and name resolution

A source string can contain multiple procedures. Calls may refer forward to a
procedure defined later in the same compilation unit. Compilation fails if a
source procedure remains undefined when that source finishes compiling.

Local helper procedure names are made unique internally. User procedures are
the only procedures eligible for automatic SQLite registration. Builtins and
embedded-library procedures remain private implementation details, although
user procedures may call them.

### Numeric builtins

The VM registers arithmetic, comparison, scale-management, random-number, and
conversion-related builtins supplied by the compiler runtime. The definitive
list and arities are in `compiler.c`. Common examples include:

```text
add sub mul div mod pow
neg abs sqrt
cmp eq lt le gt ge
get_scale set_scale
state_get state_set
```

`state_get` and `state_set` exist only when persistent state support is compiled
in and are valid only during aggregate/window callbacks.

## Compiler and VM architecture

`compiler.c` and `compiler.h` contain both the source compiler and VM.

The compiler:

1. reads a bounded `ScSlice` rather than requiring NUL-terminated input;
2. parses procedures and special forms;
3. interns exact numeric constants;
4. resolves builtin and procedure symbols;
5. emits compact bytecode; and
6. installs complete procedures into an `ScVm`.

The VM owns the `BclContext`, symbols, procedures, constants, value slots,
frames, operand stack, and optional persistent-state transaction. `ScValue` is
an index into VM-managed numeric slots, not a raw `BclNumber` pointer.

Reference-counted value lifetime is managed by the VM. Public retain/release
operations are available for C integrations that keep values across calls.

### Recoverable and fatal errors

The VM distinguishes:

- `SC_VM_HEALTH_READY`;
- `SC_VM_HEALTH_RECOVERABLE_ERROR`; and
- `SC_VM_HEALTH_FATAL_ERROR`.

A recoverable runtime error can be cleared with `sc_vm_reset_after_error()`;
the operation unwinds frames and restores a usable execution state. Fatal
errors make the VM unusable and require teardown. Compiler errors reject the
source being compiled and provide a code, byte offset, line, column, and
message.

### Security limits

`ScSecurityLimits` bounds untrusted source and execution:

- source length and generated bytecode;
- procedures, symbols, and constants;
- arguments, locals, stack depth, frame depth, loop depth, and parse depth;
- total instructions executed by a call.

Call `sc_security_limits_default()` and then lower selected fields before
initialising the VM or SQLite adapter. Compile-time hard maxima are declared at
the top of `compiler.h`.

## Embedded BC-Lite libraries

The library files are:

```text
lib/lib.bc-lite
lib/lib2.bc-lite
lib/lib3.bc-lite
```

They are converted at build time to C byte strings in
`embedded_libraries.c`. No Python interpreter or script is used.

Library loading is cumulative:

| Level | Loaded source |
|---:|---|
| 0 | no library |
| 1 | `lib.bc-lite` |
| 2 | levels 1 and 2 |
| 3 | levels 1, 2, and 3 |

The first libraries provide translated `bc` mathematical functions; level 3
adds decimal rounding modes. A build can embed fewer levels, and each context
can choose any available level at runtime. Library procedures may be called by
user source but cannot themselves be registered as SQL functions.

After editing library source, regenerate the embedded C files with:

```sh
make libraries
```

## SQLite extension behaviour

Loading `build/scbclite.so` installs one adapter context on the SQLite
connection and registers these administrative SQL functions:

| SQL function | Purpose |
|---|---|
| `sc_bclite_load(source)` | Compile user BC-Lite source into the connection context. |
| `sc_bclite_register_all()` | Register every eligible user procedure under its procedure name. |
| `sc_bclite_register_scalar(sql_name, proc_name, argc)` | Register one user procedure under an explicit SQL name. |
| `sc_bclite_register_aggregate(sql_name, argc, state_count, step_proc, final_proc)` | Register an aggregate. |
| `sc_bclite_register_window(sql_name, argc, state_count, step_proc, inverse_proc, value_proc, final_proc)` | Register a true window function. |
| `sc_bclite_prefix()` | Return the current exact-number text prefix. |
| `sc_bclite_prefix(prefix)` | Change the prefix used for subsequent imports, exports, and source compilation. |

Administrative functions are intended for connection setup. Applications that
need stricter control should use the C API and apply SQLite authorisation or
`DIRECTONLY` policies appropriate to their environment.

### SQLite input types

The default extension configuration uses strict exact input:

- `INTEGER` is imported exactly;
- prefixed `TEXT`, such as `#123.45`, is imported exactly;
- unsupported or malformed values cause an error;
- SQLite binary floating-point values are not silently accepted as exact.

The C API additionally offers `SC_SQLITE_INPUT_SQLITE_NUMERIC` for integrations
that explicitly choose SQLite numeric coercion behaviour.

### SQLite output types

The default output policy is
`SC_SQLITE_OUTPUT_INT64_OR_PREFIXED_TEXT`:

- an exact integer fitting signed SQLite `int64` is returned as `INTEGER`;
- all other exact values are returned as prefixed `TEXT`.

`SC_SQLITE_OUTPUT_ALWAYS_PREFIXED_TEXT` forces every result to prefixed text.
The receiver owns any heap string produced by the lower-level external-value
conversion API, as documented in `compiler.h`.

### NULL policies

The C adapter can be configured to:

- reject NULL (`SC_SQLITE_NULL_ERROR`);
- propagate NULL (`SC_SQLITE_NULL_PROPAGATE`); or
- import NULL as exact zero (`SC_SQLITE_NULL_AS_ZERO`).

### Function names and flags

The C API supports exact SQL names or an application-defined name prefix.
Functions can be marked with supported combinations of:

- `SC_SQLITE_FUNC_DETERMINISTIC`;
- `SC_SQLITE_FUNC_DIRECTONLY`; and
- `SC_SQLITE_FUNC_INNOCUOUS`.

Registration can reject duplicates or replace an existing registration using
`SC_SQLITE_REGISTER_REPLACE_EXISTING`.

## Aggregates and window functions

Each aggregate instance has a fixed number of exact numeric state slots. Slots
are zero-initialised and indexed from zero.

```tcl
proc sum_step {x} {
    state_set #0 [add [state_get #0] $x]
    return [state_get #0]
}

proc sum_final {} {
    return [state_get #0]
}
```

Register it from SQL:

```sql
select sc_bclite_register_aggregate(
    'bc_sum', 1, 1, 'sum_step', 'sum_final'
);
```

For a window function, provide an inverse callback:

```tcl
proc sum_inverse {x} {
    state_set #0 [sub [state_get #0] $x]
    return [state_get #0]
}
```

```sql
select sc_bclite_register_window(
    'bc_window_sum', 1, 1,
    'sum_step', 'sum_inverse', 'sum_final', 'sum_final'
);
```

State changes made by `step` or `inverse` are transactional: they commit only
when the callback succeeds. State is read-only during a value callback. The
number of slots is bounded by `SC_PERSISTENT_STATE_SLOTS`, 10 by default.

## Public C APIs

### Compiler/VM API

Include `compiler.h`. The normal embedding sequence is:

1. initialise security limits;
2. call `sc_vm_init()`;
3. optionally call `sc_vm_load_libraries()`;
4. initialise an `ScCompiler` over source;
5. call `sc_compile_source()`;
6. destroy the compiler; and
7. call user procedures through the VM, then `sc_vm_destroy()`.

The header also exposes lower-level compiler and VM operations for tests,
custom front ends, and adapters. Applications should prefer the high-level
initialisation, source-compilation, and call operations unless they need that
control.

### SQLite adapter API

Include `sc_sqlite.h` and link `compiler.c`, `embedded_libraries.c`,
`sc_sqlite.c`, `libbcl`, SQLite, `libm`, and pthreads.

Typical sequence:

```c
ScSqliteConfig config;
ScSqlite *context = NULL;

sc_sqlite_config_init(&config);
config.library_level = SC_SQLITE_LIBRARY_1_2_3;

if (sc_sqlite_create(db, &config, &context) != SC_SQLITE_OK) {
    /* inspect sc_sqlite_error(context) when context is available */
}

/* sc_sqlite_load_source(), then register functions */

sc_sqlite_destroy(context);
```

Every versioned input structure must have `struct_size` set correctly; the
initialiser does this for `ScSqliteConfig`. Set `abi_version` to
`SC_SQLITE_ABI_VERSION` when constructing configuration manually.

`ScSqliteError` carries the adapter result, SQLite result, compiler result, VM
result, source position, line, column, and a human-readable message.

## Build configuration

### Make variables

Pass variables on the `make` command line, for example:

```sh
make EMBED_LEVEL=2 BUILD_DIR=out test
```

| Variable | Default | Meaning |
|---|---|---|
| `CC` | `cc` | C compiler. |
| `CFLAGS` | strict C11, warnings-as-errors, `-O2` | Compiler flags. |
| `CPPFLAGS` | generated defaults | Preprocessor/include flags; append rather than replacing when possible. |
| `ROOT` | `..` | Parent containing sibling projects. |
| `BC_DIR` | `$(ROOT)/bc` | `bc` source directory. |
| `JIMTCL_DIR` | `$(ROOT)/jimtcl` | JimTcl source directory. |
| `JSQLITE_DIR` | `$(ROOT)/jsqlite` | `jsqlite` source directory. |
| `BUILD_DIR` | `build` | All local generated binaries and objects. |
| `BC_BUILD` | `$(BUILD_DIR)/bcl` | Non-PIC `libbcl` build tree. |
| `BC_PIC_BUILD` | `$(BUILD_DIR)/bcl-pic` | PIC `libbcl` build tree. |
| `JIMSH` | `$(JIMTCL_DIR)/jimsh` | JimTcl executable used by the tutorial. |
| `JSQLITE_SO` | `$(JSQLITE_DIR)/jsqlite.so` | JimTcl SQLite module. |
| `LONG_BIT` | host `getconf LONG_BIT` | Integer-width setting passed to `bcl`. |
| `EMBED_LEVEL` | `3` | Highest library level compiled into the binary, 0–3. |
| `VALGRIND` | `valgrind` | Valgrind executable. |

### C preprocessor switches

| Switch | Default | Effect |
|---|---:|---|
| `SC_EMBEDDED_LIBRARY_LEVEL` | Makefile `EMBED_LEVEL` | Highest embedded library level available at runtime. |
| `SC_ENABLE_PERSISTENT_STATE` | `1` | Compile aggregate/window state support and `state_get`/`state_set`. Set to `0` for a scalar-only VM. |
| `SC_PERSISTENT_STATE_SLOTS` | `10` | Maximum slots in one persistent state object when state support is enabled. |
| `SC_SQLITE_DEFAULT_ENTRYPOINT` | extension target only | Exports the standard SQLite loadable-extension entry point wrapper. |
| `SC_SQLITE_STATIC` | unset | Suppresses GCC default-visibility decoration when statically embedding the adapter. |
| `SC_TESTING` | tests only | Enables deterministic allocation-failure hooks. Do not enable in production. |
| `SQLITE_CORE` | adapter unit test only | Builds against SQLite's direct API rather than loadable-extension API indirection. |
| `LONG_BIT`, `BC_LONG_BIT` | detected | Passed through for `bcl` platform configuration. |

The hard capacity and security maxima near the top of `compiler.h` are also
preprocessor constants, but changing them is an implementation build, not a
stable ABI promise. Rebuild every object that includes the header if modifying
them.

## Make targets

| Target | Action |
|---|---|
| `make all` | Build compiler tests, SQLite tests, and `build/scbclite.so`. |
| `make dependencies` | Build `libbcl`, JimTcl, and `jsqlite`. |
| `make libraries` | Regenerate embedded library C source using the native C helper. |
| `make test` | Run core, SQLite, loadability, and all embedded-level tests. |
| `make test-core` | Run compiler/VM and math-vector tests. |
| `make test-sqlite` | Run direct SQLite adapter tests. |
| `make test-loadable` | Dynamically load and exercise the `.so`. |
| `make embedded-level-tests` | Rebuild and test library levels 0 through 3. |
| `make sanitizers` | Run AddressSanitizer and UndefinedBehaviorSanitizer tests. |
| `make valgrind` | Run the core suite with full leak reporting. |
| `make fuzz-smoke` | Run 1,000 deterministic isolated-process mutations. |
| `make fuzz` | Run 100,000 mutations. |
| `make tutorial` | Run the interactive JimTcl tutorial. |
| `make tutorial-test` | Run the complete tutorial without pauses. |
| `make clean` | Remove the selected local build directory. |

## Tutorial

Run:

```sh
make tutorial
```

The tutorial:

- prints the exact JimTcl code for each example;
- prints the resulting SQL and output;
- pauses for Enter between examples;
- covers loading source, scalar registration, exact result types, embedded
  libraries, aggregates, windows, prefixes, bound Tcl values, errors, and
  privacy of library procedures.

For automated verification:

```sh
make tutorial-test
```

You may also invoke it directly:

```sh
../jimtcl/jimsh tutorial/jsqlite_bclite_tutorial.tcl \
    ../jsqlite/jsqlite.so build/scbclite.so
```

## Testing and production checks

The normal pre-release sequence is:

```sh
make clean
make test
make sanitizers
make valgrind
make fuzz-smoke
make tutorial-test
```

`make fuzz` provides the longer mutation run. AddressSanitizer leak detection is
disabled in the target because `bcl`/process-global teardown is checked more
reliably by the dedicated Valgrind target; ASan still checks invalid accesses.

The tests include exact vectors translated from the upstream `bc` libraries,
compiler success and failure paths, allocation failure injection, VM recovery,
persistent-state commit/rollback, SQLite type conversion, aggregate/window
registration, and loadable-extension behaviour.

## Project layout

```text
sqlite_bc_ext/
├── compiler.c, compiler.h                 compiler and VM
├── sc_sqlite.c, sc_sqlite.h               SQLite adapter/public API
├── embedded_libraries.c, .h               generated embedded source
├── lib/                                    BC-Lite library source
├── tests/                                  unit, vector, SQLite, fuzz tests
├── tools/                                  native C build/test helpers
├── tutorial/jsqlite_bclite_tutorial.tcl   interactive tutorial
├── docs/                                   supplementary notes
├── Makefile
├── README.md
└── INSTALL.md
```

The implementation intentionally keeps source and public headers at the project
root. The build and test process contains no Python scripts.

## Platform and linkage notes

The project targets Unix-like systems and builds an ELF-style shared extension
with `-fPIC -shared`. Windows-specific entry points and build support are not
provided.

The extension links against SQLite, `libbcl`, `libm`, and pthreads. Its SQLite
initialiser is `sqlite3_scbclite_init`; the default-entrypoint build also allows
SQLite's normal `.load` discovery path.

## License

A project LICENSE file is included in this source snapshot. Before redistributing the combined work, verify the licence obligations of SQLite, JimTcl, `jsqlite`, and the `bc`/`bcl`
dependency.
