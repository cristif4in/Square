/*
 * Flame — machine emitter abstraction.
 *
 * A deliberately small virtual ISA: exactly what the baseline/optimizing
 * tiers need (value-stack traffic, tag tests, int fast paths, helper calls
 * and branch patching). Two backends implement it:
 *   emitter_x64.c    — x86_64, SysV and Windows ABIs
 *   emitter_arm64.c  — aarch64 (Apple/Win/Linux)
 *
 * Helpers are always called as `void (*)(FlameJitState *)` — a single
 * pointer argument — which keeps the call sites identical under both x64
 * ABIs and trivially portable to ARM64. Per-site constants travel through
 * the state's `param` field.
 */
#ifndef SQUARE_FLAME_EMITTER_H
#define SQUARE_FLAME_EMITTER_H

#include <stddef.h>
#include <stdint.h>

/* Virtual registers (arch-mapped by the backends). */
typedef enum FlReg {
    FL_RTMP0 = 0,   /* volatile scratch */
    FL_RTMP1,       /* volatile scratch */
    FL_RTMP2,       /* volatile scratch */
    FL_RSTATE,      /* callee-saved: FlameJitState* */
    FL_RSP,         /* callee-saved: value stack top */
    FL_RLOC,        /* callee-saved: local_buf */
    FL_RARG,        /* first C argument register */
    FL_RCALL,       /* volatile: call target register */
} FlReg;

/* Conditions for fl_jcc / fl_setcc. */
typedef enum FlCond {
    FL_EQ = 0, FL_NE, FL_LT, FL_GE, FL_LE, FL_GT,
    FL_UB, FL_UBE, FL_UA, FL_UAE, FL_VS, FL_VC,
} FlCond;

#define FL_MAX_LABELS  1024
#define FL_MAX_PATCHES 8192

typedef struct FlameEmitter {
    uint8_t *buf;
    size_t cap;
    size_t len;
    int ok;                    /* 0 once any encoding failed */
    /* label patching (rel32/imm26) */
    struct {
        int bound;
        size_t target;
    } labels[FL_MAX_LABELS];
    struct {
        size_t at;             /* offset of the 4-byte patch field */
        int label;
        int cond;              /* -1 for unconditional jmp */
    } patches[FL_MAX_PATCHES];
    int label_count, patch_count;
} FlameEmitter;

void fl_init(FlameEmitter *e, void *buf, size_t cap);
void fl_bind(FlameEmitter *e, int label);
void fl_bind_offset(FlameEmitter *e, int label, size_t target);
int fl_label(FlameEmitter *e);

/* Control flow */
void fl_jmp(FlameEmitter *e, int label);
void fl_jcc(FlameEmitter *e, FlCond cond, int label);
void fl_call(FlameEmitter *e, void *func, FlReg argreg);
void fl_ret(FlameEmitter *e);

/* Moves / memory */
void fl_mov_rr(FlameEmitter *e, FlReg dst, FlReg src);
void fl_mov_ri32(FlameEmitter *e, FlReg dst, int32_t imm);
void fl_mov_ri64(FlameEmitter *e, FlReg dst, uint64_t imm);
void fl_mov32_ri(FlameEmitter *e, FlReg dst, uint32_t imm);
void fl_mov_rm(FlameEmitter *e, FlReg dst, FlReg base, int32_t disp);
void fl_mov_mr(FlameEmitter *e, FlReg base, int32_t disp, FlReg src);
void fl_mov32_rm(FlameEmitter *e, FlReg dst, FlReg base, int32_t disp);
void fl_mov32_mr(FlameEmitter *e, FlReg base, int32_t disp, FlReg src);
void fl_lea_rm(FlameEmitter *e, FlReg dst, FlReg base, int32_t disp);

/* Arithmetic / compare */
void fl_add_rr(FlameEmitter *e, FlReg dst, FlReg src);
void fl_add_ri(FlameEmitter *e, FlReg dst, int32_t imm);
void fl_sub_ri(FlameEmitter *e, FlReg dst, int32_t imm);
void fl_add32_rr(FlameEmitter *e, FlReg dst, FlReg src); /* sets OF */
void fl_sub32_rr(FlameEmitter *e, FlReg dst, FlReg src); /* sets OF */
void fl_mul32_rr(FlameEmitter *e, FlReg dst, FlReg src); /* sets OF */
void fl_not32(FlameEmitter *e, FlReg dst);
void fl_push(FlameEmitter *e, FlReg r);
void fl_pop(FlameEmitter *e, FlReg r);
void fl_and32_rr(FlameEmitter *e, FlReg dst, FlReg src);
void fl_or32_rr(FlameEmitter *e, FlReg dst, FlReg src);
void fl_xor32_rr(FlameEmitter *e, FlReg dst, FlReg src);
void fl_sar32_cl(FlameEmitter *e, FlReg dst);            /* shift by cl */
void fl_shl32_cl(FlameEmitter *e, FlReg dst);
void fl_shr32_cl(FlameEmitter *e, FlReg dst);
void fl_cmp_rr(FlameEmitter *e, FlReg a, FlReg b);
void fl_cmp_ri(FlameEmitter *e, FlReg a, int32_t imm);
void fl_cmp_mem_ri(FlameEmitter *e, FlReg base, int32_t disp, int32_t imm);
void fl_cv64_32(FlameEmitter *e, FlReg dst, FlReg src);  /* movsxd */

/* F64 math on scratch registers (baseline double fast paths) */
void fl_fmov_rr(FlameEmitter *e, int vd, int vs);
void fl_fload(FlameEmitter *e, int vd, FlReg base, int32_t disp);
void fl_fstore(FlameEmitter *e, int vd, FlReg base, int32_t disp);
void fl_fadd(FlameEmitter *e, int vd, int vs);
void fl_fsub(FlameEmitter *e, int vd, int vs);
void fl_fmul(FlameEmitter *e, int vd, int vs);
void fl_fdiv(FlameEmitter *e, int vd, int vs);
void fl_fcmp(FlameEmitter *e, int va, int vb); /* sets EFLAGS for FL_* */
void fl_f2i32(FlameEmitter *e, FlReg dst, int vs);   /* cvttsd2si */
void fl_i32_2f(FlameEmitter *e, int vd, FlReg src);  /* cvtsi2sd */
void fl_fmov_ri64(FlameEmitter *e, int vd, uint64_t bits); /* via gp reg */
void fl_movq_xmm_gp(FlameEmitter *e, int vd, FlReg src);
void fl_movq_gp_xmm(FlameEmitter *e, FlReg dst, int vs);

/* Resolve pending patches; clears e->ok if a label is unbound.
 * Must be called after the last emission, before executing the buffer. */
void fl_finish(FlameEmitter *e);

/* Architecture name for logs. */
const char *fl_arch_name(void);

#endif /* SQUARE_FLAME_EMITTER_H */
