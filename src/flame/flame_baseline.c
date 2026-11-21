/*
 * Flame baseline JIT.
 *
 * Translates QuickJS bytecode to native code one opcode at a time
 */
#include "flame_internal.h"
#include "flame_emitter.h"

static int osr_has_prop_store(JSFunctionBytecode *b);


typedef struct FlameJitState {
    JSStackFrame sf;         /* embedded frame (baseline entry path) */
    JSStackFrame *sfp;       /* active frame: &sf, or the interpreter's live
                                frame when running an OSR entry */
    JSContext *ctx;          /* b->realm, like the interpreter */
    JSFunctionBytecode *b;
    JSVarRef **var_refs;
    JSValue *cpool;
    uint8_t *bc_buf;
    JSValue *local_buf;      /* [args | vars | stack], malloc'd */
    JSValue *stack_base;
    JSValue *sp;
    JSValue retval;
    JSValue this_val;
    uint32_t param;          /* per-site constant for the next helper call */
    uint32_t param2;         /* second constant / helper out-value */
    uint32_t argc;
    uint32_t deopt_pc;       /* bytecode offset to resume at (tier 3) */
    /* exception / exit_kind are 32-bit because emitted code accesses them
     * with fl_mov32_rm / fl_mov32_mr; keeping them narrower would let the
     * 4-byte access bleed into adjacent fields (and, for exit_kind at the
     * struct tail, run past the allocation). */
    uint32_t exception;
    uint32_t exit_kind;      /* 0 running, 1 exception, 2 return, 3 deopt */
} FlameJitState;

/* Field offsets used by emitted code (ABI-agnostic: everything goes
 * through the state pointer). */
#define FL_OFF_LOCAL_BUF  ((int)offsetof(FlameJitState, local_buf))
#define FL_OFF_SP         ((int)offsetof(FlameJitState, sp))
#define FL_OFF_RETVAL_U   ((int)offsetof(FlameJitState, retval) + 0)
#define FL_OFF_RETVAL_T   ((int)offsetof(FlameJitState, retval) + 8)
#define FL_OFF_PARAM      ((int)offsetof(FlameJitState, param))
#define FL_OFF_PARAM2     ((int)offsetof(FlameJitState, param2))
#define FL_OFF_CPOOL      ((int)offsetof(FlameJitState, cpool))
#define FL_OFF_BC_BUF     ((int)offsetof(FlameJitState, bc_buf))
#define FL_OFF_DEOPT_PC   ((int)offsetof(FlameJitState, deopt_pc))
#define FL_OFF_EXCEPTION  ((int)offsetof(FlameJitState, exception))
#define FL_OFF_EXIT_KIND  ((int)offsetof(FlameJitState, exit_kind))
#define FL_OFF_SFP        ((int)offsetof(FlameJitState, sfp))
#define FL_SF_CUR_PC      ((int)offsetof(JSStackFrame, cur_pc))

/* JSValue slot layout: the 64-bit two-struct encoding from quickjs.h
 * (u union at offset 0, int64 tag at offset 8, sizeof == 16). */
#define FL_SZ_VALUE   16
#define FL_OFF_U      0
#define FL_OFF_TAG    8

#define FL_TAG_INT          0
#define FL_TAG_BOOL         1
#define FL_TAG_NULL         2
#define FL_TAG_UNDEFINED    3
#define FL_TAG_FLOAT64      8
#define FL_TAG_UNINITIALIZED 4
#define FL_TAG_OBJECT      (-1)

/* JSObject field offsets for speculative inline paths (same TU as
 * quickjs.c, so these are always correct) */
#define FL_OFF_CLASS_ID   ((int)offsetof(JSObject, class_id))
#define FL_OFF_ARR_COUNT  ((int)offsetof(JSObject, u.array.count))
#define FL_OFF_ARR_VALUES ((int)offsetof(JSObject, u.array.u.values))

/* The array-element fast path multiplies idx by 16 (shift left 4).
 * Bounding idx below 2^27 keeps idx*16 representable as a positive
 * int32_t, so the 32-bit shift cannot overflow. */
#define FL_MAX_DENSE_IDX  0x07FFFFFF


static void h_set_pc(FlameJitState *st)
{
    st->sfp->cur_pc = st->bc_buf + st->param2;
}

static void h_binop(FlameJitState *st)
{
    int op = (int)st->param;
    h_set_pc(st);
    if (op == OP_add) {
        if (js_add_slow(st->ctx, st->sp))
            st->exception = 1;
    } else {
        if (js_binary_arith_slow(st->ctx, st->sp, op))
            st->exception = 1;
    }
    if (!st->exception)
        st->sp--;
}

static void h_logic(FlameJitState *st)
{
    h_set_pc(st);
    if (js_binary_logic_slow(st->ctx, st->sp, (int)st->param))
        st->exception = 1;
    if (!st->exception)
        st->sp--;
}

static void h_relational(FlameJitState *st)
{
    h_set_pc(st);
    if (js_relational_slow(st->ctx, st->sp, (int)st->param))
        st->exception = 1;
    if (!st->exception)
        st->sp--;
}

static void h_eq(FlameJitState *st)
{
    h_set_pc(st);
    if (js_eq_slow(st->ctx, st->sp, (int)st->param))
        st->exception = 1;
    if (!st->exception)
        st->sp--;
}

static void h_strict_eq(FlameJitState *st)
{
    JSValue op1 = st->sp[-2], op2 = st->sp[-1];
    BOOL res = js_strict_eq(st->ctx, op1, op2);
    JS_FreeValue(st->ctx, op1);
    JS_FreeValue(st->ctx, op2);
    st->sp[-2] = JS_NewBool(st->ctx, res);
    st->sp--;
}

static void h_to_bool(FlameJitState *st)
{
    JSValue op1 = st->sp[-1];
    uint32_t tag = (uint32_t)JS_VALUE_GET_TAG(op1);
    if (tag <= (uint32_t)JS_TAG_UNDEFINED) {
        st->param2 = (uint32_t)JS_VALUE_GET_INT(op1);
    } else {
        /* JS_ToBoolFree returns -1 on exception (not 0). The original
         * "res == 0 && IsException(current_exception)" guard never fired,
         * silently swallowing conversion errors. */
        int res = JS_ToBoolFree(st->ctx, op1);
        if (res < 0) {
            st->exception = 1;
            return;
        }
        st->param2 = (uint32_t)res;
    }
    st->sp--;
}

static void h_get_field(FlameJitState *st)
{
    JSAtom atom = (JSAtom)st->param;
    JSValue obj = st->sp[-1];
    JSValue val;
    h_set_pc(st);
    if (!flame_ic_get_field(st->ctx, st->b, st->param2, obj, atom, &val)) {
        val = JS_GetProperty(st->ctx, obj, atom);
        if (JS_IsException(val)) {
            st->exception = 1;
            return;
        }
        flame_ic_record_field(st->ctx, st->b, st->param2, obj, atom, val);
    }
    JS_FreeValue(st->ctx, obj);
    st->sp[-1] = val;
}

static void h_get_field2(FlameJitState *st)
{
    JSAtom atom = (JSAtom)st->param;
    JSValue val;
    h_set_pc(st);
    val = JS_GetProperty(st->ctx, st->sp[-1], atom);
    if (JS_IsException(val)) {
        st->exception = 1;
        return;
    }
    *st->sp++ = val;
}

static void h_put_field(FlameJitState *st)
{
    JSAtom atom = (JSAtom)st->param;
    JSValue obj = st->sp[-2];
    int ret;
    h_set_pc(st);
    ret = JS_SetPropertyInternal(st->ctx, obj, atom, st->sp[-1], obj,
                                 JS_PROP_THROW_STRICT);
    JS_FreeValue(st->ctx, obj);
    st->sp -= 2;
    if (ret < 0)
        st->exception = 1;
}

static void h_get_array_el(FlameJitState *st)
{
    JSValue obj = st->sp[-2], prop = st->sp[-1];
    JSValue val;
    h_set_pc(st);
    if (!flame_ic_get_array_el(st->ctx, st->b, st->param2, obj, prop, &val)) {
        /* JS_GetPropertyValue consumes `prop` (frees it internally). The
         * IC's record call still needs the key, so keep a reference.
         * On exception prop is already gone -- only free the dup. */
        JSValue prop_keep = JS_DupValue(st->ctx, prop);
        val = JS_GetPropertyValue(st->ctx, obj, prop);
        if (JS_IsException(val)) {
            JS_FreeValue(st->ctx, obj);
            JS_FreeValue(st->ctx, prop_keep);
            st->exception = 1;
            return;
        }
        flame_ic_record_array_el(st->ctx, st->b, st->param2, obj, prop_keep, val);
        JS_FreeValue(st->ctx, prop_keep);
    }
    JS_FreeValue(st->ctx, obj);
    st->sp[-2] = val;
    st->sp--;
}

static void h_get_array_el2(FlameJitState *st)
{
    JSValue val;
    h_set_pc(st);
    val = JS_GetPropertyValue(st->ctx, st->sp[-2], st->sp[-1]);
    st->sp[-1] = val;
    if (JS_IsException(val))
        st->exception = 1;
}

static void h_put_array_el(FlameJitState *st)
{
    JSValue obj = st->sp[-3];
    int ret;
    h_set_pc(st);
    ret = JS_SetPropertyValue(st->ctx, obj, st->sp[-2], st->sp[-1],
                              JS_PROP_THROW_STRICT);
    /* JS_SetPropertyValue consumes prop and val; only obj remains ours */
    JS_FreeValue(st->ctx, obj);
    st->sp -= 3;
    if (ret < 0)
        st->exception = 1;
}

static void h_get_length(FlameJitState *st)
{
    JSValue obj = st->sp[-1];
    JSValue val;
    h_set_pc(st);
    val = JS_GetProperty(st->ctx, obj, JS_ATOM_length);
    if (JS_IsException(val)) {
        st->exception = 1;
        return;
    }
    JS_FreeValue(st->ctx, obj);
    st->sp[-1] = val;
}

static void h_call(FlameJitState *st)
{
    int argc = (int)st->param;
    int method = (int)st->param2;
    JSValue *call_argv = st->sp - argc;
    JSValue ret;
    ret = JS_CallInternal(st->ctx, call_argv[-1],
                          method ? call_argv[-2] : JS_UNDEFINED,
                          JS_UNDEFINED, argc, call_argv, 0);
    if (JS_IsException(ret)) {
        st->exception = 1; /* stack unwound (freed) by the epilogue */
        return;
    }
    {
        int i, base = method ? -2 : -1;
        for (i = base; i < argc; i++)
            JS_FreeValue(st->ctx, call_argv[i]);
    }
    st->sp -= argc + 1 + method;
    *st->sp++ = ret;
}

static void h_tail_call(FlameJitState *st)
{
    int argc = (int)st->param;
    int method = (int)(st->param2 & 1);
    JSValue *call_argv = st->sp - argc;
    JSValue ret;
    ret = JS_CallInternal(st->ctx, call_argv[-1],
                          method ? call_argv[-2] : JS_UNDEFINED,
                          JS_UNDEFINED, argc, call_argv, 0);
    st->retval = ret;
    st->exit_kind = JS_IsException(ret) ? 1 : 2;
}

static void h_var_ref(FlameJitState *st)
{
    int idx = (int)st->param;
    int mode = (int)st->param2; /* 0 get, 1 put, 2 set */
    JSVarRef *vr;
    if (idx >= st->b->closure_var_count || !st->var_refs ||
        !(vr = st->var_refs[idx])) {
        JS_ThrowInternalError(st->ctx, "flame: bad var_ref %d", idx);
        st->exception = 1;
        return;
    }
    switch (mode) {
    case 0:
        *st->sp++ = JS_DupValue(st->ctx, *vr->pvalue);
        break;
    case 1:
        set_value(st->ctx, vr->pvalue, st->sp[-1]);
        st->sp--;
        break;
    default:
        set_value(st->ctx, vr->pvalue, JS_DupValue(st->ctx, st->sp[-1]));
        break;
    }
}

static void h_get_var(FlameJitState *st)
{
    JSAtom atom = (JSAtom)st->param;
    int flags = (int)st->param2; /* 0 = undefined-if-missing, 1 = throw */
    JSValue val;
    h_set_pc(st);
    val = JS_GetGlobalVar(st->ctx, atom, flags);
    if (JS_IsException(val)) {
        st->exception = 1;
        return;
    }
    *st->sp++ = val;
}

static void h_put_var(FlameJitState *st)
{
    JSAtom atom = (JSAtom)st->param;
    int mode = (int)st->param2;
    int ret;
    h_set_pc(st);
    ret = JS_SetGlobalVar(st->ctx, atom, st->sp[-1], mode);
    st->sp--;
    if (ret < 0)
        st->exception = 1;
}

static void h_fclosure(FlameJitState *st)
{
    int idx = (int)st->param;
    JSValue bfunc = JS_DupValue(st->ctx, st->cpool[idx]);
    h_set_pc(st);
    *st->sp++ = js_closure(st->ctx, bfunc, st->var_refs, st->sfp);
    if (JS_IsException(st->sp[-1]))
        st->exception = 1;
}

static void h_push_atom(FlameJitState *st)
{
    h_set_pc(st);
    *st->sp++ = JS_AtomToValue(st->ctx, (JSAtom)st->param);
    if (JS_IsException(st->sp[-1]))
        st->exception = 1;
}

static void h_push_empty_string(FlameJitState *st)
{
    *st->sp++ = JS_AtomToString(st->ctx, JS_ATOM_empty_string);
}

static void h_push_this(FlameJitState *st)
{
    JSValue val;
    JSValueConst this_obj = st->this_val;
    if (!(st->b->js_mode & JS_MODE_STRICT)) {
        uint32_t tag = JS_VALUE_GET_TAG(this_obj);
        if (tag == (uint32_t)JS_TAG_NULL || tag == (uint32_t)JS_TAG_UNDEFINED) {
            val = JS_DupValue(st->ctx, st->ctx->global_obj);
        } else if (tag == (uint32_t)JS_TAG_OBJECT) {
            val = JS_DupValue(st->ctx, this_obj);
        } else {
            val = JS_ToObject(st->ctx, this_obj);
            if (JS_IsException(val)) {
                st->exception = 1;
                return;
            }
        }
    } else {
        val = JS_DupValue(st->ctx, this_obj);
    }
    *st->sp++ = val;
}

static void h_unary_arith(FlameJitState *st)
{
    int op = (int)st->param;
    h_set_pc(st);
    if (op == OP_post_inc || op == OP_post_dec) {
        /* js_post_inc_slow writes the old value above sp and bumps it */
        if (js_post_inc_slow(st->ctx, st->sp, op))
            st->exception = 1;
        else
            st->sp++;
    } else {
        if (js_unary_arith_slow(st->ctx, st->sp, op))
            st->exception = 1;
    }
}

static void h_add_loc(FlameJitState *st)
{
    int idx = (int)st->param;
    JSValue *pv = &st->sfp->var_buf[idx];
    JSValue op2 = st->sp[-1];
    if (likely(JS_VALUE_IS_BOTH_INT(*pv, op2))) {
        int64_t r = (int64_t)JS_VALUE_GET_INT(*pv) + JS_VALUE_GET_INT(op2);
        if ((int)r == r) {
            *pv = JS_NewInt32(st->ctx, (int)r);
            st->sp--;
            return;
        }
    }
    {
        JSValue ops[2];
        ops[0] = JS_DupValue(st->ctx, *pv);
        ops[1] = op2;
        st->sp--;
        if (js_add_slow(st->ctx, ops + 2))
            st->exception = 1;
        else
            set_value(st->ctx, pv, ops[0]);
    }
}

static void h_incdec_loc(FlameJitState *st)
{
    int idx = (int)st->param;
    int op = (int)st->param2;
    JSValue *pv = &st->sfp->var_buf[idx];
    if (likely(JS_VALUE_GET_TAG(*pv) == JS_TAG_INT)) {
        *pv = JS_NewInt32(st->ctx, JS_VALUE_GET_INT(*pv) +
                          (op == OP_inc_loc ? 1 : -1));
        return;
    }
    {
        JSValue ops[2];
        ops[0] = JS_DupValue(st->ctx, *pv);
        if (js_unary_arith_slow(st->ctx, ops + 1,
                                op == OP_inc_loc ? OP_inc : OP_dec))
            st->exception = 1;
        else
            set_value(st->ctx, pv, ops[0]);
    }
}

static void h_typeof(FlameJitState *st)
{
    JSValue op1 = st->sp[-1];
    int atom;
    h_set_pc(st);
    atom = js_operator_typeof(st->ctx, op1);
    if (atom < 0) {
        st->exception = 1;
        return;
    }
    JS_FreeValue(st->ctx, op1);
    st->sp[-1] = JS_AtomToString(st->ctx, (JSAtom)atom);
}

static void h_typeof_is(FlameJitState *st)
{
    JSValue op1 = st->sp[-1];
    int got = js_operator_typeof(st->ctx, op1);
    JS_FreeValue(st->ctx, op1);
    st->sp[-1] = JS_NewBool(st->ctx, got == (int)st->param);
}

static void h_not(FlameJitState *st)
{
    h_set_pc(st);
    if (js_not_slow(st->ctx, st->sp))
        st->exception = 1;
}

static void h_lnot(FlameJitState *st)
{
    JSValue op1 = st->sp[-1];
    uint32_t tag = (uint32_t)JS_VALUE_GET_TAG(op1);
    if (tag <= (uint32_t)JS_TAG_UNDEFINED) {
        st->sp[-1] = JS_NewBool(st->ctx, !JS_VALUE_GET_INT(op1));
    } else {
        int res = JS_ToBoolFree(st->ctx, op1);
        if (res < 0) {
            st->exception = 1;
            return;
        }
        st->sp[-1] = JS_NewBool(st->ctx, !res);
    }
}

static void h_throw(FlameJitState *st)
{
    JS_Throw(st->ctx, *--st->sp);
    st->exception = 1;
}

static void h_is_undefined_or_null(FlameJitState *st)
{
    uint32_t tag = (uint32_t)JS_VALUE_GET_TAG(st->sp[-1]);
    st->sp[-1] = JS_NewBool(st->ctx, tag == (uint32_t)JS_TAG_UNDEFINED ||
                                        tag == (uint32_t)JS_TAG_NULL);
}

static void h_is_tag(FlameJitState *st)
{
    uint32_t tag = (uint32_t)JS_VALUE_GET_TAG(st->sp[-1]);
    st->sp[-1] = JS_NewBool(st->ctx, tag == st->param);
}

static void h_drop(FlameJitState *st)
{
    JS_FreeValue(st->ctx, *--st->sp);
}

static void h_get_loc_check(FlameJitState *st)
{
    int idx = (int)st->param;
    int is_arg = (int)st->param2;
    JSValue *pv = is_arg ? &st->sfp->arg_buf[idx] : &st->sfp->var_buf[idx];
    if (unlikely(JS_IsUninitialized(*pv))) {
        JS_ThrowReferenceErrorUninitialized2(st->ctx, st->b, idx, FALSE);
        st->exception = 1;
        return;
    }
    *st->sp++ = JS_DupValue(st->ctx, *pv);
}

static void h_put_loc_check(FlameJitState *st)
{
    int idx = (int)st->param;
    int mode = (int)st->param2;
    JSValue *pv = &st->sfp->var_buf[idx];
    if (mode == 0) {
        if (unlikely(JS_IsUninitialized(*pv))) {
            JS_ThrowReferenceErrorUninitialized2(st->ctx, st->b, idx, FALSE);
            st->exception = 1;
            return;
        }
    } else {
        if (unlikely(!JS_IsUninitialized(*pv))) {
            JS_ThrowReferenceError(st->ctx, "'this' can be initialized only once");
            st->exception = 1;
            return;
        }
    }
    set_value(st->ctx, pv, st->sp[-1]);
    st->sp--;
}

static void h_var_ref_check(FlameJitState *st)
{
    int idx = (int)st->param;
    int mode = (int)st->param2;
    JSVarRef *vr = st->var_refs[idx];
    JSValue val;
    if (!vr) {
        JS_ThrowInternalError(st->ctx, "flame: bad var_ref %d", idx);
        st->exception = 1;
        return;
    }
    val = *vr->pvalue;
    if (mode == 0) {
        if (unlikely(JS_IsUninitialized(val))) {
            JS_ThrowReferenceErrorUninitialized2(st->ctx, st->b, idx, TRUE);
            st->exception = 1;
            return;
        }
        *st->sp++ = JS_DupValue(st->ctx, val);
    } else if (mode == 1) {
        if (unlikely(JS_IsUninitialized(val))) {
            JS_ThrowReferenceErrorUninitialized2(st->ctx, st->b, idx, TRUE);
            st->exception = 1;
            return;
        }
        set_value(st->ctx, vr->pvalue, st->sp[-1]);
        st->sp--;
    } else {
        if (unlikely(!JS_IsUninitialized(val))) {
            JS_ThrowReferenceError(st->ctx, "'this' can be initialized only once");
            st->exception = 1;
            return;
        }
        set_value(st->ctx, vr->pvalue, st->sp[-1]);
        st->sp--;
    }
}

static void h_close_loc(FlameJitState *st)
{
    int idx = (int)st->param;
    close_lexical_var(st->ctx, st->sfp, idx);
}

static void h_define_var(FlameJitState *st)
{
    if (JS_DefineGlobalVar(st->ctx, (JSAtom)st->param, (int)st->param2))
        st->exception = 1;
}

static void h_check_define_var(FlameJitState *st)
{
    if (JS_CheckDefineGlobalVar(st->ctx, (JSAtom)st->param, (int)st->param2))
        st->exception = 1;
}

static void h_define_func(FlameJitState *st)
{
    if (JS_DefineGlobalFunction(st->ctx, (JSAtom)st->param, st->sp[-1],
                                (int)st->param2))
        st->exception = 1;
    JS_FreeValue(st->ctx, st->sp[-1]);
    st->sp--;
}

static void h_put_loc(FlameJitState *st)
{
    int idx = (int)st->param;
    JSValue *pv = st->param2 ? &st->sfp->arg_buf[idx] : &st->sfp->var_buf[idx];
    set_value(st->ctx, pv, st->sp[-1]);
    st->sp--;
}

static void h_set_loc(FlameJitState *st)
{
    int idx = (int)st->param;
    JSValue *pv = st->param2 ? &st->sfp->arg_buf[idx] : &st->sfp->var_buf[idx];
    set_value(st->ctx, pv, st->sp[-1]);
}

#define FL_MAX_OSR_HEADERS 8   /* loop-header OSR entries per function;
                                  enough for deeply nested loop nests */

typedef struct FlameCmp {
    FlameEmitter e;
    JSFunctionBytecode *b;
    struct FlameProfile *prof; /* tier-1 feedback (tier 3) */
    uint8_t tier;              /* 2 = baseline, 3 = optimizing */
    int32_t *labels;      /* bc offset -> label, -1 */
    size_t *nat;          /* bc offset -> native code offset (emit map) */
    uint32_t cur_off;     /* bytecode offset currently being emitted */
    uint32_t osr_bc[FL_MAX_OSR_HEADERS];   /* backward-jump targets */
    int osr_count;
    /* out-of-line deopt trampolines: (label, resume point) pairs, emitted
       after the body so the fast paths fall through to the next opcode */
    struct { int label; uint32_t bc_off; } deopt_pending[64];
    int deopt_count;
    uint32_t pending_param;  /* per-site constants for the next helper */
    uint32_t pending_param2;
    int has_pending;
    int epilogue;            /* native epilogue label */
} FlameCmp;

static void fc_set_param(FlameCmp *c, uint32_t p1, uint32_t p2)
{
    c->pending_param = p1;
    c->pending_param2 = p2;
    c->has_pending = 1;
}

static void fc_call(FlameCmp *c, void *fn)
{
    FlameEmitter *e = &c->e;
    if (c->has_pending) {
        fl_mov32_ri(e, FL_RTMP2, c->pending_param);
        fl_mov32_mr(e, FL_RSTATE, FL_OFF_PARAM, FL_RTMP2);
        fl_mov32_ri(e, FL_RTMP2, c->pending_param2);
        fl_mov32_mr(e, FL_RSTATE, FL_OFF_PARAM2, FL_RTMP2);
        c->has_pending = 0;
    }
    /* helpers push/pop the JS stack: publish r15 -> st->sp before the call
       and reload it after (both paths) */
    fl_mov_mr(e, FL_RSTATE, FL_OFF_SP, FL_RSP);
    fl_call(e, fn, FL_RSTATE);
    fl_mov_rm(e, FL_RSP, FL_RSTATE, FL_OFF_SP);
    fl_mov32_rm(e, FL_RTMP2, FL_RSTATE, FL_OFF_EXCEPTION);
    fl_cmp_ri(e, FL_RTMP2, 0);
    fl_jcc(e, FL_NE, c->epilogue);
}

/* tier-3 guard failure: jump to this site's out-of-line trampoline
   (emitted after the body), which records the resume point and exits to
   the wrapper; the wrapper hands the live frame to the interpreter
   (flame_deopt_resume), which redoes the op from this exact state */
static int fc_deopt(FlameCmp *c, uint32_t bc_off)
{
    int lbl;
    if (c->deopt_count >= 64) {
        c->e.ok = 0;
        return 0;
    }
    lbl = fl_label(&c->e);
    c->deopt_pending[c->deopt_count].label = lbl;
    c->deopt_pending[c->deopt_count].bc_off = bc_off;
    c->deopt_count++;
    return lbl;
}

/* element-kind feedback for a get_array_el site (from the tier-1 ICs) */
static int fc_elem_feedback(FlameCmp *c, uint32_t bc_off)
{
    int i;
    if (!c->prof)
        return -1;
    for (i = 0; i < c->prof->ic_count; i++)
        if (c->prof->ics[i].kind == FLAME_IC_ELEM &&
            c->prof->ics[i].pc_off == bc_off &&
            c->prof->ics[i].elem_kind != FLAME_KIND_NONE)
            return c->prof->ics[i].elem_kind;
    return -1;
}

/* feedback byte at a bytecode offset: (lhs<<4)|rhs FlameKind, or -1 */
static int fc_feedback(FlameCmp *c, uint32_t bc_off)
{
    if (!c->prof || !c->prof->binop_fb ||
        bc_off >= (uint32_t)c->prof->binop_fb_len)
        return -1;
    return c->prof->binop_fb[bc_off];
}

/* free the JSValue in st->retval (helper call keeps full JS_FreeValue
   semantics: finalizers, atom refs, etc.) */
static void h_free_retval(FlameJitState *st)
{
    JS_FreeValue(st->ctx, st->retval);
}

static void fc_free_slot(FlameCmp *c, FlReg base, int32_t disp)
{
    fl_mov_rm(&c->e, FL_RTMP1, base, disp + FL_OFF_U);
    fl_mov_mr(&c->e, FL_RSTATE, FL_OFF_RETVAL_U, FL_RTMP1);
    fl_mov_rm(&c->e, FL_RTMP1, base, disp + FL_OFF_TAG);
    fl_mov_mr(&c->e, FL_RSTATE, FL_OFF_RETVAL_T, FL_RTMP1);
    fl_call(&c->e, (void *)h_free_retval, FL_RSTATE);
}

/* bump the refcount of the JSValue at [base+disp] when pointer-like
   (mirrors JS_DupValue: only tags < 0 hold references) */
static void fc_dup_slot(FlameCmp *c, FlReg base, int32_t disp)
{
    FlameEmitter *e = &c->e;
    int done = fl_label(e);
    fl_mov_rm(e, FL_RTMP0, base, disp + FL_OFF_TAG);
    fl_cmp_ri(e, FL_RTMP0, 0);
    fl_jcc(e, FL_GE, done);
    fl_mov_rm(e, FL_RTMP1, base, disp + FL_OFF_U);
    {
        int r = fl_reg_x64[FL_RTMP1];
        if (r >= 8)
            fl_emit8(e, 0x41);
        fl_emit8(e, 0xFF);
        fl_emit8(e, (uint8_t)(r & 7));
    }
    fl_bind(e, done);
}

/* update sf->cur_pc = bc_buf + off (for exception backtraces) */
static void fc_set_cur_pc(FlameCmp *c, uint32_t bc_off)
{
    fl_mov_ri32(&c->e, FL_RTMP2, (int32_t)bc_off);
    fl_mov_rm(&c->e, FL_RTMP1, FL_RSTATE, FL_OFF_BC_BUF);
    fl_add_rr(&c->e, FL_RTMP1, FL_RTMP2);
    fl_mov_rm(&c->e, FL_RTMP0, FL_RSTATE, FL_OFF_SFP);
    fl_mov_mr(&c->e, FL_RTMP0, FL_SF_CUR_PC, FL_RTMP1);
}

static void fc_copy_slot(FlameCmp *c, FlReg sbase, int32_t sdisp,
                         FlReg dbase, int32_t ddisp)
{
    fl_mov_rm(&c->e, FL_RTMP0, sbase, sdisp);
    fl_mov_rm(&c->e, FL_RTMP1, sbase, sdisp + 8);
    fl_mov_mr(&c->e, dbase, ddisp, FL_RTMP0);
    fl_mov_mr(&c->e, dbase, ddisp + 8, FL_RTMP1);
}

static void fc_push_int(FlameCmp *c, int32_t v)
{
    fl_mov32_ri(&c->e, FL_RTMP0, (uint32_t)v);
    fl_mov32_mr(&c->e, FL_RSP, FL_OFF_U, FL_RTMP0);
    fl_mov32_ri(&c->e, FL_RTMP0, 0);
    fl_mov_mr(&c->e, FL_RSP, FL_OFF_TAG, FL_RTMP0);
    fl_add_ri(&c->e, FL_RSP, FL_SZ_VALUE);
}

static void fc_store_bool(FlameCmp *c, FlReg base, int32_t disp, uint32_t b)
{
    fl_mov32_ri(&c->e, FL_RTMP0, b);
    fl_mov32_mr(&c->e, base, disp + FL_OFF_U, FL_RTMP0);
    fl_mov32_ri(&c->e, FL_RTMP0, FL_TAG_BOOL);
    fl_mov_mr(&c->e, base, disp + FL_OFF_TAG, FL_RTMP0);
}

/* tier-3 speculative binop: consult the tier-1 feedback; when the site saw
   a single consistent kind pair, emit an inline guarded fast path whose
   only fallback is deoptimization (the interpreter redoes the op) */
static int fc_label_at(FlameCmp *c, uint32_t bc_off)
{
    if (bc_off > (uint32_t)c->b->byte_code_len)
        return -1;
    if (c->labels[bc_off] < 0) {
        c->labels[bc_off] = fl_label(&c->e);
        if (bc_off <= c->cur_off) {
            /* backward target: its native code was already emitted */
            fl_bind_offset(&c->e, c->labels[bc_off], c->nat[bc_off]);
            /* loop header: record as an OSR entry point */
            if (c->osr_count < FL_MAX_OSR_HEADERS)
                c->osr_bc[c->osr_count++] = bc_off;
        }
    }
    return c->labels[bc_off];
}

static void fc_bind_at(FlameCmp *c, uint32_t bc_off)
{
    if (bc_off <= (uint32_t)c->b->byte_code_len &&
        c->labels[bc_off] >= 0 &&
        !c->e.labels[c->labels[bc_off]].bound)
        fl_bind(&c->e, c->labels[bc_off]);
}

static void fc_emit_binop(FlameCmp *c, uint32_t bc_off, int op)
{
    FlameEmitter *e = &c->e;
    int fb, lhs, rhs;

    if (c->tier != 3) {
        fc_set_param(c, (uint32_t)op, bc_off);
        fc_call(c, (void *)h_binop);
        return;
    }
    fb = fc_feedback(c, bc_off);
    if (fb < 0) {
        fc_set_param(c, (uint32_t)op, bc_off);
        fc_call(c, (void *)h_binop);
        return;
    }
    lhs = fb & 0xF;
    rhs = (fb >> 4) & 0xF;

    /* guards jump to the deopt trampoline; the fast path falls through to
       the next opcode (the trampolines live out of line, after the body) */
    if (lhs == FLAME_KIND_INT && rhs == FLAME_KIND_INT &&
        (op == OP_add || op == OP_sub || op == OP_mul)) {
        int dep = fc_deopt(c, bc_off);
        int store = fl_label(e);
        /* guard: both tags INT */
        fl_mov_rm(e, FL_RTMP0, FL_RSP, -2 * FL_SZ_VALUE + FL_OFF_TAG);
        fl_cmp_ri(e, FL_RTMP0, FL_TAG_INT);
        fl_jcc(e, FL_NE, dep);
        fl_mov_rm(e, FL_RTMP0, FL_RSP, -FL_SZ_VALUE + FL_OFF_TAG);
        fl_cmp_ri(e, FL_RTMP0, FL_TAG_INT);
        fl_jcc(e, FL_NE, dep);
        fl_mov32_rm(e, FL_RTMP1, FL_RSP, -2 * FL_SZ_VALUE);
        fl_mov32_rm(e, FL_RTMP2, FL_RSP, -FL_SZ_VALUE);
        if (op == OP_add)
            fl_add32_rr(e, FL_RTMP1, FL_RTMP2);
        else if (op == OP_sub)
            fl_sub32_rr(e, FL_RTMP1, FL_RTMP2);
        else
            fl_mul32_rr(e, FL_RTMP1, FL_RTMP2);
        /* int32 overflow -> double semantics -> deopt */
        fl_jcc(e, FL_VS, dep);
        if (op == OP_mul) {
            /* -0 result: r == 0 && (lhs < 0 || rhs < 0) -> deopt */
            fl_cmp_ri(e, FL_RTMP1, 0);
            fl_jcc(e, FL_NE, store);
            fl_mov32_rm(e, FL_RTMP0, FL_RSP, -2 * FL_SZ_VALUE);
            fl_cmp_ri(e, FL_RTMP0, 0);
            fl_jcc(e, FL_LT, dep);
            fl_cmp_ri(e, FL_RTMP2, 0);
            fl_jcc(e, FL_LT, dep);
        }
        fl_bind(e, store);
        fl_mov32_mr(e, FL_RSP, -2 * FL_SZ_VALUE, FL_RTMP1);
        fl_add_ri(e, FL_RSP, -FL_SZ_VALUE);
        return;
    }
    if (lhs == FLAME_KIND_FLOAT && rhs == FLAME_KIND_FLOAT) {
        int dep = fc_deopt(c, bc_off);
        /* guard: both tags FLOAT64 */
        fl_mov_rm(e, FL_RTMP0, FL_RSP, -2 * FL_SZ_VALUE + FL_OFF_TAG);
        fl_cmp_ri(e, FL_RTMP0, FL_TAG_FLOAT64);
        fl_jcc(e, FL_NE, dep);
        fl_mov_rm(e, FL_RTMP0, FL_RSP, -FL_SZ_VALUE + FL_OFF_TAG);
        fl_cmp_ri(e, FL_RTMP0, FL_TAG_FLOAT64);
        fl_jcc(e, FL_NE, dep);
        fl_fload(e, 0, FL_RSP, -2 * FL_SZ_VALUE);
        fl_fload(e, 1, FL_RSP, -FL_SZ_VALUE);
        switch (op) {
        case OP_add: fl_fadd(e, 0, 1); break;
        case OP_sub: fl_fsub(e, 0, 1); break;
        case OP_mul: fl_fmul(e, 0, 1); break;
        case OP_div: fl_fdiv(e, 0, 1); break;
        default:
            fc_set_param(c, (uint32_t)op, bc_off);
            fc_call(c, (void *)h_binop);
            return;
        }
        fl_fstore(e, 0, FL_RSP, -2 * FL_SZ_VALUE);
        fl_mov32_ri(e, FL_RTMP0, FL_TAG_FLOAT64);
        fl_mov_mr(e, FL_RSP, -2 * FL_SZ_VALUE + FL_OFF_TAG, FL_RTMP0);
        fl_add_ri(e, FL_RSP, -FL_SZ_VALUE);
        return;
    }
    /* mixed / unknown feedback: baseline helper (always correct) */
    fc_set_param(c, (uint32_t)op, bc_off);
    fc_call(c, (void *)h_binop);
}

static void fc_emit_relational(FlameCmp *c, uint32_t bc_off, int op)
{
    FlameEmitter *e = &c->e;
    int fb, lhs, rhs;
    FlCond c1;

    if (c->tier != 3) {
        fc_set_param(c, (uint32_t)op, bc_off);
        fc_call(c, (void *)h_relational);
        return;
    }
    fb = fc_feedback(c, bc_off);
    lhs = fb >= 0 ? (fb & 0xF) : -1;
    rhs = fb >= 0 ? ((fb >> 4) & 0xF) : -1;
    if (lhs != FLAME_KIND_INT || rhs != FLAME_KIND_INT) {
        fc_set_param(c, (uint32_t)op, bc_off);
        fc_call(c, (void *)h_relational);
        return;
    }
    switch (op) {
    case OP_lt: c1 = FL_LT; break;
    case OP_gt: c1 = FL_GT; break;
    default:
        fc_set_param(c, (uint32_t)op, bc_off);
        fc_call(c, (void *)h_relational);
        return;
    }
    {
        int dep = fc_deopt(c, bc_off);
        int lt = fl_label(e), ge = fl_label(e);
        fl_mov_rm(e, FL_RTMP0, FL_RSP, -2 * FL_SZ_VALUE + FL_OFF_TAG);
        fl_cmp_ri(e, FL_RTMP0, FL_TAG_INT);
        fl_jcc(e, FL_NE, dep);
        fl_mov_rm(e, FL_RTMP0, FL_RSP, -FL_SZ_VALUE + FL_OFF_TAG);
        fl_cmp_ri(e, FL_RTMP0, FL_TAG_INT);
        fl_jcc(e, FL_NE, dep);
        fl_mov32_rm(e, FL_RTMP1, FL_RSP, -2 * FL_SZ_VALUE);
        fl_mov32_rm(e, FL_RTMP2, FL_RSP, -FL_SZ_VALUE);
        fl_cv64_32(e, FL_RTMP1, FL_RTMP1);
        fl_cv64_32(e, FL_RTMP2, FL_RTMP2);
        fl_cmp_rr(e, FL_RTMP1, FL_RTMP2);
        fl_jcc(e, c1, lt);
        fl_jmp(e, ge);
        fl_bind(e, lt);
        fc_store_bool(c, FL_RSP, -2 * FL_SZ_VALUE, 1);
        fl_jmp(e, ge);
        fl_bind(e, ge);
        fc_store_bool(c, FL_RSP, -2 * FL_SZ_VALUE, 0);
        fl_add_ri(e, FL_RSP, -FL_SZ_VALUE);
    }
}

static void fc_emit_eq(FlameCmp *c, uint32_t bc_off, int op)
{
    FlameEmitter *e = &c->e;
    int fb, lhs, rhs, neg;
    int strict = (op == OP_strict_eq || op == OP_strict_neq);

    if (c->tier != 3 || strict) {
        fc_set_param(c, (uint32_t)op, bc_off);
        fc_call(c, strict ? (void *)h_strict_eq : (void *)h_eq);
        return;
    }
    fb = fc_feedback(c, bc_off);
    lhs = fb >= 0 ? (fb & 0xF) : -1;
    rhs = fb >= 0 ? ((fb >> 4) & 0xF) : -1;
    if (lhs != FLAME_KIND_INT || rhs != FLAME_KIND_INT) {
        fc_set_param(c, (uint32_t)op, bc_off);
        fc_call(c, (void *)h_eq);
        return;
    }
    neg = (op == OP_neq);
    {
        int dep = fc_deopt(c, bc_off);
        int t = fl_label(e), f = fl_label(e);
        fl_mov_rm(e, FL_RTMP0, FL_RSP, -2 * FL_SZ_VALUE + FL_OFF_TAG);
        fl_cmp_ri(e, FL_RTMP0, FL_TAG_INT);
        fl_jcc(e, FL_NE, dep);
        fl_mov_rm(e, FL_RTMP0, FL_RSP, -FL_SZ_VALUE + FL_OFF_TAG);
        fl_cmp_ri(e, FL_RTMP0, FL_TAG_INT);
        fl_jcc(e, FL_NE, dep);
        fl_mov32_rm(e, FL_RTMP1, FL_RSP, -2 * FL_SZ_VALUE);
        fl_mov32_rm(e, FL_RTMP2, FL_RSP, -FL_SZ_VALUE);
        fl_cmp_rr(e, FL_RTMP1, FL_RTMP2);
        fl_jcc(e, neg ? FL_NE : FL_EQ, t);
        fl_jmp(e, f);
        fl_bind(e, t);
        fc_store_bool(c, FL_RSP, -2 * FL_SZ_VALUE, 1);
        fl_jmp(e, f);
        fl_bind(e, f);
        fc_store_bool(c, FL_RSP, -2 * FL_SZ_VALUE, 0);
        fl_add_ri(e, FL_RSP, -FL_SZ_VALUE);
    }
}

static void fc_emit_logic(FlameCmp *c, uint32_t bc_off, int op)
{
    fc_set_param(c, (uint32_t)op, bc_off);
    fc_call(c, (void *)h_logic);
}

/* tier-3 speculative dense-array element load (OP_get_array_el).
   Guards: object tag, class Array/Arguments, int index, in bounds,
   and idx < FL_MAX_DENSE_IDX (so idx*16 can't overflow int32).
   Any mismatch deoptimizes; the interpreter redoes the access. */
static void fc_emit_array_el(FlameCmp *c, uint32_t bc_off)
{
    FlameEmitter *e = &c->e;
    int dep = fc_deopt(c, bc_off);
    int class_ok = fl_label(e);

    fl_mov_rm(e, FL_RTMP0, FL_RSP, -2 * FL_SZ_VALUE + FL_OFF_TAG);
    fl_cmp_ri(e, FL_RTMP0, FL_TAG_OBJECT);
    fl_jcc(e, FL_NE, dep);
    fl_mov_rm(e, FL_RTMP1, FL_RSP, -2 * FL_SZ_VALUE + FL_OFF_U);
    fl_mov32_rm(e, FL_RTMP2, FL_RTMP1, FL_OFF_CLASS_ID);
    fl_cmp_ri(e, FL_RTMP2, JS_CLASS_ARRAY);
    fl_jcc(e, FL_EQ, class_ok);
    fl_cmp_ri(e, FL_RTMP2, JS_CLASS_ARGUMENTS);
    fl_jcc(e, FL_NE, dep);
    fl_bind(e, class_ok);
    /* index: int32, 0 <= idx <= FL_MAX_DENSE_IDX, idx < count */
    fl_mov32_rm(e, FL_RTMP0, FL_RSP, -FL_SZ_VALUE + FL_OFF_U);
    fl_cmp_ri(e, FL_RTMP0, 0);
    fl_jcc(e, FL_LT, dep);                  /* negative index */
    fl_cmp_ri(e, FL_RTMP0, FL_MAX_DENSE_IDX);
    fl_jcc(e, FL_GT, dep);                  /* shift would overflow int32 */
    fl_mov32_rm(e, FL_RTMP2, FL_RTMP1, FL_OFF_ARR_COUNT);
    fl_cmp_rr(e, FL_RTMP2, FL_RTMP0); /* count vs idx */
    fl_jcc(e, FL_UBE, dep);           /* count <= idx -> OOB */
    /* addr = values + idx*16 */
    fl_shl32_ri(e, FL_RTMP0, 4);
    fl_cv64_32(e, FL_RTMP0, FL_RTMP0);
    fl_mov_rm(e, FL_RTMP1, FL_RTMP1, FL_OFF_ARR_VALUES);
    fl_add_rr(e, FL_RTMP0, FL_RTMP1);
    fl_mov_rm(e, FL_RTMP1, FL_RTMP0, FL_OFF_U);
    fl_mov_rm(e, FL_RTMP2, FL_RTMP0, FL_OFF_TAG);
    fl_mov_mr(e, FL_RSP, -2 * FL_SZ_VALUE + FL_OFF_U, FL_RTMP1);
    fl_mov_mr(e, FL_RSP, -2 * FL_SZ_VALUE + FL_OFF_TAG, FL_RTMP2);
    fc_dup_slot(c, FL_RSP, -2 * FL_SZ_VALUE);
    /* drop the object slot's reference with full free semantics */
    fc_free_slot(c, FL_RSP, -FL_SZ_VALUE);
    fl_add_ri(e, FL_RSP, -2 * FL_SZ_VALUE);
}

/* if_true / if_false with native branch decision */
static void fc_emit_if(FlameCmp *c, uint32_t bc_off, uint32_t target,
                       int branch_if_true)
{
    FlameEmitter *e = &c->e;
    int tgt = fc_label_at(c, target);
    int fast_tag = fl_label(e);
    int decided = fl_label(e);
    FlCond cond = branch_if_true ? FL_NE : FL_EQ;

    fl_mov_rm(e, FL_RTMP0, FL_RSP, -FL_SZ_VALUE + FL_OFF_TAG);
    fl_cmp_ri(e, FL_RTMP0, FL_TAG_UNDEFINED);
    fl_jcc(e, FL_UBE, fast_tag);

    /* slow path: helper converts + pops */
    fc_set_param(c, 0, bc_off);
    fc_call(c, (void *)h_to_bool);
    fl_mov32_rm(e, FL_RTMP1, FL_RSTATE, FL_OFF_PARAM2);
    fl_cmp_ri(e, FL_RTMP1, 0);
    fl_jcc(e, cond, tgt);
    fl_jmp(e, decided);

    fl_bind(e, fast_tag);
    fl_mov32_rm(e, FL_RTMP1, FL_RSP, -FL_SZ_VALUE + FL_OFF_U);
    fl_add_ri(e, FL_RSP, -FL_SZ_VALUE);
    fl_cmp_ri(e, FL_RTMP1, 0);
    fl_jcc(e, cond, tgt);
    fl_bind(e, decided);
}



static int baseline_whitelist_ok(JSFunctionBytecode *b)
{
    const uint8_t *pc = b->byte_code_buf;
    const uint8_t *end = pc + b->byte_code_len;
    while (pc < end) {
        uint8_t op = *pc++;
        uint32_t bc_off = (uint32_t)(pc - 1 - b->byte_code_buf);
        switch (op) {
        case OP_push_i32: case OP_push_const: case OP_fclosure:
        case OP_push_atom_value: case OP_undefined: case OP_null:
        case OP_push_this: case OP_push_false: case OP_push_true:
        case OP_drop: case OP_nip: case OP_nip1: case OP_dup: case OP_dup1:
        case OP_dup2: case OP_dup3: case OP_insert2: case OP_insert3:
        case OP_insert4: case OP_perm3: case OP_perm4: case OP_perm5:
        case OP_swap: case OP_swap2: case OP_rot3l: case OP_rot3r:
        case OP_rot4l: case OP_rot5l:
        case OP_call: case OP_tail_call: case OP_call_method:
        case OP_tail_call_method:
        case OP_return: case OP_return_undef: case OP_throw:
        case OP_get_var_undef: case OP_get_var: case OP_put_var:
        case OP_put_var_init: case OP_put_var_strict:
        case OP_get_field: case OP_get_field2: case OP_put_field:
        case OP_get_array_el: case OP_get_array_el2: case OP_put_array_el:
        case OP_get_loc: case OP_put_loc: case OP_set_loc:
        case OP_get_arg: case OP_put_arg: case OP_set_arg:
        case OP_get_var_ref: case OP_put_var_ref: case OP_set_var_ref:
        case OP_if_false: case OP_if_true: case OP_goto:
        case OP_mul: case OP_div: case OP_mod: case OP_add: case OP_sub:
        case OP_lt: case OP_lte: case OP_gt: case OP_gte:
        case OP_eq: case OP_neq: case OP_strict_eq: case OP_strict_neq:
        case OP_and: case OP_xor: case OP_or:
        case OP_shl: case OP_sar: case OP_shr:
        case OP_not: case OP_lnot: case OP_typeof:
        case OP_neg: case OP_plus: case OP_inc: case OP_dec:
        case OP_post_inc: case OP_post_dec:
        case OP_dec_loc: case OP_inc_loc: case OP_add_loc:
        case OP_nop: case OP_is_undefined_or_null:
        case OP_push_minus1: case OP_push_0: case OP_push_1: case OP_push_2:
        case OP_push_3: case OP_push_4: case OP_push_5: case OP_push_6:
        case OP_push_7: case OP_push_i8: case OP_push_i16:
        case OP_push_const8: case OP_fclosure8: case OP_push_empty_string:
        case OP_get_loc8: case OP_put_loc8: case OP_set_loc8:
        case OP_get_length:
        case OP_if_false8: case OP_if_true8: case OP_goto8: case OP_goto16:
        case OP_call0: case OP_call1: case OP_call2: case OP_call3:
        case OP_is_undefined: case OP_is_null:
        case OP_typeof_is_undefined: case OP_typeof_is_function:
        case OP_get_loc_check: case OP_put_loc_check:
        case OP_put_loc_check_init: case OP_get_loc_checkthis:
        case OP_get_var_ref_check: case OP_put_var_ref_check:
        case OP_put_var_ref_check_init: case OP_close_loc:
        case OP_set_loc_uninitialized:
        case OP_define_var: case OP_check_define_var: case OP_define_func:
        case OP_get_loc0: case OP_get_loc1: case OP_get_loc2:
        case OP_get_loc3: case OP_put_loc0: case OP_put_loc1:
        case OP_put_loc2: case OP_put_loc3: case OP_set_loc0:
        case OP_set_loc1: case OP_set_loc2: case OP_set_loc3:
        case OP_get_arg0: case OP_get_arg1: case OP_get_arg2:
        case OP_get_arg3: case OP_put_arg0: case OP_put_arg1:
        case OP_put_arg2: case OP_put_arg3: case OP_set_arg0:
        case OP_set_arg1: case OP_set_arg2: case OP_set_arg3:
        case OP_get_var_ref0: case OP_get_var_ref1: case OP_get_var_ref2:
        case OP_get_var_ref3: case OP_put_var_ref0: case OP_put_var_ref1:
        case OP_put_var_ref2: case OP_put_var_ref3: case OP_set_var_ref0:
        case OP_set_var_ref1: case OP_set_var_ref2: case OP_set_var_ref3:
            break;
        default:
            flame_log(FLAME_LOG_VERBOSE,
                      "unsupported opcode %d (0x%02x) at bc:%u",
                      op, op, bc_off);
            return 0;
        }
        pc += short_opcode_info(op).size - 1;
    }
    return pc == end;
}

static void emit_one(FlameCmp *c, const uint8_t *pc, uint8_t op,
                     uint32_t bc_off)
{
    FlameEmitter *e = &c->e;
    JSFunctionBytecode *b = c->b;
    const uint8_t *imm = pc + 1;
    uint32_t u32;
    int32_t s32;

#define U32() (u32 = get_u32(imm), imm += 4, u32)
#define U16() (u32 = get_u16(imm), imm += 2, u32)
#define I16() (s32 = (int16_t)get_u16(imm), imm += 2, s32)
#define I32() (s32 = get_u32(imm), imm += 4, s32)
#define U8()  (u32 = *imm++, u32)
#define I8()  (s32 = (int8_t)*imm++, s32)

    switch (op) {
    case OP_push_i32:
        fc_push_int(c, I32());
        break;
    case OP_push_minus1: fc_push_int(c, -1); break;
    case OP_push_0: fc_push_int(c, 0); break;
    case OP_push_1: fc_push_int(c, 1); break;
    case OP_push_2: fc_push_int(c, 2); break;
    case OP_push_3: fc_push_int(c, 3); break;
    case OP_push_4: fc_push_int(c, 4); break;
    case OP_push_5: fc_push_int(c, 5); break;
    case OP_push_6: fc_push_int(c, 6); break;
    case OP_push_7: fc_push_int(c, 7); break;
    case OP_push_i8: fc_push_int(c, I8()); break;
    case OP_push_i16: fc_push_int(c, I16()); break;

    case OP_push_const:
    case OP_push_const8:
        if (op == OP_push_const8) U8(); else U32();
        /* load cpool[idx] (16 bytes) and push (dup) */
        fl_mov_rm(e, FL_RTMP0, FL_RSTATE, FL_OFF_CPOOL);
        fl_mov_rm(e, FL_RTMP1, FL_RTMP0, (int32_t)(u32 * FL_SZ_VALUE));
        fl_mov_mr(e, FL_RSP, 0, FL_RTMP1);
        fl_mov_rm(e, FL_RTMP1, FL_RTMP0, (int32_t)(u32 * FL_SZ_VALUE + 8));
        fl_mov_mr(e, FL_RSP, 8, FL_RTMP1);
        fc_dup_slot(c, FL_RSP, 0);
        fl_add_ri(e, FL_RSP, FL_SZ_VALUE);
        break;

    case OP_fclosure:
    case OP_fclosure8:
        if (op == OP_fclosure8) U8(); else U32();
        fc_set_param(c, u32, bc_off);
        fc_call(c, (void *)h_fclosure);
        break;

    case OP_push_atom_value:
        fc_set_param(c, U32(), bc_off);
        fc_call(c, (void *)h_push_atom);
        break;
    case OP_push_empty_string:
        fc_call(c, (void *)h_push_empty_string);
        break;

    case OP_undefined:
        fc_push_int(c, 0);
        fl_mov32_ri(e, FL_RTMP0, FL_TAG_UNDEFINED);
        fl_mov_mr(e, FL_RSP, -FL_SZ_VALUE + FL_OFF_TAG, FL_RTMP0);
        break;
    case OP_null:
        fc_push_int(c, 0);
        fl_mov32_ri(e, FL_RTMP0, FL_TAG_NULL);
        fl_mov_mr(e, FL_RSP, -FL_SZ_VALUE + FL_OFF_TAG, FL_RTMP0);
        break;
    case OP_push_false:
        fc_push_int(c, 0);
        fl_mov32_ri(e, FL_RTMP0, FL_TAG_BOOL);
        fl_mov_mr(e, FL_RSP, -FL_SZ_VALUE + FL_OFF_TAG, FL_RTMP0);
        break;
    case OP_push_true:
        fc_push_int(c, 1);
        fl_mov32_ri(e, FL_RTMP0, FL_TAG_BOOL);
        fl_mov_mr(e, FL_RSP, -FL_SZ_VALUE + FL_OFF_TAG, FL_RTMP0);
        break;
    case OP_push_this:
        fc_set_param(c, 0, bc_off);
        fc_call(c, (void *)h_push_this);
        break;

    case OP_get_loc0: case OP_get_loc1: case OP_get_loc2: case OP_get_loc3:
        fc_copy_slot(c, FL_RLOC, (b->arg_count + op - OP_get_loc0) *
                     FL_SZ_VALUE, FL_RSP, 0);
        fc_dup_slot(c, FL_RSP, 0);
        fl_add_ri(e, FL_RSP, FL_SZ_VALUE);
        break;
    case OP_put_loc0: case OP_put_loc1: case OP_put_loc2: case OP_put_loc3:
        fc_set_param(c, op - OP_put_loc0, 0);
        fc_call(c, (void *)h_put_loc);
        break;
    case OP_set_loc0: case OP_set_loc1: case OP_set_loc2: case OP_set_loc3:
        fc_set_param(c, op - OP_set_loc0, 0);
        fc_call(c, (void *)h_set_loc);
        break;
    case OP_get_arg0: case OP_get_arg1: case OP_get_arg2: case OP_get_arg3:
        fc_copy_slot(c, FL_RLOC, (op - OP_get_arg0) * FL_SZ_VALUE,
                     FL_RSP, 0);
        fc_dup_slot(c, FL_RSP, 0);
        fl_add_ri(e, FL_RSP, FL_SZ_VALUE);
        break;
    case OP_put_arg0: case OP_put_arg1: case OP_put_arg2: case OP_put_arg3:
        fc_set_param(c, op - OP_put_arg0, 1);
        fc_call(c, (void *)h_put_loc);
        break;
    case OP_set_arg0: case OP_set_arg1: case OP_set_arg2: case OP_set_arg3:
        fc_set_param(c, op - OP_set_arg0, 1);
        fc_call(c, (void *)h_set_loc);
        break;
    case OP_get_loc: case OP_get_loc8:
        if (op == OP_get_loc8) U8(); else U16();
        fc_copy_slot(c, FL_RLOC, (int32_t)((b->arg_count + u32) * FL_SZ_VALUE),
                     FL_RSP, 0);
        fc_dup_slot(c, FL_RSP, 0);
        fl_add_ri(e, FL_RSP, FL_SZ_VALUE);
        break;
    case OP_put_loc: case OP_put_loc8:
        if (op == OP_put_loc8) U8(); else U16();
        fc_set_param(c, u32, 0);
        fc_call(c, (void *)h_put_loc);
        break;
    case OP_set_loc: case OP_set_loc8:
        if (op == OP_set_loc8) U8(); else U16();
        fc_set_param(c, u32, 0);
        fc_call(c, (void *)h_set_loc);
        break;
    case OP_get_arg: case OP_put_arg: case OP_set_arg:
        U16();
        if (op == OP_get_arg) {
            fc_copy_slot(c, FL_RLOC, (int32_t)(u32 * FL_SZ_VALUE),
                         FL_RSP, 0);
            fc_dup_slot(c, FL_RSP, 0);
            fl_add_ri(e, FL_RSP, FL_SZ_VALUE);
        } else if (op == OP_put_arg) {
            fc_set_param(c, u32, 1);
            fc_call(c, (void *)h_put_loc);
        } else {
            fc_set_param(c, u32, 1);
            fc_call(c, (void *)h_set_loc);
        }
        break;

    case OP_get_var_ref: case OP_put_var_ref: case OP_set_var_ref:
        U16();
        fc_set_param(c, u32,
                     op == OP_get_var_ref ? 0 :
                     op == OP_put_var_ref ? 1 : 2);
        fc_call(c, (void *)h_var_ref);
        break;
    case OP_get_var_ref0: case OP_get_var_ref1:
    case OP_get_var_ref2: case OP_get_var_ref3:
        fc_set_param(c, op - OP_get_var_ref0, 0);
        fc_call(c, (void *)h_var_ref);
        break;
    case OP_put_var_ref0: case OP_put_var_ref1:
    case OP_put_var_ref2: case OP_put_var_ref3:
        fc_set_param(c, op - OP_put_var_ref0, 1);
        fc_call(c, (void *)h_var_ref);
        break;
    case OP_set_var_ref0: case OP_set_var_ref1:
    case OP_set_var_ref2: case OP_set_var_ref3:
        fc_set_param(c, op - OP_set_var_ref0, 2);
        fc_call(c, (void *)h_var_ref);
        break;
    case OP_set_loc_uninitialized:
        U16();
        /* store JS_UNINITIALIZED into the var slot (no stack traffic) */
        u32 += b->arg_count;
        fl_mov32_ri(e, FL_RTMP0, FL_TAG_UNINITIALIZED);
        fl_mov_mr(e, FL_RLOC, (int32_t)(u32 * FL_SZ_VALUE + FL_OFF_TAG),
                  FL_RTMP0);
        fl_mov32_ri(e, FL_RTMP0, 0);
        fl_mov32_mr(e, FL_RLOC, (int32_t)(u32 * FL_SZ_VALUE + FL_OFF_U),
                    FL_RTMP0);
        break;

    case OP_get_var: case OP_get_var_undef:
        fc_set_param(c, U32(), op == OP_get_var ? 1 : 0);
        fc_call(c, (void *)h_get_var);
        break;
    case OP_put_var: case OP_put_var_init: case OP_put_var_strict:
        fc_set_param(c, U32(),
                     op == OP_put_var ? 0 :
                     op == OP_put_var_init ? 1 : 2);
        fc_call(c, (void *)h_put_var);
        break;

    case OP_get_field: case OP_get_field2:
        fc_set_param(c, U32(), bc_off);
        fc_call(c, op == OP_get_field ? (void *)h_get_field :
                                        (void *)h_get_field2);
        break;
    case OP_put_field:
        fc_set_param(c, U32(), bc_off);
        fc_call(c, (void *)h_put_field);
        break;
    case OP_get_array_el:
        if (c->tier == 3 && fc_elem_feedback(c, bc_off) >= 0) {
            fc_emit_array_el(c, bc_off);
            break;
        }
        fc_set_param(c, 0, bc_off);
        fc_call(c, (void *)h_get_array_el);
        break;
    case OP_get_array_el2:
        fc_set_param(c, 0, bc_off);
        fc_call(c, (void *)h_get_array_el2);
        break;
    case OP_put_array_el:
        fc_set_param(c, 0, bc_off);
        fc_call(c, (void *)h_put_array_el);
        break;
    case OP_get_length:
        fc_set_cur_pc(c, bc_off);
        fc_set_param(c, 0, bc_off);
        fc_call(c, (void *)h_get_length);
        break;

    case OP_call: case OP_tail_call:
        fc_set_cur_pc(c, bc_off);
        fc_set_param(c, U16(), op == OP_call_method ? 1 : 0);
        fc_call(c, op == OP_tail_call ? (void *)h_tail_call :
                                        (void *)h_call);
        if (op == OP_tail_call)
            fl_jmp(e, c->epilogue);
        break;
    case OP_call_method: case OP_tail_call_method:
        fc_set_cur_pc(c, bc_off);
        fc_set_param(c, U16(), 1);
        fc_call(c, op == OP_tail_call_method ? (void *)h_tail_call :
                                               (void *)h_call);
        if (op == OP_tail_call_method)
            fl_jmp(e, c->epilogue);
        break;
    case OP_call0: case OP_call1: case OP_call2: case OP_call3:
        fc_set_cur_pc(c, bc_off);
        fc_set_param(c, op - OP_call0, 0);
        fc_call(c, (void *)h_call);
        break;

    case OP_return:
        fl_sub_ri(e, FL_RSP, FL_SZ_VALUE);
        fl_mov_rm(e, FL_RTMP0, FL_RSP, FL_OFF_U);
        fl_mov_mr(e, FL_RSTATE, FL_OFF_RETVAL_U, FL_RTMP0);
        fl_mov_rm(e, FL_RTMP0, FL_RSP, FL_OFF_TAG);
        fl_mov_mr(e, FL_RSTATE, FL_OFF_RETVAL_T, FL_RTMP0);
        fl_mov32_ri(e, FL_RTMP0, 2);
        fl_mov32_mr(e, FL_RSTATE, FL_OFF_EXIT_KIND, FL_RTMP0);
        fl_jmp(e, c->epilogue);
        break;
    case OP_return_undef:
        fl_mov32_ri(e, FL_RTMP0, 0);
        fl_mov32_mr(e, FL_RSTATE, FL_OFF_RETVAL_U, FL_RTMP0);
        fl_mov32_ri(e, FL_RTMP0, FL_TAG_UNDEFINED);
        fl_mov_mr(e, FL_RSTATE, FL_OFF_RETVAL_T, FL_RTMP0);
        fl_mov32_ri(e, FL_RTMP0, 2);
        fl_mov32_mr(e, FL_RSTATE, FL_OFF_EXIT_KIND, FL_RTMP0);
        fl_jmp(e, c->epilogue);
        break;
    case OP_throw:
        fc_call(c, (void *)h_throw);
        fl_jmp(e, c->epilogue);
        break;

    case OP_if_false8: case OP_if_true8:
        fc_emit_if(c, bc_off, (uint32_t)(bc_off + 1 + I8()),
                   op == OP_if_true8);
        break;
    case OP_if_false: case OP_if_true:
        fc_emit_if(c, bc_off, (uint32_t)(bc_off + 1 + I32()),
                   op == OP_if_true);
        break;
    case OP_goto8:
        fl_jmp(e, fc_label_at(c, (uint32_t)(bc_off + 1 + I8())));
        break;
    case OP_goto16:
        fl_jmp(e, fc_label_at(c, (uint32_t)(bc_off + 1 + I16())));
        break;
    case OP_goto:
        fl_jmp(e, fc_label_at(c, (uint32_t)(bc_off + 1 + I32())));
        break;

    case OP_add: case OP_sub: case OP_mul: case OP_div: case OP_mod:
        fc_emit_binop(c, bc_off, op);
        break;
    case OP_lt: case OP_lte: case OP_gt: case OP_gte:
        fc_emit_relational(c, bc_off, op);
        break;
    case OP_eq: case OP_neq: case OP_strict_eq: case OP_strict_neq:
        fc_emit_eq(c, bc_off, op);
        break;
    case OP_and: case OP_xor: case OP_or: case OP_shl: case OP_sar:
    case OP_shr:
        fc_emit_logic(c, bc_off, op);
        break;

    case OP_not:
        fc_set_param(c, 0, bc_off);
        fc_call(c, (void *)h_not);
        break;
    case OP_lnot:
        fc_set_param(c, 0, bc_off);
        fc_call(c, (void *)h_lnot);
        break;

    case OP_typeof:
        fc_set_param(c, 0, bc_off);
        fc_call(c, (void *)h_typeof);
        break;
    case OP_typeof_is_undefined:
        fc_set_param(c, JS_ATOM_undefined, bc_off);
        fc_call(c, (void *)h_typeof_is);
        break;
    case OP_typeof_is_function:
        fc_set_param(c, JS_ATOM_function, bc_off);
        fc_call(c, (void *)h_typeof_is);
        break;

    case OP_neg: case OP_plus: case OP_inc: case OP_dec:
    case OP_post_inc: case OP_post_dec:
        fc_set_param(c, op, bc_off);
        fc_call(c, (void *)h_unary_arith);
        break;
    case OP_inc_loc: case OP_dec_loc:
        fc_set_param(c, U8(), op);
        fc_call(c, (void *)h_incdec_loc);
        break;
    case OP_add_loc:
        fc_set_param(c, U8(), bc_off);
        fc_call(c, (void *)h_add_loc);
        break;

    case OP_is_undefined_or_null:
        fc_call(c, (void *)h_is_undefined_or_null);
        break;
    case OP_is_undefined:
        fc_set_param(c, FL_TAG_UNDEFINED, 0);
        fc_call(c, (void *)h_is_tag);
        break;
    case OP_is_null:
        fc_set_param(c, FL_TAG_NULL, 0);
        fc_call(c, (void *)h_is_tag);
        break;

    case OP_drop:
        fc_call(c, (void *)h_drop);
        break;
    case OP_nop:
        break;

    /* pure stack shuffles: 16-byte slot moves (no refcount changes) */
    case OP_dup: /* a -> a a (fresh reference on the copy) */
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, 0);
        fc_dup_slot(c, FL_RSP, 0);
        fl_add_ri(e, FL_RSP, FL_SZ_VALUE);
        break;
    case OP_dup1: /* a b -> a a b (dup of the first) */
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, 0);
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, -FL_SZ_VALUE);
        fc_dup_slot(c, FL_RSP, -FL_SZ_VALUE);
        fl_add_ri(e, FL_RSP, FL_SZ_VALUE);
        break;
    case OP_dup2: /* a b -> a b a b (both copies are fresh refs) */
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, 0);
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, FL_SZ_VALUE);
        fc_dup_slot(c, FL_RSP, 0);
        fc_dup_slot(c, FL_RSP, FL_SZ_VALUE);
        fl_add_ri(e, FL_RSP, 2 * FL_SZ_VALUE);
        break;
    case OP_dup3:
        fc_copy_slot(c, FL_RSP, -3 * FL_SZ_VALUE, FL_RSP, 0);
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, 2 * FL_SZ_VALUE);
        fc_dup_slot(c, FL_RSP, 0);
        fc_dup_slot(c, FL_RSP, FL_SZ_VALUE);
        fc_dup_slot(c, FL_RSP, 2 * FL_SZ_VALUE);
        fl_add_ri(e, FL_RSP, 3 * FL_SZ_VALUE);
        break;
    case OP_nip: /* a b -> b */
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, -2 * FL_SZ_VALUE);
        fl_sub_ri(e, FL_RSP, FL_SZ_VALUE);
        break;
    case OP_nip1: /* a b c -> b c */
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, -3 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, -2 * FL_SZ_VALUE);
        fl_sub_ri(e, FL_RSP, FL_SZ_VALUE);
        break;
    case OP_swap:
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, -2 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, -FL_SZ_VALUE);
        break;
    case OP_swap2: /* a b c d -> c d a b */
        fc_copy_slot(c, FL_RSP, -3 * FL_SZ_VALUE, FL_RSP, -FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -4 * FL_SZ_VALUE, FL_RSP, -2 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, -3 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, -4 * FL_SZ_VALUE);
        break;
    case OP_rot3l: /* x a b -> a b x */
        fc_copy_slot(c, FL_RSP, -3 * FL_SZ_VALUE, FL_RSP, -FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, -2 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, -3 * FL_SZ_VALUE);
        break;
    case OP_rot3r: /* a b x -> x a b */
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, -3 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -3 * FL_SZ_VALUE, FL_RSP, -2 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, -FL_SZ_VALUE);
        break;
    case OP_rot4l:
        fc_copy_slot(c, FL_RSP, -4 * FL_SZ_VALUE, FL_RSP, -FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -3 * FL_SZ_VALUE, FL_RSP, -2 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, -3 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, -4 * FL_SZ_VALUE);
        break;
    case OP_rot5l:
        fc_copy_slot(c, FL_RSP, -5 * FL_SZ_VALUE, FL_RSP, -FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -4 * FL_SZ_VALUE, FL_RSP, -2 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -3 * FL_SZ_VALUE, FL_RSP, -3 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, -4 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, -5 * FL_SZ_VALUE);
        break;
    case OP_insert2: /* obj a -> a obj a */
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, 0);
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, -FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, -2 * FL_SZ_VALUE);
        fl_add_ri(e, FL_RSP, FL_SZ_VALUE);
        break;
    case OP_insert3: /* obj prop a -> a obj prop a */
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, 0);
        fc_copy_slot(c, FL_RSP, -3 * FL_SZ_VALUE, FL_RSP, -2 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, -3 * FL_SZ_VALUE);
        fl_add_ri(e, FL_RSP, FL_SZ_VALUE);
        break;
    case OP_insert4: /* this obj prop a -> a this obj prop a */
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, 0);
        fc_copy_slot(c, FL_RSP, -4 * FL_SZ_VALUE, FL_RSP, -3 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -3 * FL_SZ_VALUE, FL_RSP, -4 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, -2 * FL_SZ_VALUE);
        fl_add_ri(e, FL_RSP, FL_SZ_VALUE);
        break;
    case OP_perm3: /* obj a b -> a obj b */
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RSP, -2 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, -FL_SZ_VALUE);
        break;
    case OP_perm4: /* obj prop a b -> a obj prop b */
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, -4 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -4 * FL_SZ_VALUE, FL_RSP, -3 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -3 * FL_SZ_VALUE, FL_RSP, -2 * FL_SZ_VALUE);
        break;
    case OP_perm5: /* this obj prop a b -> a this obj prop b */
        fc_copy_slot(c, FL_RSP, -2 * FL_SZ_VALUE, FL_RSP, -5 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -5 * FL_SZ_VALUE, FL_RSP, -4 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -4 * FL_SZ_VALUE, FL_RSP, -3 * FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, -3 * FL_SZ_VALUE, FL_RSP, -2 * FL_SZ_VALUE);
        break;

    case OP_get_loc_check: case OP_get_loc_checkthis:
        U16();
        fc_set_param(c, u32, 0);
        fc_call(c, (void *)h_get_loc_check);
        break;
    case OP_put_loc_check: case OP_put_loc_check_init:
        U16();
        fc_set_param(c, u32, op == OP_put_loc_check ? 0 : 1);
        fc_call(c, (void *)h_put_loc_check);
        break;
    case OP_get_var_ref_check:
        U16();
        fc_set_param(c, u32, 0);
        fc_call(c, (void *)h_var_ref_check);
        break;
    case OP_put_var_ref_check:
        U16();
        fc_set_param(c, u32, 1);
        fc_call(c, (void *)h_var_ref_check);
        break;
    case OP_put_var_ref_check_init:
        U16();
        fc_set_param(c, u32, 2);
        fc_call(c, (void *)h_var_ref_check);
        break;
    case OP_close_loc:
        U16();
        fc_set_param(c, u32, 0);
        fc_call(c, (void *)h_close_loc);
        break;
    case OP_define_var:
    case OP_check_define_var:
        fc_set_param(c, U32(), (uint32_t)imm[0]);
        fc_call(c, op == OP_define_var ? (void *)h_define_var :
                                         (void *)h_check_define_var);
        break;
    case OP_define_func:
        fc_set_param(c, U32(), (uint32_t)imm[0]);
        fc_call(c, (void *)h_define_func);
        break;

    default:
        /* tier 2: should be unreachable (the whitelist covers everything);
           tier 3: deoptimize at this opcode — the interpreter resumes here
           with the exact frame and executes the op itself */
        if (c->tier == 3)
            fc_deopt(c, bc_off);
        else
            c->e.ok = 0;
        break;
    }
#undef U32
#undef U16
#undef I16
#undef I32
#undef U8
#undef I8
}

static int flame_compile_tier(JSContext *ctx, JSFunctionBytecode *b,
                              uint8_t tier);

int flame_baseline_compile(JSContext *ctx, JSFunctionBytecode *b)
{
    return flame_compile_tier(ctx, b, 2);
}

void flame_opt_compile(JSContext *ctx, JSFunctionBytecode *b)
{
    FlameProfile *prof = (FlameProfile *)b->flame_prof;
    /* Tier 3 is experimental: opt in via FLAME_TIER3=1 until the deopt
       resume handoff is fully validated. */
    {
        const char *t3 = getenv("FLAME_TIER3");
        if (!t3 || !*t3 || strcmp(t3, "0") == 0)
            return;
    }
    if (!b->flame_compiled || b->flame_opt)
        return;
    if (!flame_rt_is_enabled(ctx->rt))
        return;
    if (prof && (prof->no_opt || prof->no_compile))
        return;
    /* see osr_has_prop_store_impl */
    if (osr_has_prop_store(b))
        return;
    flame_log(FLAME_LOG_TIER, "tier3 compiling function");
    flame_compile_tier(ctx, b, 3);
}

static int flame_compile_tier(JSContext *ctx, JSFunctionBytecode *b,
                              uint8_t tier)
{
    {
        const char *nc = getenv("FLAME_NO_COMPILE");
        if (nc && *nc && strcmp(nc, "0") != 0)
            return -1;
    }
    FlameCmp c;
    FlameExecBlock *blk;
    FlameEmitter *e;
    const uint8_t *pc, *end;
    /* emission budget: ~128 bytes of native code per bytecode byte plus a
       fixed prologue/epilogue/trampoline reserve; if the budget is ever
       exceeded the emitter flags it and the function falls back to the
       interpreter, so this is a guess with a safe failure mode */
    size_t code_cap = (size_t)b->byte_code_len * 128 + 4096;
    char *lbl = NULL;

    /* zero c up front so every early "goto refuse" path is safe */
    memset(&c, 0, sizeof(c));

    if (b->func_kind != JS_FUNC_NORMAL || b->is_derived_class_constructor)
        goto refuse_quiet;
    if (b->stack_size > 4000 || b->byte_code_len <= 0)
        goto refuse_quiet;
    if (!baseline_whitelist_ok(b))
        goto refuse;

    blk = flame_rt_new_block(ctx->rt, code_cap);
    if (!blk)
        goto refuse;

    c.b = b;
    c.tier = tier;
    c.prof = (FlameProfile *)b->flame_prof;
    c.labels = js_mallocz(ctx, sizeof(int32_t) * (size_t)(b->byte_code_len + 1));
    c.nat = js_mallocz(ctx, sizeof(size_t) * (size_t)(b->byte_code_len + 1));
    if (!c.labels || !c.nat)
        goto refuse;
    {
        uint32_t i;
        for (i = 0; i <= (uint32_t)b->byte_code_len; i++)
            c.labels[i] = -1;
    }
    e = &c.e;
    fl_init(e, blk->base, blk->size);
    c.epilogue = fl_label(e);

    /* prologue: int entry(FlameJitState *st); rsp%16 == 8 on entry, three
       pushes keep the helper-call alignment invariant */
    fl_mov_rr(e, FL_RSTATE, FL_RARG);
    fl_push(e, FL_RSTATE);
    fl_push(e, FL_RSP);
    fl_push(e, FL_RLOC);
    fl_mov_rm(e, FL_RLOC, FL_RSTATE, FL_OFF_LOCAL_BUF);
    fl_mov_rm(e, FL_RSP, FL_RSTATE, FL_OFF_SP);

    pc = b->byte_code_buf;
    end = pc + b->byte_code_len;
    while (pc < end) {
        uint8_t op = *pc;
        uint32_t off = (uint32_t)(pc - b->byte_code_buf);
        c.cur_off = off;
        c.nat[off] = c.e.len;
        fc_bind_at(&c, off);
        emit_one(&c, pc, op, off);
        if (flame_debug_init() >= FLAME_LOG_TRACE)
            flame_log(FLAME_LOG_TRACE, "post-emit op=%d bc=%u ok=%d",
                      op, off, e->ok);
        if (!e->ok) {
            flame_log(FLAME_LOG_TIER,
                      "emitter failed at opcode %d (0x%02x) bc:%u",
                      op, op, off);
            goto fail;
        }
        pc += short_opcode_info(op).size;
    }
    if (pc != end)
        goto fail;
    if (!e->ok) { flame_log(FLAME_LOG_TIER, "body ok but e.ok=0?"); goto fail; }

    /* ---- out-of-line deopt trampolines ----
     * guard-failure landings: set exit_kind=3 + resume pc, publish sp,
     * exit to the wrapper (which resumes in the interpreter) */
    {
        int k;
        for (k = 0; k < c.deopt_count; k++) {
            fl_bind(e, c.deopt_pending[k].label);
            fl_mov32_ri(e, FL_RTMP0, FLAME_EXIT_DEOPT);
            fl_mov32_mr(e, FL_RSTATE, FL_OFF_EXIT_KIND, FL_RTMP0);
            fl_mov32_ri(e, FL_RTMP0, c.deopt_pending[k].bc_off);
            fl_mov32_mr(e, FL_RSTATE, FL_OFF_DEOPT_PC, FL_RTMP0);
            fl_mov_mr(e, FL_RSTATE, FL_OFF_SP, FL_RSP);
            fl_jmp(e, c.epilogue);
        }
    }

    /* implicit return undefined (falls off the end) */
    fc_bind_at(&c, (uint32_t)b->byte_code_len);
    fl_mov32_ri(e, FL_RTMP0, 0);
    fl_mov32_mr(e, FL_RSTATE, FL_OFF_RETVAL_U, FL_RTMP0);
    fl_mov32_ri(e, FL_RTMP0, FL_TAG_UNDEFINED);
    fl_mov_mr(e, FL_RSTATE, FL_OFF_RETVAL_T, FL_RTMP0);
    fl_mov32_ri(e, FL_RTMP0, 2);
    fl_mov32_mr(e, FL_RSTATE, FL_OFF_EXIT_KIND, FL_RTMP0);
    fl_jmp(e, c.epilogue);
    if (!e->ok) { flame_log(FLAME_LOG_TIER, "implicit-return emission failed"); goto fail; }

    /* ---- epilogue ----
     * write sp back, restore regs, return 0. */
    fl_bind(e, c.epilogue);
    fl_mov_mr(e, FL_RSTATE, FL_OFF_SP, FL_RSP);
    fl_pop(e, FL_RLOC);
    fl_pop(e, FL_RSP);
    fl_pop(e, FL_RSTATE);
    fl_ret(e);

    /* ---- OSR entry stubs (one per loop header) ----
     * C-callable: int stub(FlameJitState *st) — loads the live frame
     * pointers from the state, pushes the callee-saved regs the epilogue
     * expects, and jumps into the loop header code. */
    {
        uint32_t osr_bc[FL_MAX_OSR_HEADERS];
        void *osr_entry[FL_MAX_OSR_HEADERS];
        int n = c.osr_count, k;
        for (k = 0; k < n; k++) {
            int lbl = fl_label(e);
            size_t stub_at = e->len;
            fl_mov_rr(e, FL_RSTATE, FL_RARG);
            fl_mov_rm(e, FL_RLOC, FL_RSTATE, FL_OFF_LOCAL_BUF);
            fl_mov_rm(e, FL_RSP, FL_RSTATE, FL_OFF_SP);
            /* match the main prologue: the epilogue pops these */
            fl_push(e, FL_RSTATE);
            fl_push(e, FL_RSP);
            fl_push(e, FL_RLOC);
            fl_jmp(e, lbl);
            fl_bind_offset(e, lbl, c.nat[c.osr_bc[k]]);
            osr_bc[k] = c.osr_bc[k];
            osr_entry[k] = blk->base + stub_at;
        }
        /* registered after fl_finish (patches must be resolved first) */
        fl_finish(e);
        if (!e->ok) {
            int i, unb = 0;
            for (i = 0; i < e->label_count; i++)
                if (!e->labels[i].bound) unb++;
            flame_log(FLAME_LOG_TIER,
                      "fl_finish failed: labels=%d unbound=%d patches=%d",
                      e->label_count, unb, e->patch_count);
            goto fail;
        }
        flame_rt_register_func(ctx->rt, b, blk->base, e->len, 2,
                               osr_bc, osr_entry, n, &b->flame_compiled);
    }

    js_free(ctx, c.labels);
    js_free(ctx, c.nat);
    c.labels = NULL;
    c.nat = NULL;
    {
        const char *dump = getenv("FLAME_DUMP");
        if (dump) {
            char path[512];
            JSValue nm = JS_AtomToString(ctx, b->func_name);
            const char *nm_s = JS_ToCString(ctx, nm);
            JS_FreeValue(ctx, nm);
            snprintf(path, sizeof(path), "%s_%s.bin", dump,
                     nm_s ? nm_s : "anon");
            if (nm_s)
                JS_FreeCString(ctx, nm_s);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(blk->base, 1, e->len, f);
                fclose(f);
                flame_log(FLAME_LOG_TIER, "dumped %zu native bytes to %s",
                          e->len, path);
            }
        }
    }
    flame_exec_flush_icache(blk->base, e->len);

    if (JS_AtomIsString(ctx, b->func_name)) {
        const char *s = JS_AtomToCString(ctx, b->func_name);
        if (s) { lbl = js_strdup(ctx, s); JS_FreeCString(ctx, s); }
    }
    flame_log(FLAME_LOG_TIER,
              "baseline compiled: %s (%d bc bytes -> %zu native bytes, "
              "%d osr entries)",
              lbl ? lbl : "<anon>", b->byte_code_len, e->len, c.osr_count);
    js_free(ctx, lbl);
    return 0;

fail:
    flame_log(FLAME_LOG_TIER, "emit failed: e.ok=%d pc_off=%u",
              c.e.ok, (uint32_t)(pc - b->byte_code_buf));
    js_free(ctx, c.labels);
    js_free(ctx, c.nat);
    c.labels = NULL;
    c.nat = NULL;
    /* fall through */
refuse:
    flame_stat_compile_fails++;
    {
        FlameProfile *prof = (FlameProfile *)b->flame_prof;
        if (prof)
            prof->no_compile = 1;
    }
    if (JS_AtomIsString(ctx, b->func_name)) {
        const char *s = JS_AtomToCString(ctx, b->func_name);
        if (s) { lbl = js_strdup(ctx, s); JS_FreeCString(ctx, s); }
    }
    flame_log(FLAME_LOG_TIER, "baseline compile refused: %s",
              lbl ? lbl : "<anon>");
    js_free(ctx, lbl);
    return -1;

refuse_quiet:
    flame_stat_compile_fails++;
    return -1;
}



/* True if this call should run in the baseline JIT. */
int flame_should_enter_jit(JSContext *ctx, JSObject *p,
                           JSFunctionBytecode *b, int flags)
{
    if (!flame_rt_is_enabled(ctx->rt))
        return 0;
    if (flags != 0) /* CONSTRUCTOR / GENERATOR / COPY_ARGV: interpreter */
        return 0;
    if (b->func_kind != JS_FUNC_NORMAL)
        return 0;
    if (osr_has_prop_store(b))
        return 0; /* containment: see flame_baseline_maybe_compile */
    return (b->flame_opt ? b->flame_opt : b->flame_compiled) != NULL;
}

static JSValue flame_deopt_resume(JSContext *caller_ctx,
                                  JSValueConst func_obj,
                                  FlameJitState *st, JSFunctionBytecode *b);

/* free every owned JSValue in a JIT frame plus the frame storage.
   Used on failure paths only: the caller still owns `st` itself. */
static void flame_jit_frame_dispose(JSContext *ctx, FlameJitState *st)
{
    JSValue *pv;
    if (!st->local_buf)
        return;
    for (pv = st->local_buf; pv < st->sp; pv++)
        JS_FreeValue(ctx, *pv);
    JS_FreeValue(ctx, st->this_val);
    js_free(ctx, st->local_buf);
    st->local_buf = NULL;
    st->this_val = JS_UNDEFINED;
}

JSValue flame_baseline_call(JSContext *caller_ctx, JSValueConst func_obj,
                            JSValueConst this_obj, int argc, JSValue *argv)
{
    JSRuntime *rt = caller_ctx->rt;
    JSObject *p = JS_VALUE_GET_OBJ(func_obj);
    JSFunctionBytecode *b = p->u.func.function_bytecode;
    FlameCompiledFunc *cf = (FlameCompiledFunc *)(b->flame_opt ?
                                                  b->flame_opt :
                                                  b->flame_compiled);
    typedef int (*FlameEntry)(FlameJitState *);
    FlameEntry fn = (FlameEntry)cf->code;
    FlameJitState *st;
    JSContext *ctx = b->realm;
    JSValue *local_buf;
    JSValue *pv;
    JSValue ret;
    int nlocals, i;

    if (!fn)
        return JS_ThrowInternalError(caller_ctx, "flame: null entry");

    /* fixed frame layout: [arg_count args | var_count vars | stack].
     * Extra arguments (argc > arg_count) stay in the caller's argv, like
     * the interpreter's COPY_ARGV path; the JIT frame is compile-time
     * sized so local-variable addressing is static. */
    nlocals = b->arg_count;
    if (js_check_stack_overflow(rt,
            (size_t)nlocals * sizeof(JSValue) + sizeof(FlameJitState) + 64))
        return JS_ThrowStackOverflow(caller_ctx);

    /* heap-allocated (not alloca'd): the JIT'd code and every deopt/OSR
     * path write through this pointer for the whole activation, and a
     * frame-allocated copy would let any out-of-bounds write land in this
     * function's own locals (this was a real corruption source) */
    st = js_mallocz(ctx, sizeof(FlameJitState));
    if (!st)
        return JS_ThrowOutOfMemory(caller_ctx);
    local_buf = js_malloc(ctx, sizeof(JSValue) *
                          (size_t)(nlocals + b->var_count + b->stack_size + 2));
    if (!local_buf) {
        js_free(ctx, st);
        return JS_ThrowOutOfMemory(caller_ctx);
    }
    /* guard slots past the frame: the JIT must never touch these */
    local_buf[nlocals + b->var_count + b->stack_size].u.int32 = 0x41414141;
    local_buf[nlocals + b->var_count + b->stack_size].tag = (int64_t)0x42424242;
    local_buf[nlocals + b->var_count + b->stack_size + 1].u.int32 = 0x43434343;
    local_buf[nlocals + b->var_count + b->stack_size + 1].tag = (int64_t)0x44444444;

    /* copy args (callee-owned, dup'd like JS_CALL_FLAG_COPY_ARGV) */
    {
        int ncopy = argc < b->arg_count ? argc : b->arg_count;
        for (i = 0; i < ncopy; i++)
            local_buf[i] = JS_DupValue(ctx, argv[i]);
        for (; i < nlocals + b->var_count; i++)
            local_buf[i] = JS_UNDEFINED;
    }

    st->ctx = ctx;
    st->sfp = &st->sf;
    st->b = b;
    st->bc_buf = b->byte_code_buf;
    st->var_refs = p->u.func.var_refs;
    st->cpool = b->cpool;
    st->local_buf = local_buf;
    st->stack_base = local_buf + nlocals + b->var_count;
    st->sp = st->stack_base;
    st->this_val = JS_DupValue(ctx, this_obj);
    st->sf.js_mode = b->js_mode;
    st->sf.arg_count = nlocals;
    st->sf.cur_func = (JSValue)func_obj; /* borrowed, like the interpreter */
    st->sf.cur_pc = b->byte_code_buf;
    st->sf.arg_buf = local_buf;
    st->sf.var_buf = local_buf + nlocals;
    init_list_head(&st->sf.var_ref_list);
    st->sf.prev_frame = rt->current_stack_frame;
    rt->current_stack_frame = &st->sf;

    flame_stat_jit_calls++;
    if (flame_debug_init() >= FLAME_LOG_TRACE)
        fprintf(stderr, "[flame] JIT enter fn bc=%d nlocals=%d\n",
                b->byte_code_len, nlocals);
    /* tiering continues while the function runs JIT: the interpreter's
     * entry hook is bypassed once compiled, so count entries here */
    {
        FlameProfile *prof = (FlameProfile *)b->flame_prof;
        if (prof && !prof->no_opt) {
            prof->entries++;
            if (prof->entries >= FLAME_OPT_FUNC_THRESHOLD &&
                (prof->entries - FLAME_OPT_FUNC_THRESHOLD) % 10000 == 0)
                flame_opt_maybe_compile(ctx, b);
        }
    }
    fn(st);

    if (st->exit_kind == FLAME_EXIT_DEOPT) {
        /* guard failure: the frame transfers to the interpreter; the resume
         * path owns local_buf, this_val and the frame's var_ref_list */
        FlameProfile *prof = (FlameProfile *)b->flame_prof;
        rt->current_stack_frame = st->sf.prev_frame;
        if (prof) {
            prof->opt_deopts++;
            if (prof->opt_deopts >= FLAME_OPT_DEOPT_LIMIT) {
                prof->no_opt = 1;
                b->flame_opt = NULL; /* permanent revert to baseline */
                flame_log(FLAME_LOG_TIER,
                          "deopt limit reached: reverting to baseline");
            }
        }
        ret = flame_deopt_resume(caller_ctx, func_obj, st, b);
        js_free(ctx, st); /* values were moved (or freed by resume); st dead */
        return ret;
    }

    rt->current_stack_frame = st->sf.prev_frame;
    if (!list_empty(&st->sf.var_ref_list))
        close_var_refs(rt, &st->sf);
    if (st->sp > st->stack_base + b->stack_size ||
        st->sp < st->stack_base)
        st->sp = st->stack_base; /* JIT bug guard: never free out of range */
    {
        JSValue *g = &local_buf[nlocals + b->var_count + b->stack_size];
        if (g[0].u.int32 != (int32_t)0x41414141 ||
            g[1].tag != (int64_t)0x44444444) {
            if (flame_debug_init() >= FLAME_LOG_TRACE)
                fprintf(stderr, "[flame] CANARY HIT: fn bc=%d sp_off=%d\n",
                        b->byte_code_len, (int)(st->sp - st->stack_base));
        }
    }
    for (pv = local_buf; pv < st->sp; pv++) {
        if (flame_debug_init() >= FLAME_LOG_TRACE)
            fprintf(stderr, "[flame] free slot %d tag=%d\n",
                    (int)(pv - local_buf), (int)JS_VALUE_GET_TAG(*pv));
        JS_FreeValue(ctx, *pv);
    }
    JS_FreeValue(ctx, st->this_val);
    js_free(ctx, local_buf);

    {
        uint32_t exc = st->exception, kind = st->exit_kind;
        JSValue rv = st->retval;
        js_free(ctx, st);
        if (exc || kind == 1)
            return JS_EXCEPTION;
        return rv;
    }
}


/* Materialize the deopted JIT frame as a suspended interpreter frame and
 * resume at the failed opcode. All frame values transfer by ownership:
 * the wrapper skips its normal free path for a deopt exit.
 *
 * Failure paths (OOM / stack overflow) take ownership of the JIT frame's
 * values + local_buf + this_val and free them here; the caller only frees
 * the FlameJitState struct itself. */
static JSValue flame_deopt_resume(JSContext *caller_ctx,
                                  JSValueConst func_obj,
                                  FlameJitState *st, JSFunctionBytecode *b)
{
    JSRuntime *rt = caller_ctx->rt;
    JSContext *ctx = b->realm;
    JSAsyncFunctionState *s;
    JSStackFrame *sf;
    JSValue ret, *pv, *q;
    int i, n, arg_buf_len, local_count;

    s = js_mallocz(ctx, sizeof(*s));
    if (!s) {
        flame_jit_frame_dispose(ctx, st);
        return JS_ThrowOutOfMemory(caller_ctx);
    }
    s->header.ref_count = 1;
    add_gc_object(rt, &s->header, JS_GC_OBJ_TYPE_ASYNC_FUNCTION);
    sf = &s->frame;
    init_list_head(&sf->var_ref_list);
    sf->js_mode = b->js_mode;
    sf->cur_pc = b->byte_code_buf + st->deopt_pc;

    arg_buf_len = b->arg_count;
    local_count = arg_buf_len + b->var_count + b->stack_size;
    sf->arg_buf = js_malloc(ctx, sizeof(JSValue) *
                            (size_t)max_int(local_count, 1));
    if (!sf->arg_buf) {
        remove_gc_object(&s->header);
        js_free_rt(rt, s);
        flame_jit_frame_dispose(ctx, st);
        return JS_ThrowOutOfMemory(caller_ctx);
    }
    sf->cur_func = JS_DupValue(ctx, func_obj);
    s->this_val = st->this_val; /* ownership transferred from the JIT frame */
    st->this_val = JS_UNDEFINED;
    s->argc = arg_buf_len;
    sf->arg_count = arg_buf_len;
    sf->var_buf = sf->arg_buf + arg_buf_len;
    /* move args/vars/stack values from the JIT frame; cur_sp uses the
     * interpreter convention: ONE PAST the last pushed stack value */
    n = (int)(st->sp - st->stack_base);
    sf->cur_sp = sf->var_buf + b->var_count + n;
    for (i = 0; i < arg_buf_len + b->var_count; i++)
        sf->arg_buf[i] = st->local_buf[i];
    for (i = 0, pv = st->stack_base, q = sf->cur_sp - n; i < n; i++, pv++, q++)
        *q = *pv;
    s->throw_flag = st->exception;
    s->resolving_funcs[0] = JS_UNDEFINED;
    s->resolving_funcs[1] = JS_UNDEFINED;

    /* relocate any var_refs linked to the JIT frame's list head */
    if (!list_empty(&st->sf.var_ref_list)) {
        struct list_head *first = st->sf.var_ref_list.next;
        struct list_head *last = st->sf.var_ref_list.prev;
        first->prev = &sf->var_ref_list;
        last->next = &sf->var_ref_list;
        sf->var_ref_list.next = first;
        sf->var_ref_list.prev = last;
    }

    js_free(ctx, st->local_buf); /* raw: values moved to the resume frame */
    st->local_buf = NULL;

    if (js_check_stack_overflow(rt, 0)) {
        /* sf->arg_buf holds owned values; free them before the buffer */
        for (i = 0; i < local_count; i++)
            JS_FreeValueRT(rt, sf->arg_buf[i]);
        JS_FreeValueRT(rt, sf->cur_func);
        JS_FreeValueRT(rt, s->this_val);
        js_free_rt(rt, sf->arg_buf);
        remove_gc_object(&s->header);
        js_free_rt(rt, s);
        return JS_ThrowStackOverflow(caller_ctx);
    }

    /* resume exactly like a suspended generator: the interpreter picks up
     * at sf->cur_pc with sf->cur_sp */
    ret = JS_CallInternal(ctx, JS_MKPTR(JS_TAG_INT, s), s->this_val,
                          JS_UNDEFINED, s->argc, sf->arg_buf,
                          JS_CALL_FLAG_GENERATOR);

    /* normal functions always complete: the resumed interpreter freed the
     * frame VALUES (done label); release the frame arrays + borrowed refs */
    js_free_rt(rt, sf->arg_buf);
    JS_FreeValueRT(rt, sf->cur_func);
    JS_FreeValueRT(rt, s->this_val);
    remove_gc_object(&s->header);
    js_free_rt(rt, s);
    return ret;
}


/* Transfer the CURRENT interpreter activation into compiled code at a hot
 * loop header. The JIT frame aliases the interpreter's live frame (st->sfp
 * points at it), so args/vars stay in place; the loop continues natively
 * and the function runs to completion in compiled code. */
/* OSR currently skips functions with property-store ops: the store's
 * setter chain can re-enter the runtime while the aliased live frame is
 * active, which surfaced teardown refcount issues on DOM-heavy pages.
 * Entry-time JIT (flame_baseline_call) is unaffected. */
static int osr_has_prop_store_impl(JSFunctionBytecode *b)
{
    const uint8_t *pc = b->byte_code_buf;
    const uint8_t *end = pc + b->byte_code_len;
    while (pc < end) {
        uint8_t op = *pc++;
        switch (op) {
        case OP_put_field: case OP_put_array_el: case OP_put_var:
        case OP_put_var_init: case OP_put_var_strict:
            return 1;
        }
        pc += short_opcode_info(op).size - 1;
    }
    return 0;
}

static int osr_has_prop_store(JSFunctionBytecode *b)
{
    return osr_has_prop_store_impl(b);
}

int flame_osr_run(JSContext *ctx, JSFunctionBytecode *b, FlameOsrEntry *oe,
                  JSValue *sp, JSValue *out)
{
    if (osr_has_prop_store(b))
        return 0;
    JSRuntime *rt = ctx->rt;
    JSStackFrame *sf = rt->current_stack_frame;
    JSObject *p;
    typedef int (*FlameEntry)(FlameJitState *);
    FlameJitState *st;
    JSValue *jit_base;

    if (!sf || sf->cur_pc == NULL)
        return 0;
    /* the JIT frame layout requires vars at (arg_count + idx) slots from
     * the JIT base: use the frame's arg_buf for copied-arg frames, or
     * var_buf directly for parameterless functions (top-level scripts) */
    if (b->arg_count == 0)
        jit_base = sf->var_buf;
    else if (sf->var_buf == sf->arg_buf + b->arg_count)
        jit_base = sf->arg_buf;
    else
        return 0; /* in-place argv with parameters: skip OSR */
    p = JS_VALUE_GET_OBJ(sf->cur_func);
    if (p->u.func.function_bytecode != b)
        return 0;

    st = (FlameJitState *)alloca(sizeof(FlameJitState));
    memset(st, 0, sizeof(*st));
    st->sfp = sf; /* alias the live interpreter frame */
    st->ctx = ctx;
    st->b = b;
    st->bc_buf = b->byte_code_buf;
    st->var_refs = p->u.func.var_refs;
    st->cpool = b->cpool;
    st->local_buf = jit_base;
    st->stack_base = sf->var_buf + b->var_count;
    st->sp = sp;
    st->argc = sf->arg_count;
    /* this_val is only consumed by push_this, which only appears at
     * function entry — unreachable after OSR */
    st->this_val = JS_UNDEFINED;

    if (sp < st->stack_base || sp > st->stack_base + b->stack_size) {
        flame_log(FLAME_LOG_TIER, "osr bail: sp out of range");
        return 0;
    }

    flame_stat_osr++;
    ((FlameEntry)oe->entry)(st);

    /* The JIT owns the frame now. JIT helpers freed consumed interpreter
     * stack slots inline; any values the JIT pushed remain above the
     * original sp. Hand the interpreter a consistent sf->cur_sp (the
     * JIT's current sp) so its exception/normal unwind frees exactly
     * [base, st->sp) -- nothing the JIT already freed is touched again. */
    if (st->sp >= st->stack_base && st->sp <= st->stack_base + b->stack_size)
        sf->cur_sp = st->sp;
    if (st->exception) {
        *out = JS_EXCEPTION;
        return 1;
    }
    *out = st->retval;
    return 1;
}

/* Tiering trigger: called from flame_hook_function_entry when the
 * function is hot enough to deserve native code. */
void flame_baseline_maybe_compile(JSContext *ctx, JSFunctionBytecode *b)
{
    FlameProfile *prof = (FlameProfile *)b->flame_prof;
    if (!flame_rt_is_enabled(ctx->rt))
        return;
    if (b->flame_compiled)
        return;
    if (flame_debug_init() >= FLAME_LOG_TRACE) {
        const char *nm = "<noatom>";
        if (JS_AtomIsString(ctx, b->func_name))
            nm = JS_AtomToCString(ctx, b->func_name);
        fprintf(stderr, "[flame] maybe_compile fn=%s entry=%u compiled=%p\n",
                nm, b->flame_exec_count, b->flame_compiled);
        if (nm && JS_AtomIsString(ctx, b->func_name))
            JS_FreeCString(ctx, nm);
    }
    if (prof && prof->no_compile)
        return;
    /* conservative containment: property-store functions stay interpreted
     * until the exotic-setter teardown interaction is root-caused */
    if (osr_has_prop_store(b)) {
        if (prof)
            prof->no_compile = 1;
        return;
    }
    flame_baseline_compile(ctx, b);
}

/* Tier-3 trigger: optimizing compilation of an already-baseline-compiled
 * function. Failures are silent; the baseline code remains active. */
void flame_opt_maybe_compile(JSContext *ctx, JSFunctionBytecode *b)
{
    flame_opt_compile(ctx, b);
}