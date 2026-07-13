CC ?= cc
VALGRIND ?= valgrind
ROOT ?= ..
BC_DIR ?= $(ROOT)/bc
JIMTCL_DIR ?= $(ROOT)/jimtcl
JSQLITE_DIR ?= $(ROOT)/jsqlite
BUILD_DIR ?= build
BC_BUILD ?= $(BUILD_DIR)/bcl
BC_PIC_BUILD ?= $(BUILD_DIR)/bcl-pic
BC_LIB := $(BC_BUILD)/bin/libbcl.a
BC_PIC_LIB := $(BC_PIC_BUILD)/bin/libbcl.a
JIMSH ?= $(JIMTCL_DIR)/jimsh
JSQLITE_SO ?= $(JSQLITE_DIR)/jsqlite.so
LONG_BIT ?= $(shell getconf LONG_BIT 2>/dev/null || echo 64)
EMBED_LEVEL ?= 3
CPPFLAGS += -I. -Itests -I$(BC_DIR)/include -DLONG_BIT=$(LONG_BIT) -DBC_LONG_BIT=$(LONG_BIT) -DSC_EMBEDDED_LIBRARY_LEVEL=$(EMBED_LEVEL)
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDLIBS := -lm -pthread
CORE := compiler.c embedded_libraries.c
HEADERS := compiler.h embedded_libraries.h
LIBS := lib/lib.bc-lite lib/lib2.bc-lite lib/lib3.bc-lite
GEN := $(BUILD_DIR)/generate_embedded_libraries
CORPUS_GEN := $(BUILD_DIR)/make_fuzz_corpus
FUZZ_MUTATE := $(BUILD_DIR)/fuzz_mutate
TEST := $(BUILD_DIR)/test_compiler
SQL_TEST := $(BUILD_DIR)/test_sc_sqlite
EXT := $(BUILD_DIR)/scbclite.so
LOAD_TEST := $(BUILD_DIR)/test_loadable
FUZZ := $(BUILD_DIR)/fuzz_runner

.PHONY: all test test-core test-sqlite test-loadable tutorial tutorial-test libraries embedded-level-tests sanitizers asan ubsan valgrind fuzz fuzz-smoke clean dependencies jimtcl jsqlite
all: libraries $(TEST) $(SQL_TEST) $(EXT)
$(BUILD_DIR):
	mkdir -p $@

$(GEN): tools/generate_embedded_libraries.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

libraries: $(GEN) $(LIBS)
	$(GEN) lib embedded_libraries.h embedded_libraries.c

$(BC_LIB):
	@test -x "$(BC_DIR)/configure.sh" || { echo "bc not found at $(BC_DIR)" >&2; exit 1; }
	mkdir -p $(BC_BUILD)
	cd $(BC_BUILD) && $(abspath $(BC_DIR))/configure.sh -a -O2
	$(MAKE) -C $(BC_BUILD)
$(BC_PIC_LIB):
	@test -x "$(BC_DIR)/configure.sh" || { echo "bc not found at $(BC_DIR)" >&2; exit 1; }
	mkdir -p $(BC_PIC_BUILD)
	cd $(BC_PIC_BUILD) && CFLAGS="-O2 -fPIC" $(abspath $(BC_DIR))/configure.sh -a -O2
	$(MAKE) -C $(BC_PIC_BUILD)

$(TEST): $(CORE) $(HEADERS) tests/test_compiler.c tests/math_vectors.inc tests/lib2_vectors.inc tests/lib3_vectors.inc $(BC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -DSC_TESTING $(CFLAGS) $(CORE) tests/test_compiler.c $(BC_LIB) $(LDLIBS) -o $@
$(SQL_TEST): $(CORE) sc_sqlite.c $(HEADERS) sc_sqlite.h tests/test_sc_sqlite.c $(BC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -DSQLITE_CORE $(CFLAGS) $(CORE) sc_sqlite.c tests/test_sc_sqlite.c $(BC_LIB) -lsqlite3 $(LDLIBS) -o $@
$(EXT): $(CORE) sc_sqlite.c $(HEADERS) sc_sqlite.h $(BC_PIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared -fvisibility=hidden -DSC_SQLITE_DEFAULT_ENTRYPOINT $(CORE) sc_sqlite.c $(BC_PIC_LIB) -lsqlite3 $(LDLIBS) -o $@
$(LOAD_TEST): tools/test_loadable.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -lsqlite3 -o $@

test-core: $(TEST)
	./$(TEST)
test-sqlite: $(SQL_TEST)
	./$(SQL_TEST)
test-loadable: $(EXT) $(LOAD_TEST)
	./$(LOAD_TEST) $(abspath $(EXT))
test: test-core test-sqlite test-loadable embedded-level-tests

embedded-level-tests: $(BC_LIB) libraries | $(BUILD_DIR)
	@set -e; for level in 0 1 2 3; do \
	  bin=$(BUILD_DIR)/test_library_level_$$level; \
	  $(CC) -I. -I$(BC_DIR)/include -DLONG_BIT=$(LONG_BIT) -DBC_LONG_BIT=$(LONG_BIT) -DSC_EMBEDDED_LIBRARY_LEVEL=$$level $(CFLAGS) $(CORE) tests/test_library_levels.c $(BC_LIB) $(LDLIBS) -o $$bin; \
	  ./$$bin $$level; \
	done

asan: $(BC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -DSC_TESTING $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address $(CORE) tests/test_compiler.c $(BC_LIB) $(LDLIBS) -o $(BUILD_DIR)/test_asan
	ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 ./$(BUILD_DIR)/test_asan
ubsan: $(BC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -DSC_TESTING $(CFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=undefined -fno-sanitize-recover=all $(CORE) tests/test_compiler.c $(BC_LIB) $(LDLIBS) -o $(BUILD_DIR)/test_ubsan
	UBSAN_OPTIONS=halt_on_error=1 ./$(BUILD_DIR)/test_ubsan
sanitizers: asan ubsan
valgrind: $(TEST)
	$(VALGRIND) --error-exitcode=99 --leak-check=full --show-leak-kinds=all ./$(TEST)

$(FUZZ): $(CORE) tests/fuzz_compiler.c tests/fuzz_runner.c $(BC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=undefined -fno-sanitize-recover=all $(CORE) tests/fuzz_compiler.c tests/fuzz_runner.c $(BC_LIB) $(LDLIBS) -o $@
$(CORPUS_GEN): tools/make_fuzz_corpus.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@
$(FUZZ_MUTATE): tools/fuzz_mutate.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@
$(BUILD_DIR)/fuzz-corpus: $(CORPUS_GEN)
	mkdir -p $@
	$(CORPUS_GEN) $@
fuzz-smoke: $(FUZZ) $(FUZZ_MUTATE) $(BUILD_DIR)/fuzz-corpus
	$(FUZZ_MUTATE) $(abspath $(FUZZ)) $(abspath $(BUILD_DIR)/fuzz-corpus) 1000 1
fuzz: $(FUZZ) $(FUZZ_MUTATE) $(BUILD_DIR)/fuzz-corpus
	$(FUZZ_MUTATE) $(abspath $(FUZZ)) $(abspath $(BUILD_DIR)/fuzz-corpus) 100000 1

jimtcl:
	@if [ ! -f $(JIMTCL_DIR)/Makefile ]; then cd $(JIMTCL_DIR) && ./configure; fi
	$(MAKE) -C $(JIMTCL_DIR)
jsqlite: jimtcl
	$(MAKE) -C $(JSQLITE_DIR) JIMDIR=$(abspath $(JIMTCL_DIR)) JIMSH=$(abspath $(JIMSH))
dependencies: $(BC_LIB) jimtcl jsqlite
tutorial: $(EXT) jsqlite
	$(JIMSH) tutorial/jsqlite_bclite_tutorial.tcl $(abspath $(JSQLITE_SO)) $(abspath $(EXT))
tutorial-test: $(EXT) jsqlite
	$(JIMSH) tutorial/jsqlite_bclite_tutorial.tcl --self-test $(abspath $(JSQLITE_SO)) $(abspath $(EXT))
clean:
	rm -rf $(BUILD_DIR)
