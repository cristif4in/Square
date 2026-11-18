/*
 * Flame — tier 1: profiling side tables.
 *
 * A JSFunctionBytecode starts unprofiled (tier 0). Once it accumulates
 * FLAME_HOT_ENTRY_THRESHOLD interpreter entries, a FlameProfile side table
 * is attached and the interpreter hooks start recording:
 *   - per-site value-kind feedback for arithmetic ops (binop_fb, low nibble
 *     = lhs kind, high nibble = rhs kind, FLAME_KIND_NONE = no data)
 *   - call-site frequency + callee kind (calls[])
 *   - loop backedge counts keyed by loop-header offset (loops[])
 *
 * The profile is what later tiers consume; nothing here changes observable
 * interpreter behavior. All hook implementations live in this file so the
 * interpreter call sites stay one-hop.
 */
#include "flame_internal.h"

/* ------------------------------------------------------------------ */
/* Kind classification                                                 */
/* ------------------------------------------------------------------ */

FlameKind flame_kind_of(JSValueConst v)
{
    switch (JS_VALUE_GET_TAG(v)) {
    case JS_TAG_INT:           return FLAME_KIND_INT;
    case JS_TAG_FLOAT64:       return FLAME_KIND_FLOAT;
    case JS_TAG_BOOL:          return FLAME_KIND_BOOL;
    case JS_TAG_NULL:
    case JS_TAG_UNDEFINED:     return FLAME_KIND_NULLISH;
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:   return FLAME_KIND_STRING;
    case JS_TAG_OBJECT:        return FLAME_KIND_OBJECT;
    case JS_TAG_BIG_INT:
    case JS_TAG_SHORT_BIG_INT: return FLAME_KIND_BIGINT;
    case JS_TAG_SYMBOL:        return FLAME_KIND_SYMBOL;
    default:                   return FLAME_KIND_NONE;
    }
}

/* Monotonic widening inside a feedback cell: first kind wins, any other
 * kind flips the cell to FLAME_KIND_MIXED, mixed stays mixed. */
static void fb_cell(uint8_t *cell, FlameKind k)
{
    if (*cell == FLAME_KIND_NONE)
        *cell = (uint8_t)k;
    else if (*cell != k)
        *cell = FLAME_KIND_MIXED;
}

void flame_fb_record(uint8_t *slot, FlameKind k)
{
    fb_cell(slot, k);
}

/* Same monotonic widening, but inside a 4-bit nibble of a feedback byte
 * (shift 0 = lhs, shift 4 = rhs). */
static void fb_nibble(uint8_t *byte, int shift, FlameKind k)
{
    uint8_t cur = (uint8_t)((*byte >> shift) & 0xF);
    uint8_t nv;
    if (cur == FLAME_KIND_NONE)
        nv = (uint8_t)k;
    else if (cur == (uint8_t)k)
        nv = cur;
    else
        nv = FLAME_KIND_MIXED;
    *byte = (uint8_t)((*byte & ~(0xFu << shift)) | (nv << shift));
}

/* ------------------------------------------------------------------ */
/* Profile lifecycle                                                   */
/* ------------------------------------------------------------------ */

/* Build a human-readable function label (atom name or NULL). Caller frees. */
static char *flame_fn_label(JSContext *ctx, JSFunctionBytecode *b)
{
    const char *s;
    char *out;
    if (!JS_AtomIsString(ctx, b->func_name))
        return NULL;
    s = JS_AtomToCString(ctx, b->func_name);
    if (!s)
        return NULL;
    out = js_strdup(ctx, s);
    JS_FreeCString(ctx, s);
    return out;
}

/* Attach a fresh profile to a bytecode that just went hot. */
static FlameProfile *flame_attach_profile(JSContext *ctx, JSFunctionBytecode *b)
{
    FlameProfile *prof;
    char *label = flame_fn_label(ctx, b);

    prof = js_mallocz(ctx, sizeof(FlameProfile));
    if (!prof) {
        js_free(ctx, label);
        return NULL;
    }
    prof->magic = FLAME_PROFILE_MAGIC;
    if (b->byte_code_len > 0) {
        prof->binop_fb = js_mallocz(ctx, b->byte_code_len);
        if (!prof->binop_fb) {
            js_free(ctx, prof);
            js_free(ctx, label);
            return NULL;
        }
        prof->binop_fb_len = b->byte_code_len;
    }
    b->flame_prof = prof;
    flame_log(FLAME_LOG_TIER, "tier1 profiling enabled: %s (%d bytes of bytecode)",
              label ? label : "<anon>", b->byte_code_len);
    js_free(ctx, label);
    return prof;
}

void flame_free_profile(JSRuntime *rt, void *profv)
{
    FlameProfile *prof = (FlameProfile *)profv;
    if (!prof)
        return;
    flame_log(FLAME_LOG_VERBOSE, "freeing profile %p (ics=%d)", (void *)prof,
              prof->ic_count);
    flame_ic_free_profile(rt, prof);
    js_free_rt(rt, prof->binop_fb);
    js_free_rt(rt, prof->calls);
    js_free_rt(rt, prof->loops);
    js_free_rt(rt, prof);
}

/* ------------------------------------------------------------------ */
/* Side-table lookup (linear; site counts are small in practice)       */
/* ------------------------------------------------------------------ */

static FlameCallSite *flame_find_call(JSContext *ctx, FlameProfile *prof,
                                      uint32_t pc_off)
{
    int i;
    for (i = 0; i < prof->call_count; i++)
        if (prof->calls[i].pc_off == pc_off)
            return &prof->calls[i];
    if (prof->call_count == prof->call_cap) {
        int cap = prof->call_cap ? prof->call_cap * 2 : 4;
        FlameCallSite *nc = js_realloc_rt(ctx->rt, prof->calls,
                                          cap * sizeof(*nc));
        if (!nc)
            return NULL;
        prof->calls = nc;
        prof->call_cap = cap;
    }
    prof->calls[prof->call_count].pc_off = pc_off;
    prof->calls[prof->call_count].count = 0;
    prof->calls[prof->call_count].arg_kind = FLAME_KIND_NONE;
    return &prof->calls[prof->call_count++];
}

static FlameLoop *flame_find_loop(JSContext *ctx, FlameProfile *prof,
                                  uint32_t pc_off)
{
    int i;
    for (i = 0; i < prof->loop_count; i++)
        if (prof->loops[i].pc_off == pc_off)
            return &prof->loops[i];
    if (prof->loop_count == prof->loop_cap) {
        int cap = prof->loop_cap ? prof->loop_cap * 2 : 4;
        FlameLoop *nl = js_realloc_rt(ctx->rt, prof->loops,
                                      cap * sizeof(*nl));
        if (!nl)
            return NULL;
        prof->loops = nl;
        prof->loop_cap = cap;
    }
    prof->loops[prof->loop_count].pc_off = pc_off;
    prof->loops[prof->loop_count].count = 0;
    return &prof->loops[prof->loop_count++];
}

/* ------------------------------------------------------------------ */
/* Hook implementations (called from the interpreter)                  */
/* ------------------------------------------------------------------ */

void flame_hook_function_entry(JSContext *ctx, JSObject *p,
                               JSFunctionBytecode *b)
{
    FlameProfile *prof;

    if (!flame_is_enabled(NULL))
        return;
    b->flame_exec_count++;
    prof = (FlameProfile *)b->flame_prof;
    if (!prof) {
        if (b->flame_exec_count >= FLAME_HOT_ENTRY_THRESHOLD)
            flame_attach_profile(ctx, b);
        return;
    }
    prof->entries++;
    /* Tier 1 -> 2: enough entries to justify native code. */
    if (prof->entries == FLAME_HOT_FUNC_THRESHOLD)
        flame_baseline_maybe_compile(ctx, b);
}

void flame_hook_loop_backedge(JSFunctionBytecode *b, uint32_t pc_off)
{
    FlameProfile *prof = (FlameProfile *)b->flame_prof;
    JSContext *ctx = b->realm;
    FlameLoop *loop;
    if (!flame_is_enabled(NULL))
        return;
    /* A hot loop qualifies the function for profiling even if it is only
     * ever entered once (top-level scripts, one-shot callbacks). */
    if (!prof)
        prof = flame_attach_profile(ctx, b);
    if (!prof)
        return;
    loop = flame_find_loop(ctx, prof, pc_off);
    if (!loop)
        return;
    loop->count++;
    if (loop->count == FLAME_HOT_LOOP_THRESHOLD) {
        flame_log(FLAME_LOG_TIER, "hot loop @ bc:%u (baseline candidate)",
                  pc_off);
        flame_baseline_maybe_compile(ctx, b);
    }
}

void flame_hook_call_site(JSFunctionBytecode *b, uint32_t pc_off,
                          int argc, const JSValue *argv)
{
    FlameProfile *prof = (FlameProfile *)b->flame_prof;
    JSContext *ctx = b->realm;
    FlameCallSite *site;
    if (!prof || !flame_is_enabled(NULL))
        return;
    site = flame_find_call(ctx, prof, pc_off);
    if (!site)
        return;
    site->count++;
    /* interpreter call stack layout: argv[-1] = callee, argv[0] = this */
    flame_fb_record(&site->arg_kind, flame_kind_of(argv[-1]));
    (void)argc;
}

void flame_hook_binop(JSFunctionBytecode *b, uint32_t pc_off,
                      int op, JSValueConst a, JSValueConst b2)
{
    FlameProfile *prof = (FlameProfile *)b->flame_prof;
    uint8_t *fb;
    if (!prof || !flame_is_enabled(NULL))
        return;
    if (!prof->binop_fb || pc_off >= (uint32_t)prof->binop_fb_len)
        return;
    fb = &prof->binop_fb[pc_off];
    fb_nibble(fb, 0, flame_kind_of(a));  /* low nibble = lhs kind */
    fb_nibble(fb, 4, flame_kind_of(b2)); /* high nibble = rhs kind */
    (void)op;
}

void flame_hook_get_field(JSFunctionBytecode *b, uint32_t pc_off,
                          JSValueConst obj, JSAtom atom)
{
    FlameProfile *prof = (FlameProfile *)b->flame_prof;
    if (!prof || !flame_is_enabled(NULL))
        return;
    /* IC recording lands in phase 3 (flame_ic.c). */
    (void)pc_off; (void)obj; (void)atom;
}

void flame_hook_get_array_el(JSFunctionBytecode *b, uint32_t pc_off,
                             JSValueConst obj, JSValueConst prop)
{
    FlameProfile *prof = (FlameProfile *)b->flame_prof;
    if (!prof || !flame_is_enabled(NULL))
        return;
    (void)pc_off; (void)obj; (void)prop;
}
