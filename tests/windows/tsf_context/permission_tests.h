// EpoCanva P3.9 -- case runners for the input-permission verification program.
#ifndef EPOCANVA_TESTS_WINDOWS_TSF_CONTEXT_PERMISSION_TESTS_H_
#define EPOCANVA_TESTS_WINDOWS_TSF_CONTEXT_PERMISSION_TESTS_H_

namespace epocanva {
namespace tsf {
namespace context {
namespace test {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitCheckFailed = 5;
constexpr int kExitSelfCheckFailed = 14;

int RunPureClassification();
int RunRealPathProbe();
int RunPlainEditableAllowed();
int RunReadOnlyDenied();
int RunKeyboardDisabledValues();
int RunEmptyContextDenied();
int RunSensitiveScopesDenied();
int RunScopeAbsentWrongTypeFail();
int RunClearedCompartmentRecreated();
int RunRepeatedQueriesConsistent();
int RunAsyncQueryPending();
int RunAbandonPendingQuery();
int RunResourceBalance();
int RunCanFail();

}  // namespace test
}  // namespace context
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_TSF_CONTEXT_PERMISSION_TESTS_H_
