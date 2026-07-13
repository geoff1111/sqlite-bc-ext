#include "compiler.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    ScSecurityLimits limits;
    ScVm vm;
    ScCompiler c;
    char *src;
    uint32_t nproc = 0;
    uint32_t i;

    if (size == 0 || size > 65536u) return 0;
    src = (char *) malloc(size + 1u);
    if (src == NULL) return 0;
    memcpy(src, data, size);
    src[size] = '\0';

    sc_security_limits_default(&limits);
    limits.max_source_len = 65536u;
    limits.max_bytecode_bytes = 262144u;
    limits.max_procs = 128u;
    limits.max_symbols = 512u;
    limits.max_constants = 512u;
    limits.max_instructions = 10000u;
    limits.max_frames = 32u;

    if (sc_vm_init(&vm, &limits) != SC_VM_OK) {
        free(src);
        return 0;
    }
    if (sc_vm_register_core_builtins(&vm) != SC_VM_OK) {
        sc_vm_destroy(&vm);
        free(src);
        return 0;
    }

    sc_compiler_init(&c, &vm, src, size);
    if (sc_compile_source(&c, &nproc) == SC_COMPILE_OK) {
        for (i = 0; i < vm.proc_count; ++i) {
            ScProc *p = &vm.procs[i];
            ScValue out;
            if (p->argc == 0u && p->symbol_id < vm.symbols.count) {
                ScSymbol *s = &vm.symbols.symbols[p->symbol_id];
                if (s->name != NULL && s->name_len > 0u) {
                    if (sc_vm_call_name(&vm, s->name, s->name_len, NULL, 0, &out) == SC_VM_OK) {
                        (void) sc_vm_release_value(&vm, out);
                    }
                }
            }
        }
    }

    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    free(src);
    return 0;
}
