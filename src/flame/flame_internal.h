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

/* QuickJS internal structs (defined later in quickjs.c; this header is
 * included before them). */
struct JSRuntime;
struct JSContext;
struct JSObject;
struct JSFunctionBytecode;
struct JSShape;
struct FlameProfile;

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
    struct FlameICSite *ics; /* inline-cache sites (phase 3) */
    int ic_count, ic_cap;
    uint8_t no_compile;      /* baseline tier refused this function */
} FlameProfile;

/* Inline-cache site record. Cached own-property reads are self-validating:
 * a hit requires the object's shape pointer to match AND the atom at the
 * cached shape slot to still equal the site's constant atom, so shape
 * mutation or deletion can never serve a stale value. The shape reference
 * is owned (js_dup_shape) and released on invalidation/free. */
typedef struct FlameICSite {
    uint32_t pc_off;      /* bytecode offset of the access site */
    uint8_t  kind;        /* FLAME_IC_FIELD / FLAME_IC_ELEM */
    uint8_t  cached;      /* a shape/prop_idx cache entry is live */
    struct JSShape *shape;/* unowned hint; validated via epoch */
    uint32_t epoch;       /* shape-epoch at cache time (field ICs only) */
    uint32_t prop_idx;    /* index into shape->prop[] (field ICs only) */
    JSAtom    atom;       /* site's constant atom (field ICs only) */
    uint8_t  elem_kind;   /* FLAME_KIND feedback for element loads */
    uint32_t hits, misses;
} FlameICSite;

#define FLAME_IC_FIELD 0
#define FLAME_IC_ELEM  1

/* IC fast paths (defined in flame_ic.c). Return 1 + fill *out on a cache
 * hit; return 0 (miss) and the caller uses the normal property path, then
 * invokes the matching _record function with the resolved value. */
int flame_ic_get_field(JSContext *ctx, struct JSFunctionBytecode *b,
                       uint32_t pc_off, JSValueConst obj, JSAtom atom,
                       JSValue *out);
void flame_ic_record_field(JSContext *ctx, struct JSFunctionBytecode *b,
                           uint32_t pc_off, JSValueConst obj, JSAtom atom,
                           JSValueConst result);
int flame_ic_get_array_el(JSContext *ctx, struct JSFunctionBytecode *b,
                          uint32_t pc_off, JSValueConst obj, JSValueConst prop,
                          JSValue *out);
void flame_ic_record_array_el(JSContext *ctx, struct JSFunctionBytecode *b,
                              uint32_t pc_off, JSValueConst obj,
                              JSValueConst prop, JSValueConst result);

#define FLAME_PROFILE_MAGIC 0x464C414D  /* 'FLAM' */

/* Kind of a JSValue for feedback purposes. */
FlameKind flame_kind_of(JSValueConst v);
/* Classify a FlameKind observation into a feedback byte slot (monotonic
 * widening: distinct kinds collapse to FLAME_KIND_MIXED). */
void flame_fb_record(uint8_t *slot, FlameKind k);

/* Profile lifecycle (defined in flame_profiler.c). */
void flame_free_profile(struct JSRuntime *rt, void *prof);
void flame_ic_free_profile(struct JSRuntime *rt, struct FlameProfile *prof);

/* Global IC counters (flame_ic.c). */
extern uint64_t flame_stat_ic_hits, flame_stat_ic_misses;

/* ------------------------------------------------------------------ */
/* Runtime state + executable memory (flame_rt.c)                      */
/* ------------------------------------------------------------------ */

/* Global JIT statistics. */
extern uint64_t flame_stat_compiles, flame_stat_compile_fails;
extern uint64_t flame_stat_jit_calls, flame_stat_deopts, flame_stat_osr;
extern uint64_t flame_stat_native_bytes;

/* A compiled function: native code + backlink, registered in the
 * per-runtime code cache. */
typedef struct FlameCompiledFunc {
    struct FlameCompiledFunc *next;
    struct JSFunctionBytecode *b; /* backlink (not owned) */
    uint8_t *code;                /* inside an exec block */
    size_t code_size;
    void *osr_map;                /* loop-header -> native offset map */
    int osr_count;
    uint8_t tier;                 /* 2 = baseline, 3 = optimizing */
} FlameCompiledFunc;

/* Executable memory (VirtualAlloc / mmap). */
void *flame_exec_alloc(size_t size);
void flame_exec_free(void *ptr, size_t size);
void flame_exec_flush_icache(void *ptr, size_t size);

/* Runtime plumbing. */
int flame_global_enabled_flag(void);
int flame_rt_is_enabled(struct JSRuntime *rt);
void flame_rt_dispose(struct JSRuntime *rt);
/* Allocate a fresh executable block of `size` bytes. */
struct FlameExecBlock {
    uint8_t *base;
    size_t size, used;
};
typedef struct FlameExecBlock FlameExecBlock;
typedef struct FlameCompiledFunc FlameCompiledFunc;
typedef struct FlameProfile FlameProfile;
typedef struct FlameICSite FlameICSite;
typedef struct FlameCallSite FlameCallSite;
typedef struct FlameLoop FlameLoop;
struct FlameExecBlock *flame_rt_new_block(struct JSRuntime *rt, size_t size);
void flame_rt_register_func(struct JSRuntime *rt, struct JSFunctionBytecode *b,
                            void *code, size_t code_size, uint8_t tier);
void flame_set_stats_rt(struct JSRuntime *rt);

/* Baseline tier (flame_baseline.c). */
int flame_should_enter_jit(struct JSContext *ctx, struct JSObject *p,
                           struct JSFunctionBytecode *b, int flags);
JSValue flame_baseline_call(struct JSContext *caller_ctx,
                            JSValueConst func_obj, JSValueConst this_obj,
                            int argc, JSValue *argv);
void flame_baseline_maybe_compile(struct JSContext *ctx,
                                  struct JSFunctionBytecode *b);

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
