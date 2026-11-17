/*
 * Flame — QuickJS bridge (included by quickjs.c near the top).
 *
 * Declares the hook entry points that quickjs.c calls while interpreting.
 * The implementations live in the Flame sources, which quickjs.c includes
 * at the bottom of the file (flame_all.h), so they see every internal
 * struct and helper without any ABI duplication.
 *
 * All hooks early-return on inactive state, so the interpreter cost when
 * Flame is disabled is one predictable branch per hook site.
 */
#ifndef SQUARE_FLAME_BRIDGE_H
#define SQUARE_FLAME_BRIDGE_H

#if !defined(CONFIG_FLAME)
#define CONFIG_FLAME 1
#endif

#if CONFIG_FLAME

#include "flame_internal.h"

/* QuickJS internal structs, fully defined later in quickjs.c. */
struct JSRuntime;
struct JSContext;
struct JSObject;
struct JSFunctionBytecode;
struct JSStackFrame;

/* Implemented in flame sources (included at the bottom of quickjs.c). */
void flame_global_init(void);
void flame_hook_function_entry(struct JSContext *ctx,
                               struct JSObject *func_obj,
                               struct JSFunctionBytecode *b);
void flame_hook_loop_backedge(struct JSFunctionBytecode *b, uint32_t pc_off);
void flame_hook_call_site(struct JSFunctionBytecode *b, uint32_t pc_off,
                          int argc, const JSValue *argv);
void flame_hook_binop(struct JSFunctionBytecode *b, uint32_t pc_off,
                      int op, JSValueConst a, JSValueConst b2);
void flame_hook_get_field(struct JSFunctionBytecode *b, uint32_t pc_off,
                          JSValueConst obj, JSAtom atom);
void flame_hook_get_array_el(struct JSFunctionBytecode *b, uint32_t pc_off,
                             JSValueConst obj, JSValueConst prop);

/* Free a per-bytecode profiling side table (called from bytecode finalizer). */
void flame_free_profile(struct JSRuntime *rt, void *prof);

/* Backtrace suppression: while executing Flame's own helpers we want a
 * clean "jit" frame marker. quickjs.c consults this in build_backtrace. */
int flame_is_jit_frame(struct JSStackFrame *sf);

/* True if Flame should take over this call before the interpreter runs
 * (function entry tier decision). Returns the JSValue result when it does,
 * with *handled set; otherwise *handled stays 0 and the interpreter runs. */
JSValue flame_try_enter_jit(struct JSContext *ctx, JSValueConst func_obj,
                            JSValueConst this_obj, int argc, JSValue *argv,
                            int *handled);

#endif /* CONFIG_FLAME */
#endif /* SQUARE_FLAME_BRIDGE_H */
