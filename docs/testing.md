# Testing

`make test` runs the compiler/VM suite, SQLite adapter suite, native loadable-extension suite, and embedded-library-level tests.

`make sanitizers` runs the compiler/VM suite under AddressSanitizer and UndefinedBehaviorSanitizer.

`make fuzz-smoke` builds the C corpus generator and C mutation driver, then runs 1,000 deterministic isolated-process mutations. `make fuzz` raises this to 100,000 cases.

`make tutorial-test` builds JimTcl and jsqlite from sibling directories when needed, then executes the full tutorial non-interactively.
