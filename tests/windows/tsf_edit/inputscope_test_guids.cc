// EpoCanva P3.9 -- the one translation unit that defines the InputScope GUIDs for test
// executables that compile test_text_store.cc.
//
// test_text_store.cc compares TS_ATTRID values against GUID_PROP_INPUTSCOPE and the
// msctf GUID is not resolvable from uuid.lib. Each test executable that compiles the
// store from source compiles this file exactly once, so the symbol is defined once per
// program and never collides with the production definition inside
// epocanva_tsf_context (which those programs may also link).
#include <windows.h>
#include <initguid.h>
#include <inputscope.h>
