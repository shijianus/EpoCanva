/* EpoCanva P1.4 -- diagnostics surface of the compiled-in probe component.
 *
 * This header is the ONLY thing a caller outside the engine DLL may include in
 * order to observe the probe. It is deliberately C-compatible and fixed-layout:
 *
 *   - no STL type crosses the boundary in either direction;
 *   - no function returns memory the caller must release;
 *   - the only pointers handed out are to static storage inside rime.dll
 *     (string literals), valid for as long as that module is loaded;
 *   - counters are copied into a caller-owned struct the caller allocated.
 *
 * Access path (all of it upstream machinery, no EpoCanva-specific export):
 *   RimeApi::find_module("epocanva_probe")   -> RimeModule*
 *   RimeModule::get_api()                    -> RimeCustomApi* , cast to
 *                                               EpoCanvaProbeApi*
 * There is no new exported symbol and no permanent EpoCanva ABI: the module's
 * get_api slot is the mechanism librime itself uses for the levers module
 * (third_party/librime/src/rime/lever/levers_module.cc:38-40,
 * src/rime/lever/levers_api_impl.h:242-280).
 *
 * A caller must not keep the RimeModule and EpoCanvaProbeApi pointers it was
 * given and use them after RimeApi::finalize() has returned: finalize() runs
 * UnloadModules() (src/rime_api_impl.h:58-63) and the engine may be torn down
 * afterwards. The counters themselves live in static storage of this module and
 * stay readable for the process lifetime, but obtaining them goes through a
 * table the engine hands out, so the rule is stated as "do not touch this
 * surface after finalize()" and the test program follows it.
 *
 * UNITS: every counter is a count of events, not a length or an offset.
 *
 * PRIVACY (AGENTS.md section 2): these counters record THAT the probe ran, never
 * WHAT the user typed. No key code, no input buffer, no composition and no
 * candidate text is stored anywhere in this component. The test key is a fixed
 * constant compiled into the component (EPOCANVA_PROBE_TEST_KEYCODE, defined
 * below), and the only per-key statistics kept are aggregate counts that do not
 * identify a key.
 */
#ifndef EPOCANVA_PROBE_DIAGNOSTICS_H_
#define EPOCANVA_PROBE_DIAGNOSTICS_H_

#include <rime_api.h> /* Bool, False, True, RimeCustomApi, RIME_STRUCT* */

#ifdef __cplusplus
extern "C" {
#endif

/* Module name as registered with rime's ModuleManager, and component name as
 * registered with rime's Registry. A schema lists the component name under
 * engine/processors; a front end lists the module name in RimeTraits.modules. */
#define EPOCANVA_PROBE_MODULE_NAME "epocanva_probe"
#define EPOCANVA_PROBE_PROCESSOR_NAME "epocanva_probe_processor"

/* The single key this component claims to own. XK_F6, value read out of the
 * pinned checkout: third_party/librime/include/X11/keysymdef.h:257
 *   #define XK_F6  0xffc3
 * (NOT 0xffbe -- that is XK_F1, at :252.) Modifier must be zero, i.e. a bare F6;
 * Shift+F6 and friends are deliberately left to the rest of the input chain. */
#define EPOCANVA_PROBE_TEST_KEYCODE 0xffc3

/* Which keys this component counts as "passed through" in
 * keys_passed_through. It is the count of ProcessKeyEvent() calls that the
 * component released to the next processor (kNoop); it does not classify the
 * key, so it cannot leak which key was pressed. The fixture drives exactly
 * three of them ('a', 'b', 'c'), which is what makes "the letters really went
 * through this component" an observable rather than an assumption. */

/* Fixed-layout snapshot of the probe counters. Caller-allocated; the engine
 * fills it. data_size follows librime's own versioning convention
 * (src/rime_api.h:60-61): RIME_STRUCT_INIT() records the payload size so a
 * newer struct read by an older component (or vice versa) is detectable. */
typedef struct epocanva_probe_counters_t {
  int data_size;

  /* Lifetime / reachability counters. */
  unsigned long long module_initialized;  /* rime_epocanva_probe_initialize() */
  unsigned long long module_finalized;    /* rime_epocanva_probe_finalize()  */

  /* Object counters. live_instances is maintained directly by the constructor
   * and destructor, so it is a real "currently alive" reading rather than an
   * arithmetic difference a caller has to trust. */
  unsigned long long component_constructed;
  unsigned long long component_destroyed;
  unsigned long long live_instances;

  /* Behaviour counters, incremented inside ProcessKeyEvent(). */
  unsigned long long process_key_calls;    /* total entries into the component */
  unsigned long long keys_passed_through;  /* returned kNoop */
  unsigned long long test_keys_consumed;   /* returned kAccepted for bare F6   */
} EpoCanvaProbeCounters;

/* Function table handed out by the probe module's get_api(). */
typedef struct epocanva_probe_api_t {
  int data_size; /* must stay first: it is RimeCustomApi's only member */

  /* Copies the counters into *out. Returns False if out is NULL or if out is
   * too small to hold every field this build writes (detected through
   * out->data_size); on False nothing is written. Never allocates. */
  Bool (*get_counters)(EpoCanvaProbeCounters* out);

  /* Zeroes every counter except module_initialized/module_finalized only when
   * no instance is alive. Returns False -- without touching anything -- when
   * live_instances != 0, so a caller cannot lose evidence mid-run. Returns True
   * when the reset happened. */
  Bool (*reset_counters_when_idle)(void);

  /* Pointer to a string literal inside rime.dll, e.g.
   * "epocanva-probe/0.0.0-p1.4". Read-only, never freed by the caller, valid
   * while the module is loaded. Lets a report tie the observed behaviour to the
   * exact component source that produced it. */
  const char* (*build_id)(void);
} EpoCanvaProbeApi;

#ifdef __cplusplus
}
#endif

#endif /* EPOCANVA_PROBE_DIAGNOSTICS_H_ */
