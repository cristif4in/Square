/*
 * Flame — ARM64 (aarch64) emitter.
 *
 * Register mapping (matches flame_baseline.c expectations):
 *   state : x19 (callee-saved)
 *   sp    : x28 (callee-saved)
 *   local : x27 (callee-saved)
 *   call  : x16 (IP0, br x16)
 *   entry : int entry(FlameJitState *st)  — arg in x0
 *
 * Implemented in phase 6; until then compile attempts fail cleanly and
 * the interpreter runs the function (identical semantics, zero risk).
 */
#include "flame_emitter.h"
#include <string.h>

void fl_init(FlameEmitter *e, void *buf, size_t cap)
{
    memset(e, 0, sizeof(*e));
    e->buf = (uint8_t *)buf;
    e->cap = cap;
    e->ok = 0; /* ARM64 backend lands in phase 6: always fail for now */
}

int fl_label(FlameEmitter *e)                { (void)e; e->ok = 0; return 0; }
void fl_bind(FlameEmitter *e, int label)     { (void)e; (void)label; }
void fl_jmp(FlameEmitter *e, int label)      { (void)e; (void)label; e->ok = 0; }
void fl_jcc(FlameEmitter *e, FlCond c, int l){ (void)e; (void)c; (void)l; e->ok = 0; }
void fl_call(FlameEmitter *e, void *f, FlReg a) { (void)e; (void)f; (void)a; e->ok = 0; }
void fl_ret(FlameEmitter *e)                 { (void)e; e->ok = 0; }

void fl_mov_rr(FlameEmitter *e, FlReg d, FlReg s)   { (void)e; (void)d; (void)s; e->ok = 0; }
void fl_mov_ri32(FlameEmitter *e, FlReg d, int32_t i){ (void)e; (void)d; (void)i; e->ok = 0; }
void fl_mov_ri64(FlameEmitter *e, FlReg d, uint64_t i){ (void)e; (void)d; (void)i; e->ok = 0; }
void fl_mov32_ri(FlameEmitter *e, FlReg d, uint32_t i){ (void)e; (void)d; (void)i; e->ok = 0; }
void fl_mov_rm(FlameEmitter *e, FlReg d, FlReg b, int32_t o){ (void)e; (void)d; (void)b; (void)o; e->ok = 0; }
void fl_mov_mr(FlameEmitter *e, FlReg b, int32_t o, FlReg s){ (void)e; (void)b; (void)o; (void)s; e->ok = 0; }
void fl_mov32_rm(FlameEmitter *e, FlReg d, FlReg b, int32_t o){ (void)e; (void)d; (void)b; (void)o; e->ok = 0; }
void fl_mov32_mr(FlameEmitter *e, FlReg b, int32_t o, FlReg s){ (void)e; (void)b; (void)o; (void)s; e->ok = 0; }
void fl_lea_rm(FlameEmitter *e, FlReg d, FlReg b, int32_t o){ (void)e; (void)d; (void)b; (void)o; e->ok = 0; }

void fl_add_rr(FlameEmitter *e, FlReg d, FlReg s)  { (void)e; (void)d; (void)s; e->ok = 0; }
void fl_add_ri(FlameEmitter *e, FlReg d, int32_t i){ (void)e; (void)d; (void)i; e->ok = 0; }
void fl_sub_ri(FlameEmitter *e, FlReg d, int32_t i){ (void)e; (void)d; (void)i; e->ok = 0; }
void fl_add32_rr(FlameEmitter *e, FlReg d, FlReg s){ (void)e; (void)d; (void)s; e->ok = 0; }
void fl_sub32_rr(FlameEmitter *e, FlReg d, FlReg s){ (void)e; (void)d; (void)s; e->ok = 0; }
void fl_not32(FlameEmitter *e, FlReg d)            { (void)e; (void)d; e->ok = 0; }
void fl_push(FlameEmitter *e, FlReg r)             { (void)e; (void)r; e->ok = 0; }
void fl_pop(FlameEmitter *e, FlReg r)              { (void)e; (void)r; e->ok = 0; }
void fl_mul32_rr(FlameEmitter *e, FlReg d, FlReg s){ (void)e; (void)d; (void)s; e->ok = 0; }
void fl_and32_rr(FlameEmitter *e, FlReg d, FlReg s){ (void)e; (void)d; (void)s; e->ok = 0; }
void fl_or32_rr(FlameEmitter *e, FlReg d, FlReg s) { (void)e; (void)d; (void)s; e->ok = 0; }
void fl_xor32_rr(FlameEmitter *e, FlReg d, FlReg s){ (void)e; (void)d; (void)s; e->ok = 0; }
void fl_sar32_cl(FlameEmitter *e, FlReg d)         { (void)e; (void)d; e->ok = 0; }
void fl_shl32_cl(FlameEmitter *e, FlReg d)         { (void)e; (void)d; e->ok = 0; }
void fl_shr32_cl(FlameEmitter *e, FlReg d)         { (void)e; (void)d; e->ok = 0; }
void fl_cmp_rr(FlameEmitter *e, FlReg a, FlReg b)  { (void)e; (void)a; (void)b; e->ok = 0; }
void fl_cmp_ri(FlameEmitter *e, FlReg a, int32_t i){ (void)e; (void)a; (void)i; e->ok = 0; }
void fl_cmp_mem_ri(FlameEmitter *e, FlReg b, int32_t o, int32_t i){ (void)e; (void)b; (void)o; (void)i; e->ok = 0; }
void fl_cv64_32(FlameEmitter *e, FlReg d, FlReg s) { (void)e; (void)d; (void)s; e->ok = 0; }

void fl_fmov_rr(FlameEmitter *e, int d, int s)     { (void)e; (void)d; (void)s; e->ok = 0; }
void fl_fload(FlameEmitter *e, int d, FlReg b, int32_t o){ (void)e; (void)d; (void)b; (void)o; e->ok = 0; }
void fl_fstore(FlameEmitter *e, int d, FlReg b, int32_t o){ (void)e; (void)d; (void)b; (void)o; e->ok = 0; }
void fl_fadd(FlameEmitter *e, int d, int s)        { (void)e; (void)d; (void)s; e->ok = 0; }
void fl_fsub(FlameEmitter *e, int d, int s)        { (void)e; (void)d; (void)s; e->ok = 0; }
void fl_fmul(FlameEmitter *e, int d, int s)        { (void)e; (void)d; (void)s; e->ok = 0; }
void fl_fdiv(FlameEmitter *e, int d, int s)        { (void)e; (void)d; (void)s; e->ok = 0; }
void fl_fcmp(FlameEmitter *e, int a, int b)        { (void)e; (void)a; (void)b; e->ok = 0; }
void fl_f2i32(FlameEmitter *e, FlReg d, int s)     { (void)e; (void)d; (void)s; e->ok = 0; }
void fl_i32_2f(FlameEmitter *e, int d, FlReg s)    { (void)e; (void)d; (void)s; e->ok = 0; }
void fl_fmov_ri64(FlameEmitter *e, int d, uint64_t b){ (void)e; (void)d; (void)b; e->ok = 0; }
void fl_movq_xmm_gp(FlameEmitter *e, int d, FlReg s){ (void)e; (void)d; (void)s; e->ok = 0; }
void fl_movq_gp_xmm(FlameEmitter *e, FlReg d, int s){ (void)e; (void)d; (void)s; e->ok = 0; }

void fl_finish(FlameEmitter *e) { (void)e; }

const char *fl_arch_name(void) { return "aarch64"; }
