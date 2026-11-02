/* This translation unit must compile as C11 (/TC), not as C++ with extern C.
 * Include the public header first and twice: no transitive platform headers.
 */
#include "epocanva_engine.h"
#include "epocanva_engine.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(epocanva_status) == 4, "status must be int32_t");
_Static_assert(sizeof(epocanva_engine_handle) == 8, "opaque token storage is 8 bytes");
_Static_assert(offsetof(epocanva_engine_options, abi_version) == 4, "version prefix");
_Static_assert(offsetof(epocanva_snapshot, snapshot_version) == 8, "snapshot prefix");

int main(int argc, char** argv) {
  epocanva_engine_options options = {0};
  epocanva_engine_handle engine = 77;
  epocanva_status status;
  const int control = argc == 2 && strcmp(argv[1], "control") == 0;
  if (epocanva_engine_abi_version() != EPOCANVA_ENGINE_ABI_VERSION) return 5;
  options.struct_size_bytes = (uint32_t)sizeof(options);
  /* Deliberately omit abi_version. Version zero must never infer v1. */
  status = epocanva_engine_open(&options, &engine);
  if (control) {
    if (status == EPOCANVA_OK) {
      puts("RESULT PASS"); /* Would falsify this deliberately wrong expectation. */
      return 0;
    }
    puts("FAIL expected omitted ABI version to be accepted (deliberate control)");
    return 14;
  }
  if (status != EPOCANVA_ABI_VERSION_MISMATCH || engine != 0) return 5;
  options.struct_size_bytes = 0;
  status = epocanva_engine_open(&options, &engine);
  if (status != EPOCANVA_STRUCT_TOO_SMALL) return 5;
  if (epocanva_session_close(0) != EPOCANVA_INVALID_HANDLE) return 5;
  if (strcmp(epocanva_status_name(EPOCANVA_PRIVACY_DENIED), "privacy-denied") != 0) return 5;
  puts("RESULT PASS pure-C ABI consumer");
  return 0;
}
