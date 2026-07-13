#include "compiler.h"

#include <assert.h>
#include <stdlib.h>

int
main (int argc, char **argv)
{
  ScVm vm;
  ScCompileError error;
  ScLibraryLevel built;
  int expected;
  int level;

  assert (argc == 2);
  expected = atoi (argv[1]);
  built = sc_vm_embedded_library_level ();
  assert ((int) built == expected);
  assert (sc_vm_init (&vm, NULL) == SC_VM_OK);
  assert (sc_vm_register_core_builtins (&vm) == SC_VM_OK);
  for (level = 0; level <= 3; ++level)
  {
    ScCompileResult result = sc_vm_load_libraries (
        &vm, (ScLibraryLevel) level, &error);
    if (level <= expected)
    {
      assert (result == SC_COMPILE_OK);
      assert ((int) sc_vm_loaded_library_level (&vm) == level);
    }
    else
    {
      assert (result == SC_ERR_COMPILE_LIBRARY_UNAVAILABLE);
      assert (error.code == SC_ERR_COMPILE_LIBRARY_UNAVAILABLE);
      assert ((int) sc_vm_loaded_library_level (&vm) == expected);
    }
  }
  sc_vm_destroy (&vm);
  return 0;
}
