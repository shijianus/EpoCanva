// EpoCanva P3.1 -- the frozen identity constants. See identifiers.h for the generation
// rule (once, 2026-10-22, frozen in source) and the field layout.
#include "identifiers.h"

namespace epocanva {
namespace tsf {

// {B440F702-63E4-4D47-BCC4-074899ABA836}
const GUID kTextServiceClsid = {
    0xB440F702,
    0x63E4,
    0x4D47,
    {0xBC, 0xC4, 0x07, 0x48, 0x99, 0xAB, 0xA8, 0x36},
};

// {7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}
const GUID kEnglishLanguageProfileGuid = {
    0x7D3E2A94,
    0x5C61,
    0x4F0B,
    {0x9E, 0x3A, 0x1B, 0x8D, 0x6C, 0x4F, 0x2A, 0x71},
};

}  // namespace tsf
}  // namespace epocanva
