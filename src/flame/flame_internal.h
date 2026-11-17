/*
 * Flame — internal shared declarations.
 *
 * This header is only included by Flame's own sources, which are compiled
 * inside the QuickJS translation unit (quickjs.c includes flame_all.h at the
 * bottom), so full access to QuickJS internals is available and struct
 * layouts are guaranteed to match exactly.
 */
#ifndef SQUARE_FLAME_INTERNAL_H
#define SQUARE_FLAME_INTERNAL_H

#include "flame.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Debug logging                                                       */
/* ------------------------------------------------------------------ */

/* FLAME_DEBUG environment variable, parsed once on first use:
 *   0                : quiet (default)
 *   1                : tiering events, compiles, deopts, IC transitions
 *   2                : + per-compile IR/codegen dumps
 *   trace            : + interpreter per-opcode trace (very verbose)
 *   stats            : dump stats at exit (atexit)
 * Comma-separated list is accepted, e.g. "1,stats". */
extern int flame_debug_level;
#define FLAME_LOG_ALWAYS  0
#define FLAME_LOG_TIER    1
#define FLAME_LOG_VERBOSE 2
#define FLAME_LOG_TRACE   3

int flame_debug_init(void);   /* parses FLAME_DEBUG once, returns level */
void flame_dump_stats_atexit(void);
void flame_log(int level, const char *fmt, ...) __attribute__((format(printf,2,3)));

/* ------------------------------------------------------------------ */
/* Tiering thresholds                                                  */
/* ------------------------------------------------------------------ */

/* Interpreter entries before profiling kicks in (tier 0 -> 1). */
#define FLAME_HOT_ENTRY_THRESHOLD 100
/* Profiled loop backedges before baseline compilation (tier 1 -> 2). */
#define FLAME_HOT_LOOP_THRESHOLD  200
/* Profiled function entries before optimizing compilation (tier 1 -> 3). */
#define FLAME_HOT_FUNC_THRESHOLD  2000

/* ------------------------------------------------------------------ */
/* Value-kind feedback encoding                                        */
/* ------------------------------------------------------------------ */

typedef enum FlameKind {
    FLAME_KIND_NONE    = 0,
    FLAME_KIND_INT     = 1,
    FLAME_KIND_FLOAT   = 2,
    FLAME_KIND_STRING  = 3,
    FLAME_KIND_OBJECT  = 4,
    FLAME_KIND_BOOL    = 5,
    FLAME_KIND_NULLISH = 6,  /* null or undefined */
    FLAME_KIND_BIGINT  = 7,
    FLAME_KIND_SYMBOL  = 8,
    FLAME_KIND_MIXED   = 9,  /* more than one kind observed */
} FlameKind;

/* Per-bytecode profiling side table, lazily allocated when the function
 * reaches FLAME_HOT_ENTRY_THRESHOLD interpreter entries. Referenced from
 * JSFunctionBytecode.flame_prof. */
typedef struct FlameCallSite {
    uint32_t pc_off;   /* bytecode offset of the call opcode */
    uint32_t count;    /* call frequency */
    uint8_t  arg_kind; /* FLAME_KIND of the callee (argv[-1]) so far */
} FlameCallSite;

typedef struct FlameLoop {
    uint32_t pc_off;   /* bytecode offset of the loop header (jump target) */
    uint32_t count;    /* backedge count */
} FlameLoop;

typedef struct FlameProfile {
    uint32_t magic;       /* FLAME_PROFILE_MAGIC */
    uint8_t *binop_fb;    /* byte per bytecode offset: (lhs<<4)|rhs FlameKind,
                             0xFF byte offset = no arith op there */
    int binop_fb_len;     /* == byte_code_len */
    FlameCallSite *calls; int call_count, call_cap;
    FlameLoop *loops;     int loop_count, loop_cap;
    uint32_t entries;     /* interpreter entries since profiling started */
} FlameProfile;

#define FLAME_PROFILE_MAGIC 0x464C414D  /* 'FLAM' */

/* Kind of a JSValue for feedback purposes. */
FlameKind flame_kind_of(JSValueConst v);
/* Classify a FlameKind observation into a feedback byte slot (monotonic
 * widening: distinct kinds collapse to FLAME_KIND_MIXED). */
void flame_fb_record(uint8_t *slot, FlameKind k);

/* Profile lifecycle (defined in flame_profiler.c). */
void flame_free_profile(struct JSRuntime *rt, void *prof);

/* ------------------------------------------------------------------ */
/* Hook entry points (called from quickjs.c, defined in Flame sources) */
/* ------------------------------------------------------------------ */

struct JSRuntime;
struct JSContext;
struct JSObject;
struct JSFunctionBytecode;

/* One-time global init (env parsing, kill-switch). Idempotent. */
void flame_global_init(void);

/* Function entry: bump exec count, drive tiering. b may be NULL for
 * C functions / non-bytecode calls. */
void flame_hook_function_entry(struct JSContext *ctx,
                               struct JSObject *func_obj,
                               struct JSFunctionBytecode *b);

/* Backward jump (loop backedge) at bytecode offset pc_off. */
void flame_hook_loop_backedge(struct JSFunctionBytecode *b, uint32_t pc_off);

/* Call site: records call frequency + argument kind feedback. */
void flame_hook_call_site(struct JSFunctionBytecode *b, uint32_t pc_off,
                          int argc, const JSValue *argv);

/* Arithmetic/binary op feedback: records operand value kinds. */
void flame_hook_binop(struct JSFunctionBytecode *b, uint32_t pc_off,
                      int op, JSValueConst a, JSValueConst b2);

/* Property load feedback / inline cache update (field access on object). */
void flame_hook_get_field(struct JSFunctionBytecode *b, uint32_t pc_off,
                          JSValueConst obj, JSAtom atom);

/* Array element load feedback (dense-array fast path data). */
void flame_hook_get_array_el(struct JSFunctionBytecode *b, uint32_t pc_off,
                             JSValueConst obj, JSValueConst prop);

#endif /* SQUARE_FLAME_INTERNAL_H */
