/*
 * Flame — aggregation header, included at the bottom of quickjs.c.
 *
 * Flame is compiled as part of the QuickJS translation unit on purpose:
 * the baseline/optimizing emitters need byte-exact knowledge of JSValue,
 * JSObject, JSFunctionBytecode and the interpreter frame layout, and must
 * be able to call internal helpers (js_add_slow, JS_GetPropertyInternal,
 * build_backtrace, ...) directly. A single TU guarantees all of that with
 * zero duplication and zero ABI drift.
 *
 * Keep the include order: debug/rt first (they define shared state),
 * then profiler, ICs, emitters, then the tiers.
 */
#if CONFIG_FLAME

#include "flame_debug.c"
#include "flame_profiler.c"
#include "flame_ic.c"

/* flame_rt.c — runtime state, tiering policy, executable memory  (phase 4) */
/* emitter_x64.c / emitter_arm64.c — machine emitters             (phase 5/6) */
/* flame_baseline.c — baseline tier                               (phase 5) */
/* flame_opt.c — optimizing tier, OSR + deopt                     (phase 7/8) */

#endif /* CONFIG_FLAME */
