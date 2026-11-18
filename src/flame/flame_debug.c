/*
 * Flame — debug logging, env parsing, statistics skeleton.
 *
 * FLAME_DEBUG drives verbosity:
 *   unset/0 : quiet
 *   1       : tiering events, compiles, deopts, IC transitions
 *   2       : + verbose codegen dumps
 *   trace   : + per-opcode interpreter trace
 *   stats   : dump stats at exit
 * Also honors SQRL_NO_JIT=1 as a process-wide kill-switch.
 */
#include "flame_internal.h"

int flame_debug_level = -1;  /* -1 = not yet parsed */
static int flame_stats_atexit_registered;
static JSRuntime *flame_stats_rt;

int flame_debug_init(void)
{
    if (flame_debug_level >= 0)
        return flame_debug_level;
    /* crash-proof logging: never buffer diagnostic streams */
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    flame_debug_level = 0;
    {
        const char *env = getenv("FLAME_DEBUG");
        if (env && *env) {
            if (strstr(env, "trace"))
                flame_debug_level = FLAME_LOG_TRACE;
            else
                flame_debug_level = atoi(env);
            if (strstr(env, "stats") && flame_debug_level < 1)
                flame_debug_level = 1;
            if (strstr(env, "stats") && !flame_stats_atexit_registered) {
                flame_stats_atexit_registered = 1;
                atexit(flame_dump_stats_atexit);
            }
            fprintf(stderr, "[flame %s] debug level %d\n",
                    FLAME_VERSION, flame_debug_level);
        }
    }
    return flame_debug_level;
}

void flame_dump_stats_atexit(void)
{
    if (flame_stats_rt)
        flame_dump_stats(flame_stats_rt);
}

void flame_log(int level, const char *fmt, ...)
{
    va_list ap;
    if (level > flame_debug_init())
        return;
    fputs("[flame] ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void flame_global_init(void)
{
    flame_debug_init();
    {
        const char *env = getenv("SQRL_NO_JIT");
        if (env && *env && strcmp(env, "0") != 0) {
            flame_set_global_enabled(0);
            flame_log(FLAME_LOG_TIER,
                      "JIT disabled process-wide by SQRL_NO_JIT=%s", env);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Stats teardown hook (impl in flame_rt.c owns counters). */
void flame_set_stats_rt(JSRuntime *rt)
{
    flame_stats_rt = rt;
}

const char *flame_version(void)
{
    return FLAME_VERSION;
}
