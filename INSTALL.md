# Installing sqlite_bc_ext

## Quick start

### 1. Arrange the sibling projects

```text
work/
├── bc/
├── jimtcl/
├── jsqlite/
└── sqlite_bc_ext/
```

The directory names matter to the default Makefile. Alternatively, override
`BC_DIR`, `JIMTCL_DIR`, and `JSQLITE_DIR` when invoking `make`.

### 2. Install system prerequisites

On Debian/Ubuntu-like systems, the required tools are typically:

```sh
sudo apt install build-essential make sqlite3 libsqlite3-dev
```

For the optional validation targets:

```sh
sudo apt install valgrind
```

JimTcl and `jsqlite` are built from their sibling source trees by the supplied
Makefile, so system JimTcl packages are not required.

### 3. Build and test

```sh
cd work/sqlite_bc_ext
make test
```

The resulting extension is:

```text
build/scbclite.so
```

### 4. Try it in SQLite

```sh
sqlite3 :memory:
```

Then:

```sql
.load ./build/scbclite
select sc_bclite_load('proc twice {x} { return [mul $x #2] }');
select sc_bclite_register_all();
select twice(21), twice('#1.25');
```

### 5. Run the guided tutorial

```sh
make tutorial
```

Press Enter after each example. The tutorial prints both the JimTcl code and
its output. To run it unattended:

```sh
make tutorial-test
```

## Detailed dependency setup

### bc / libbcl

The project requires a `bc` source tree that provides:

```text
bc/configure.sh
bc/include/bcl.h
```

The Makefile creates two out-of-tree builds under `sqlite_bc_ext/build/`:

- `bcl/` for test executables;
- `bcl-pic/` for the shared SQLite extension.

You do not need to install `libbcl` system-wide.

### SQLite

Development headers and the link library are required. Confirm with:

```sh
pkg-config --cflags --libs sqlite3
```

The Makefile currently links with `-lsqlite3` directly.

### JimTcl and jsqlite

These are needed only for `make tutorial`, `make tutorial-test`, or an
application using that demonstration stack. The targets run approximately:

```sh
cd ../jimtcl && ./configure && make
make -C ../jsqlite JIMDIR=/absolute/path/to/jimtcl \
                     JIMSH=/absolute/path/to/jimtcl/jimsh
```

The expected products are:

```text
../jimtcl/jimsh
../jsqlite/jsqlite.so
```

## Non-default directory layout

For differently named or located dependencies:

```sh
make \
  BC_DIR=/src/bc-master \
  JIMTCL_DIR=/src/jimtcl-master \
  JSQLITE_DIR=/src/jsqlite \
  test
```

`ROOT` can replace all three when their names remain `bc`, `jimtcl`, and
`jsqlite`:

```sh
make ROOT=/src/dependencies test
```

## Build variants

### Embed fewer libraries

```sh
make clean
make EMBED_LEVEL=0 test   # no embedded library
make EMBED_LEVEL=1 test   # lib.bc-lite only
make EMBED_LEVEL=2 test   # levels 1 and 2
make EMBED_LEVEL=3 test   # all shipped libraries
```

The selected level is a compile-time maximum. A C application may request a
lower runtime level in `ScSqliteConfig.library_level`.

### Disable aggregate/window persistent state

For a scalar-only VM build:

```sh
make clean
make CPPFLAGS='-I. -Itests -I../bc/include \
  -DLONG_BIT=64 -DBC_LONG_BIT=64 \
  -DSC_EMBEDDED_LIBRARY_LEVEL=3 \
  -DSC_ENABLE_PERSISTENT_STATE=0' all
```

When replacing `CPPFLAGS`, preserve all include paths and required `bcl`
width definitions. Appending through an environment or local Makefile is less
error-prone.

### Change the state-slot maximum

```sh
make clean
make CPPFLAGS+=' -DSC_PERSISTENT_STATE_SLOTS=16' all
```

This only applies when persistent state is enabled. Rebuild all objects that
include `compiler.h`.

### Separate build directory

```sh
make BUILD_DIR=out test
```

The extension will then be `out/scbclite.so`.

### Debug build

```sh
make clean
make CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -O0 -g3' test
```

## Installing the shared extension

There is intentionally no privileged `make install` target. Copy the tested
shared object to an application-owned extension directory:

```sh
install -Dm755 build/scbclite.so \
  "$HOME/.local/lib/sqlite3/scbclite.so"
```

Load it using an absolute path or a path accepted by your SQLite host:

```sql
.load /home/USER/.local/lib/sqlite3/scbclite
```

Many SQLite hosts disable extension loading by policy. The host application
must call `sqlite3_enable_load_extension()` or an equivalent binding option
before `.load` or `sqlite3_load_extension()` can succeed.

For a server or sandboxed application, consider statically embedding the three
C implementation files and using the `sc_sqlite_*` API instead of permitting
arbitrary loadable extensions.

## Verification

Recommended full verification:

```sh
make clean
make test
make sanitizers
make valgrind
make fuzz-smoke
make tutorial-test
```

A longer fuzz run is:

```sh
make fuzz
```

Expected successful test executables print their passing summaries and return
status zero. Any compiler warning is treated as an error by the default flags.

## Troubleshooting

### `bc not found at ../bc`

Place the dependency at the expected sibling path or set `BC_DIR`:

```sh
make BC_DIR=/absolute/path/to/bc test
```

### `sqlite3.h: No such file or directory`

Install the SQLite development package, not only the `sqlite3` command-line
program.

### `cannot open shared object file` when loading

Use an absolute path and confirm the file exists:

```sh
ls -l "$(pwd)/build/scbclite.so"
sqlite3 :memory: ".load $(pwd)/build/scbclite"
```

Also inspect missing dynamic dependencies:

```sh
ldd build/scbclite.so
```

### SQLite reports that extension loading is disabled

Enable loadable extensions in the host program or use a SQLite shell build
that permits `.load`. This is a host security policy, not a BC-Lite compiler
error.

### Tutorial cannot find `jimsh` or `jsqlite.so`

Run:

```sh
make dependencies
```

Or supply paths explicitly:

```sh
make JIMSH=/path/to/jimsh JSQLITE_SO=/path/to/jsqlite.so tutorial
```

### Exact decimal input is rejected

Use an SQLite integer or prefixed text. With the default prefix:

```sql
select twice(12);          -- exact integer
select twice('#12.50');    -- exact decimal text
```

Do not pass an unprefixed decimal string or assume an SQLite `REAL` is exact.
Check the active prefix with:

```sql
select sc_bclite_prefix();
```

### Rebuilding after library edits

Run:

```sh
make libraries
make test
```

The generated `embedded_libraries.c` and `.h` are source artifacts and should
be kept consistent with files under `lib/`.
