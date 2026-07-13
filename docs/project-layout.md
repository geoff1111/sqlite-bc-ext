# Project layout

The project intentionally uses a shallow layout. Compiler, VM, SQLite adapter, and public headers are all at the root so a reader can inspect the complete implementation without navigating separate source/include/extension trees.

`tests/` is also flat: unit tests, SQLite adapter tests, fuzz harnesses, and generated vector includes live together. `tools/` contains only C helpers. Runtime BC-Lite library sources remain grouped under `lib/`, and the JimTcl tutorial remains under `tutorial/`.
