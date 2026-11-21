/* Aggregation: Flame sources are compiled inside the QuickJS translation unit. */
#if CONFIG_FLAME

#include "flame_debug.c"
#include "flame_profiler.c"
#include "flame_ic.c"
#include "flame_rt.c"

/* machine emitter: one backend per architecture */
#if defined(__aarch64__)
#include "emitter_arm64.c"
#else
#include "emitter_x64.c"
#endif

#include "flame_baseline.c"

/* flame_opt.c — optimizing tier, OSR + deopt (phase 7/8) */

#endif /* CONFIG_FLAME */
