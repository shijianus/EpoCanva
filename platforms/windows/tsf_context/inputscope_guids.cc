// EpoCanva P3.9 -- the one translation unit that defines the InputScope GUIDs.
//
// inputscope.h declares GUID_PROP_INPUTSCOPE and the ITfInputScope IIDs with
// DEFINE_GUID, and unlike the msctf GUIDs they are not resolvable from uuid.lib, so a
// program that uses them needs exactly one object that defines them. INITGUID makes
// every DEFINE_GUID processed after it a definition; this file includes nothing else,
// so it defines only the inputscope.h set and cannot collide with the msctf GUIDs,
// which every other translation unit still gets from uuid.lib.
#include <windows.h>
#include <initguid.h>
#include <inputscope.h>
