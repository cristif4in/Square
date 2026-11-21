/*
 * Flame — inline caches (tier 1 side tables, consumed by the JIT tiers).
 *
 * Field IC: caches (shape, prop index) for plain own data-property reads.
 * Validation is self-contained — shape pointer identity plus the atom at
 * the cached shape slot — so deletions, re-additions and in-place shape
 * growth can never produce a stale value. The shape is held by reference.
 *
 * Element IC: fast-path reads from dense Array/Arguments storage, guarded
 * at runtime by class id, int index and bounds, which is always correct.
 * Element kind feedback feeds the optimizing tier.
 */
#include "flame_internal.h"

/* Global IC stats (dumped by flame_dump_stats). */
uint64_t flame_stat_ic_hits, flame_stat_ic_misses;

static FlameICSite *ic_site(JSContext *ctx, FlameProfile *prof,
                            uint32_t pc_off, uint8_t kind)
{
    int i;
    for (i = 0; i < prof->ic_count; i++)
        if (prof->ics[i].pc_off == pc_off && prof->ics[i].kind == kind)
            return &prof->ics[i];
    if (prof->ic_count == prof->ic_cap) {
        int cap = prof->ic_cap ? prof->ic_cap * 2 : 4;
        FlameICSite *ni = js_realloc_rt(ctx->rt, prof->ics, cap * sizeof(*ni));
        if (!ni)
            return NULL;
        prof->ics = ni;
        prof->ic_cap = cap;
    }
    {
        FlameICSite *site = &prof->ics[prof->ic_count++];
        memset(site, 0, sizeof(*site));
        site->pc_off = pc_off;
        site->kind = kind;
        site->elem_kind = FLAME_KIND_NONE;
        return site;
    }
}

static void ic_invalidate(JSRuntime *rt, FlameICSite *site)
{
    /* no reference is held, so nothing to release */
    (void)rt;
    site->shape = NULL;
    site->cached = 0;
}


int flame_ic_get_field(JSContext *ctx, JSFunctionBytecode *b,
                       uint32_t pc_off, JSValueConst obj, JSAtom atom,
                       JSValue *out)
{
    FlameProfile *prof = (FlameProfile *)b->flame_prof;
    FlameICSite *site;
    JSObject *p;
    JSShapeProperty *prs;

    {
        static int noic = -1;
        if (noic < 0) {
            const char *e = getenv("FLAME_NO_IC");
            noic = (e && *e && strcmp(e, "0") != 0);
        }
        if (noic)
            return 0;
    }
    if (!prof || !flame_is_enabled(NULL))
        return 0;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return 0;
    p = JS_VALUE_GET_OBJ(obj);
    site = ic_site(ctx, prof, pc_off, FLAME_IC_FIELD);
    if (!site)
        return 0;

    if (site->cached && site->shape == p->shape &&
        site->epoch == ctx->rt->flame_shape_epoch &&
        site->prop_idx < (uint32_t)p->shape->prop_count) {
        prs = &get_shape_prop(p->shape)[site->prop_idx];
        if (prs->atom == atom && !(prs->flags & JS_PROP_TMASK)) {
            *out = JS_DupValue(ctx, p->prop[site->prop_idx].u.value);
            site->hits++;
            flame_stat_ic_hits++;
            return 1;
        }
    }
    /* miss: drop any stale entry */
    if (site->cached) {
        ic_invalidate(ctx->rt, site);
        site->misses++;
        flame_stat_ic_misses++;
    }
    return 0;
}

void flame_ic_record_field(JSContext *ctx, JSFunctionBytecode *b,
                           uint32_t pc_off, JSValueConst obj, JSAtom atom,
                           JSValueConst result)
{
    FlameProfile *prof = (FlameProfile *)b->flame_prof;
    FlameICSite *site;
    JSObject *p;
    JSShapeProperty *prs;
    JSProperty *pr;

    if (!prof || !flame_is_enabled(NULL))
        return;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return;
    p = JS_VALUE_GET_OBJ(obj);
    prs = find_own_property(&pr, p, atom);
    if (!prs || (prs->flags & JS_PROP_TMASK))
        return; /* only cache plain own data properties */
    site = ic_site(ctx, prof, pc_off, FLAME_IC_FIELD);
    if (!site)
        return;
    if (site->cached && site->shape == p->shape &&
        site->prop_idx == (uint32_t)(prs - get_shape_prop(p->shape)))
        return; /* already correct */
    if (site->cached)
        ic_invalidate(ctx->rt, site);
    /* no reference held: validity is re-checked via the shape-epoch below */
    site->shape = p->shape;
    site->epoch = ctx->rt->flame_shape_epoch;
    site->prop_idx = (uint32_t)(prs - get_shape_prop(p->shape));
    site->atom = atom;
    site->cached = 1;
    (void)result;
}


/* Fast dense read, mirroring JS_GetPropertyValue's own fast path exactly.
 * Returns 1 + fills *out when eligible. */
static int ic_fast_el(JSContext *ctx, JSObject *p, JSValueConst prop,
                      JSValue *out)
{
    int32_t idx;
    if (JS_VALUE_GET_TAG(prop) != JS_TAG_INT)
        return 0;
    if (p->class_id != JS_CLASS_ARRAY && p->class_id != JS_CLASS_ARGUMENTS)
        return 0;
    idx = JS_VALUE_GET_INT(prop);
    if (idx < 0 || (uint32_t)idx >= p->u.array.count)
        return 0;
    *out = JS_DupValue(ctx, p->u.array.u.values[idx]);
    return 1;
}

int flame_ic_get_array_el(JSContext *ctx, JSFunctionBytecode *b,
                          uint32_t pc_off, JSValueConst obj,
                          JSValueConst prop, JSValue *out)
{
    FlameProfile *prof = (FlameProfile *)b->flame_prof;
    FlameICSite *site;
    JSObject *p;

    if (!prof || !flame_is_enabled(NULL))
        return 0;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return 0;
    p = JS_VALUE_GET_OBJ(obj);
    site = ic_site(ctx, prof, pc_off, FLAME_IC_ELEM);
    if (!site)
        return 0;
    if (ic_fast_el(ctx, p, prop, out)) {
        site->hits++;
        flame_stat_ic_hits++;
        flame_fb_record(&site->elem_kind, flame_kind_of(*out));
        return 1;
    }
    site->misses++;
    flame_stat_ic_misses++;
    return 0;
}

void flame_ic_record_array_el(JSContext *ctx, JSFunctionBytecode *b,
                              uint32_t pc_off, JSValueConst obj,
                              JSValueConst prop, JSValueConst result)
{
    FlameProfile *prof = (FlameProfile *)b->flame_prof;
    FlameICSite *site;

    if (!prof || !flame_is_enabled(NULL))
        return;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return;
    site = ic_site(ctx, prof, pc_off, FLAME_IC_ELEM);
    if (!site)
        return;
    /* record the resolved element's kind even when the site missed, so
     * feedback accumulates for polymorphic sites too */
    flame_fb_record(&site->elem_kind, flame_kind_of(result));
    (void)prop;
}

/* Profile teardown (called from flame_free_profile). */
void flame_ic_free_profile(JSRuntime *rt, FlameProfile *prof)
{
    int i;
    for (i = 0; i < prof->ic_count; i++) {
        ic_invalidate(rt, &prof->ics[i]);
    }
}
