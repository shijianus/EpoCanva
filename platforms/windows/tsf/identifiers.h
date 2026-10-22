// EpoCanva P3.1 -- the identity constants of the Windows TSF front end.
//
// UNITS AND FORMAT (AGENTS.md section 5): a GUID is 16 bytes; the four fields below are
// its standard Win32 layout (Data1 one 32-bit word, Data2 and Data3 two 16-bit words,
// Data4 eight bytes). The braces form printed in docs/windows/tsf-bootstrap.md must keep
// matching this struct field for field.
//
// GENERATION (P3.1 brief, section 5): the value below was generated ONCE with
// PowerShell [guid]::NewGuid() on 2026-10-22 and is FROZEN in source. It is never
// regenerated per build: a text service whose CLSID changed on every compile could not
// be recognised by a host, a test or a future registration step, which is the entire
// reason the value is committed here rather than derived at build time.
//
// SCOPE: this file carries only identities. No COM call, no registry read, no network,
// no logging (AGENTS.md section 2) -- nothing here can observe user content.
#ifndef EPOCANVA_TSF_IDENTIFIERS_H_
#define EPOCANVA_TSF_IDENTIFIERS_H_

#include <guiddef.h>

namespace epocanva {
namespace tsf {

// CLSID of the EpoCanva text service.
// {B440F702-63E4-4D47-BCC4-074899ABA836}
extern const GUID kTextServiceClsid;

}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TSF_IDENTIFIERS_H_
