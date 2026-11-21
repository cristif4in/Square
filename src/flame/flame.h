/* Public API for the Flame JIT. The interpreter is the semantic fallback. */
#ifndef SQUARE_FLAME_H
#define SQUARE_FLAME_H

#include "../../thirdparty/quickjs/quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Version banner, also reported by flame_version(). */
#define FLAME_VERSION "0.1.0"

/* Enable/disable the JIT for this runtime (default: enabled).
 * Returns the previous state. This only gates tiering; functions already
 * running JIT code finish their current activation before falling back. */
int flame_set_enabled(JSRuntime *rt, int enabled);

/* Current enable state (1 = JIT allowed). */
int flame_is_enabled(JSRuntime *rt);

/* Force-disable JIT process-wide (checked on every tier decision).
 * Used by the SQRL_NO_JIT environment kill-switch. */
void flame_set_global_enabled(int enabled);

/* Dump tiering / IC / deopt statistics to stderr. */
void flame_dump_stats(JSRuntime *rt);

/* Reset per-runtime statistics. */
void flame_reset_stats(JSRuntime *rt);

/* Version string (static storage). */
const char *flame_version(void);

#ifdef __cplusplus
}
#endif

#endif /* SQUARE_FLAME_H */
