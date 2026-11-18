/*
 * Flame — tier 2: baseline JIT.
 *
 * Translates QuickJS bytecode to native code one opcode at a time
 * (Hermes-style). Design rules:
 *   - The JIT frame mirrors the interpreter frame exactly: a malloc'd
 *     buffer [args | vars | stack] of JSValue slots; the current value
 *     stack pointer lives in a callee-saved register (FL_RSP).
 *   - Every operation with full JS semantics is a call into a helper that
 *     reuses the interpreter's own slow-path functions (js_add_slow,
 *     js_binary_arith_slow, JS_GetProperty, JS_CallInternal, ...), so
 *     baseline code is semantically complete by construction — this tier
 *     needs no deopt. Hot ops (int arithmetic, comparisons, locals
 *     traffic, branches) get inline fast paths that fall back to the
 *     helper on the first mismatching tag.
 *   - Functions containing control flow the tier cannot represent
 *     (try/catch, generators, with-scopes, for-in protocol ops, ...) are
 *     rejected at compile time and run in the interpreter forever.
 *   - Helpers are `void (*)(FlameJitState *)` — one pointer argument, so
 *     call sites are identical under both x64 ABIs. Per-site constants
 *     travel through the state's param/param2 fields.
 */
#include "flame_internal.h"
#include "flame_emitter.h"

/* ------------------------------------------------------------------ */
/* JIT state                                                           */
/* ------------------------------------------------------------------ */

typedef struct FlameJitState {
    JSStackFrame sf;
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
    uint8_t exception;
    uint8_t exit_kind;       /* 0 running, 1 exception, 2 return */
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
#define FL_OFF_EXCEPTION  ((int)offsetof(FlameJitState, exception))
#define FL_OFF_EXIT_KIND  ((int)offsetof(FlameJitState, exit_kind))
#define FL_OFF_SF_CUR_PC  ((int)offsetof(FlameJitState, sf) + \
                           (int)offsetof(JSStackFrame, cur_pc))

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

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void h_set_pc(FlameJitState *st)
{
    st->sf.cur_pc = st->bc_buf + st->param2;
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
        int res = JS_ToBoolFree(st->ctx, op1);
        if (JS_IsException(st->ctx->rt->current_exception) && res == 0 &&
            JS_VALUE_GET_TAG(st->ctx->rt->current_exception) ==
            JS_TAG_EXCEPTION) {
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
    if (1 || !flame_ic_get_field(st->ctx, st->b, st->param2, obj, atom, &val)) {
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
    if (1 || !flame_ic_get_array_el(st->ctx, st->b, st->param2, obj, prop, &val)) {
        val = JS_GetPropertyValue(st->ctx, obj, prop);
        if (JS_IsException(val)) {
            JS_FreeValue(st->ctx, obj);
            JS_FreeValue(st->ctx, prop);
            st->exception = 1;
            return;
        }
        flame_ic_record_array_el(st->ctx, st->b, st->param2, obj, prop, val);
    }
    JS_FreeValue(st->ctx, obj);
    /* NOTE: JS_GetPropertyValue consumes `prop` (frees it internally) */
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
    JS_FreeValue(st->ctx, obj);
    JS_FreeValue(st->ctx, st->sp[-2]);
    JS_FreeValue(st->ctx, st->sp[-1]);
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
    *st->sp++ = js_closure(st->ctx, bfunc, st->var_refs, &st->sf);
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
    JSValue *pv = &st->sf.var_buf[idx];
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
    JSValue *pv = &st->sf.var_buf[idx];
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
        if (JS_IsException(st->ctx->rt->current_exception) && res == 0 &&
            JS_VALUE_GET_TAG(st->ctx->rt->current_exception) ==
            JS_TAG_EXCEPTION) {
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

/* ---- TDZ checks and lexical close (mirror interpreter ops) ---- */

static void h_get_loc_check(FlameJitState *st)
{
    int idx = (int)st->param;
    int is_arg = (int)st->param2; /* 0 = var, 1 = arg */
    JSValue *pv = is_arg ? &st->sf.arg_buf[idx] : &st->sf.var_buf[idx];
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
    int mode = (int)st->param2; /* 0 = check, 1 = check_init */
    JSValue *pv = &st->sf.var_buf[idx];
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
    int mode = (int)st->param2; /* 0 get, 1 put, 2 put_init */
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
            JS_ThrowReferenceError(st->ctx,
                "'this' can be initialized only once");
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
    close_lexical_var(st->ctx, &st->sf, idx);
}

/* put_loc / set_loc family: set_value semantics (free old value, take
 * ownership of the stack value). param = idx, param2 = 0 var / 1 arg. */
static void h_put_loc(FlameJitState *st)
{
    int idx = (int)st->param;
    JSValue *pv = st->param2 ? &st->sf.arg_buf[idx] : &st->sf.var_buf[idx];
    set_value(st->ctx, pv, st->sp[-1]);
    st->sp--;
}

static void h_set_loc(FlameJitState *st)
{
    int idx = (int)st->param;
    JSValue *pv = st->param2 ? &st->sf.arg_buf[idx] : &st->sf.var_buf[idx];
    set_value(st->ctx, pv, st->sp[-1]);
}

static void h_define_var(FlameJitState *st)
{
    /* param = atom, param2 = flags */
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

static void h_drop(FlameJitState *st)
{
    JS_FreeValue(st->ctx, *--st->sp);
}

/* ------------------------------------------------------------------ */
/* Compiler                                                            */
/* ------------------------------------------------------------------ */

typedef struct FlameCmp {
    FlameEmitter e;
    JSFunctionBytecode *b;
    int32_t *labels;      /* bc offset -> label, -1 */
    size_t *nat;          /* bc offset -> native code offset (emit map) */
    uint32_t cur_off;     /* bytecode offset currently being emitted */
    int epilogue;
    uint32_t pending_param;
    uint32_t pending_param2;
    int has_pending;
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
     * and reload it after (both paths) */
    fl_mov_mr(e, FL_RSTATE, FL_OFF_SP, FL_RSP);
    fl_call(e, fn, FL_RSTATE);
    fl_mov_rm(e, FL_RSP, FL_RSTATE, FL_OFF_SP);
    fl_mov32_rm(e, FL_RTMP2, FL_RSTATE, FL_OFF_EXCEPTION);
    fl_cmp_ri(e, FL_RTMP2, 0);
    fl_jcc(e, FL_NE, c->epilogue);
}

static int fc_label_at(FlameCmp *c, uint32_t bc_off)
{
    if (bc_off > (uint32_t)c->b->byte_code_len)
        return -1;
    if (c->labels[bc_off] < 0) {
        c->labels[bc_off] = fl_label(&c->e);
        if (bc_off <= c->cur_off) {
            /* backward target: its native code was already emitted */
            fl_bind_offset(&c->e, c->labels[bc_off], c->nat[bc_off]);
        }
    }
    return c->labels[bc_off];
}

static void fc_bind_at(FlameCmp *c, uint32_t bc_off)
{
    if (bc_off <= (uint32_t)c->b->byte_code_len &&
        c->labels[bc_off] >= 0 &&
        !c->e.labels[c->labels[bc_off]].bound) {
        flame_log(FLAME_LOG_TRACE, "bind label %d at bc:%u",
                  c->labels[bc_off], bc_off);
        fl_bind(&c->e, c->labels[bc_off]);
    }
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

/* bump the refcount of the JSValue at [base+disp] when pointer-like
 * (mirrors JS_DupValue: only tags < 0 hold references) */
static void fc_dup_slot(FlameCmp *c, FlReg base, int32_t disp)
{
    FlameEmitter *e = &c->e;
    int done = fl_label(e);
    fl_mov_rm(e, FL_RTMP0, base, disp + FL_OFF_TAG);
    fl_cmp_ri(e, FL_RTMP0, 0);        /* signed: tag < 0 => refcounted */
    fl_jcc(e, FL_GE, done);
    fl_mov_rm(e, FL_RTMP1, base, disp + FL_OFF_U);
    /* inc dword [TMP1] (JSRefCountHeader.ref_count at offset 0):
       FF /0 with mod=00, reg=000, rm=TMP1 */
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
    fl_mov_mr(&c->e, FL_RSTATE, FL_OFF_SF_CUR_PC, FL_RTMP1);
}

/* inline int-int arithmetic fast path with helper fallback */
static void fc_emit_binop(FlameCmp *c, uint32_t bc_off, int op)
{
    fc_set_param(c, (uint32_t)op, bc_off);
    fc_call(c, (void *)h_binop);
}

/* inline int-int comparison; fallback to js_relational_slow */
static void fc_emit_relational(FlameCmp *c, uint32_t bc_off, int op)
{
    fc_set_param(c, (uint32_t)op, bc_off);
    fc_call(c, (void *)h_relational);
}

/* inline int-int equality; fallback to slow helpers */
static void fc_emit_eq(FlameCmp *c, uint32_t bc_off, int op)
{
    int strict = (op == OP_strict_eq || op == OP_strict_neq);
    fc_set_param(c, (uint32_t)op, bc_off);
    fc_call(c, strict ? (void *)h_strict_eq : (void *)h_eq);
}

/* inline int-int bitwise logic / shifts; fallback to js_binary_logic_slow */
static void fc_emit_logic(FlameCmp *c, uint32_t bc_off, int op)
{
    fc_set_param(c, (uint32_t)op, bc_off);
    fc_call(c, (void *)h_logic);
}

/* if_true / if_false with native branch decision */
static void fc_emit_if(FlameCmp *c, uint32_t bc_off, uint32_t target,
                       int branch_if_true)
{
    FlameEmitter *e = &c->e;
    int tgt = fc_label_at(c, target);
    int fast_tag = fl_label(e);
    int decided = fl_label(e);
    flame_log(FLAME_LOG_TRACE, "emit_if bc=%u tgt=%u (label %d)",
              bc_off, target, tgt);
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

/* ------------------------------------------------------------------ */
/* Whitelist                                                           */
/* ------------------------------------------------------------------ */

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
        case OP_define_var: case OP_check_define_var: case OP_define_func:
        case OP_get_loc0: case OP_get_loc1: case OP_get_loc2:
        case OP_get_loc3: case OP_put_loc0: case OP_put_loc1:
        case OP_put_loc2: case OP_put_loc3: case OP_set_loc0:
        case OP_set_loc1: case OP_set_loc2: case OP_set_loc3:
        case OP_get_arg0: case OP_get_arg1: case OP_get_arg2:
        case OP_get_arg3: case OP_put_arg0: case OP_put_arg1:
        case OP_put_arg2: case OP_put_arg3: case OP_set_arg0:
        case OP_set_arg1: case OP_set_arg2: case OP_set_arg3:
        case OP_set_loc_uninitialized:
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

/* ------------------------------------------------------------------ */
/* Per-opcode emission                                                 */
/* ------------------------------------------------------------------ */

static void emit_mul_div(FlameCmp *c, uint32_t bc_off, int op);

static void emit_one(FlameCmp *c, const uint8_t *pc, uint8_t op,
                     uint32_t bc_off)
{
    FlameEmitter *e = &c->e;
    JSFunctionBytecode *b = c->b;
    int ok_at_entry = e->ok;
    if (flame_debug_init() >= FLAME_LOG_TRACE)
        flame_log(FLAME_LOG_TRACE, "emit op=%d bc=%u size=%u ok=%d",
                  op, bc_off, short_opcode_info(op).size, ok_at_entry);
    const uint8_t *imm = pc + 1;
    uint32_t u32;
    int32_t s32;

#define U32() (u32 = get_u32(imm), imm += 4, u32)
#define U16() (u32 = get_u16(imm), imm += 2, u32)
#define I16() (s32 = (int16_t)get_u16(imm), imm += 2, s32)
#define I32() (s32 = get_u32(imm), imm += 4, s32)
#define U8()  (u32 = *imm++, u32)
#define I8()  (s32 = (int8_t)*imm++, s32)

    flame_log(FLAME_LOG_TRACE, "SWITCH enter op=%d", op);
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
        fl_sub_ri(e, FL_RSP, FL_SZ_VALUE);
        fc_copy_slot(c, FL_RSP, 0, FL_RLOC,
                     (op - OP_put_arg0) * FL_SZ_VALUE);
        break;
    case OP_set_arg0: case OP_set_arg1: case OP_set_arg2: case OP_set_arg3:
        fc_copy_slot(c, FL_RSP, -FL_SZ_VALUE, FL_RLOC,
                     (op - OP_set_arg0) * FL_SZ_VALUE);
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
        flame_log(FLAME_LOG_TRACE, "GL entered ok=%d", e->ok);
        fc_set_param(c, 0, bc_off);
        flame_log(FLAME_LOG_TRACE, "GL after set_param ok=%d", e->ok);
        fc_call(c, (void *)h_get_length);
        flame_log(FLAME_LOG_TRACE, "GL after call ok=%d", e->ok);
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

    default:
        e->ok = 0;
        flame_log(FLAME_LOG_TIER, "emit_one default: op=%d bc=%u", op, bc_off);
        break;
    }
    if (ok_at_entry && !e->ok)
        flame_log(FLAME_LOG_TIER, "emit_one broke e.ok: op=%d bc=%u", op, bc_off);
#undef U32
#undef U16
#undef I16
#undef I32
#undef U8
#undef I8
}

/* ------------------------------------------------------------------ */
/* Compilation driver                                                  */
/* ------------------------------------------------------------------ */

int flame_baseline_compile(JSContext *ctx, JSFunctionBytecode *b)
{
    FlameCmp c;
    FlameExecBlock *blk;
    FlameEmitter *e;
    const uint8_t *pc, *end;
    size_t code_cap = (size_t)b->byte_code_len * 128 + 4096;
    char *lbl = NULL;
    int prologue_end_label;

    if (b->func_kind != JS_FUNC_NORMAL || b->is_derived_class_constructor)
        goto refuse_quiet;
    if (b->stack_size > 4000 || b->byte_code_len <= 0)
        goto refuse_quiet;
    if (!baseline_whitelist_ok(b))
        goto refuse;

    blk = flame_rt_new_block(ctx->rt, code_cap);
    if (!blk)
        goto refuse;

    memset(&c, 0, sizeof(c));
    c.b = b;
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

    /* ---- prologue ----
     * int entry(FlameJitState *st)
     * rsp%16 == 8 on entry; push rbp + 5 callee-saved -> 8; sub 8 -> 0. */
    fl_mov_rr(e, FL_RSTATE, FL_RARG);          /* st */
    fl_push(e, FL_RSTATE);                     /* rbx */
    fl_push(e, FL_RSP);                        /* r15 */
    fl_push(e, FL_RLOC);                       /* r14 */
    fl_mov_rm(e, FL_RLOC, FL_RSTATE, FL_OFF_LOCAL_BUF);
    fl_mov_rm(e, FL_RSP, FL_RSTATE, FL_OFF_SP);
    /* rsp%16 == 0 here (entry 8, minus 3 pushes); calls keep it aligned
     * (SysV: no padding, Win x64: shadow space is 32 = 16-aligned). */

    if (!e->ok) { flame_log(FLAME_LOG_TIER, "prologue failed"); goto fail; }
    {
        char hex[512]; int i, n = b->byte_code_len < 60 ? b->byte_code_len : 60;
        for (i = 0; i < n; i++) sprintf(hex + i * 3, "%02x ", b->byte_code_buf[i]);
        hex[n*3] = 0;
        flame_log(FLAME_LOG_TRACE, "bc: %s", hex);
    }
    /* ---- body ---- */
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
    if (!e->ok) { flame_log(FLAME_LOG_TIER, "epilogue emission failed"); goto fail; }

    fl_finish(e);
    if (!e->ok) {
        int i, unb = 0;
        for (i = 0; i < e->label_count; i++)
            if (!e->labels[i].bound) unb++;
        flame_log(FLAME_LOG_TIER,
                  "fl_finish failed: labels=%d unbound=%d patches=%d",
                  e->label_count, unb, e->patch_count);
        {
            int i;
            for (i = 0; i < e->label_count; i++)
                if (!e->labels[i].bound)
                    flame_log(FLAME_LOG_TIER,
                              "  unbound label id=%d created@native:%u",
                              i, (uint32_t)e->labels[i].target);
        }
        goto fail;
    }

    js_free(ctx, c.labels);
    js_free(ctx, c.nat);
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
    flame_rt_register_func(ctx->rt, b, blk->base, e->len, 2);
    flame_exec_flush_icache(blk->base, e->len);

    if (JS_AtomIsString(ctx, b->func_name)) {
        const char *s = JS_AtomToCString(ctx, b->func_name);
        if (s) { lbl = js_strdup(ctx, s); JS_FreeCString(ctx, s); }
    }
    flame_log(FLAME_LOG_TIER,
              "baseline compiled: %s (%d bc bytes -> %zu native bytes)",
              lbl ? lbl : "<anon>", b->byte_code_len, e->len);
    js_free(ctx, lbl);
    return 0;

fail:
    flame_log(FLAME_LOG_TIER, "emit failed: e.ok=%d pc_off=%u",
              c.e.ok, (uint32_t)(pc - b->byte_code_buf));
    js_free(ctx, c.labels);
    js_free(ctx, c.nat);
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

/* ------------------------------------------------------------------ */
/* Entry from the interpreter                                          */
/* ------------------------------------------------------------------ */

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
    return b->flame_compiled != NULL;
}

JSValue flame_baseline_call(JSContext *caller_ctx, JSValueConst func_obj,
                            JSValueConst this_obj, int argc, JSValue *argv)
{
    JSRuntime *rt = caller_ctx->rt;
    JSObject *p = JS_VALUE_GET_OBJ(func_obj);
    JSFunctionBytecode *b = p->u.func.function_bytecode;
    FlameCompiledFunc *cf = (FlameCompiledFunc *)b->flame_compiled;
    typedef int (*FlameEntry)(FlameJitState *);
    FlameEntry fn = (FlameEntry)cf->code;
    FlameJitState *st;
    JSContext *ctx = b->realm;
    JSValue *local_buf;
    JSValue *pv;
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

    st = (FlameJitState *)alloca(sizeof(FlameJitState));
    memset(st, 0, sizeof(*st));
    local_buf = js_malloc(ctx, sizeof(JSValue) *
                          (size_t)(nlocals + b->var_count + b->stack_size));
    if (!local_buf)
        return JS_EXCEPTION;

    /* copy args (callee-owned, dup'd like JS_CALL_FLAG_COPY_ARGV) */
    {
        int ncopy = argc < b->arg_count ? argc : b->arg_count;
        for (i = 0; i < ncopy; i++)
            local_buf[i] = JS_DupValue(ctx, argv[i]);
        for (; i < nlocals + b->var_count; i++)
            local_buf[i] = JS_UNDEFINED;
    }

    st->ctx = ctx;
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
    fn(st);

    rt->current_stack_frame = st->sf.prev_frame;
    if (!list_empty(&st->sf.var_ref_list))
        close_var_refs(rt, &st->sf);
    if (st->sp > st->stack_base + b->stack_size ||
        st->sp < st->stack_base)
        st->sp = st->stack_base; /* JIT bug guard: never free out of range */
    for (pv = local_buf; pv < st->sp; pv++)
        JS_FreeValue(ctx, *pv);
    JS_FreeValue(ctx, st->this_val);
    js_free(ctx, local_buf);

    if (st->exception || st->exit_kind == 1)
        return JS_EXCEPTION;
    return st->retval;
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
    if (prof && prof->no_compile)
        return;
    flame_baseline_compile(ctx, b);
}
