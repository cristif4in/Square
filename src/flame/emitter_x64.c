/*
 * Flame — x86_64 emitter (SysV + Windows x64 ABIs).
 *
 * Conventions shared with flame_baseline.c:
 *   state register : rbx (callee-saved)
 *   value stack    : r15 (callee-saved)
 *   local buffer   : r14 (callee-saved)
 *   scratch        : rax, rcx, rdx, r8-r11 (volatile on both ABIs)
 *   native entry   : int entry(FlameJitState *st)
 *
 * Only instructions the tiers actually emit are implemented; anything
 * else marks the emitter failed and the function falls back to the
 * interpreter.
 */
#include "flame_emitter.h"
#include <string.h>

#if !defined(_WIN32)
#define FL_SYSV 1
#endif

/* register numbers (x86 encoding order) */
enum {
    X_RAX = 0, X_RCX, X_RDX, X_RBX, X_RSP, X_RBP, X_RSI, X_RDI,
    X_R8, X_R9, X_R10, X_R11, X_R12, X_R13, X_R14, X_R15,
};

static int fl_reg_x64[] = {
    [FL_RTMP0]  = X_RAX,
    [FL_RTMP1]  = X_RCX,
    [FL_RTMP2]  = X_RDX,
    [FL_RSTATE] = X_RBX,
    [FL_RSP]    = X_R15,
    [FL_RLOC]   = X_R14,
    [FL_RARG]   = X_RDX,  /* patched to the host ABI's arg register */
    [FL_RCALL]  = X_R11,
};

/* C argument register for the 1st integer argument. */
static int fl_arg_reg(void)
{
#ifdef FL_SYSV
    return X_RDI;
#else
    return X_RCX;
#endif
}

static void fl_reg_init(void)
{
    fl_reg_x64[FL_RARG] = fl_arg_reg();
}

static void fl_emit(FlameEmitter *e, const void *bytes, int n)
{
    if (e->len + (size_t)n > e->cap) {
        e->ok = 0;
        return;
    }
    memcpy(e->buf + e->len, bytes, n);
    e->len += n;
}

static void fl_emit8(FlameEmitter *e, uint8_t b)  { fl_emit(e, &b, 1); }

/* ModRM/SIB addressing: [base + disp].
 * NOTE: callers pre-emit their own REX byte (they know reg/base), so this
 * must NOT emit a prefix — it only emits ModRM(+SIB+disp). */
static void fl_modrm(FlameEmitter *e, int reg, int base, int32_t disp)
{
    if (base == X_RSP || base == X_R12) {
        /* SIB required */
        if (disp == 0) {
            fl_emit8(e, (uint8_t)(0x00 | ((reg & 7) << 3) | 0x04));
            fl_emit8(e, 0x24);
        } else if (disp >= -128 && disp <= 127) {
            fl_emit8(e, (uint8_t)(0x40 | ((reg & 7) << 3) | 0x04));
            fl_emit8(e, 0x24);
            fl_emit8(e, (uint8_t)disp);
        } else {
            fl_emit8(e, (uint8_t)(0x80 | ((reg & 7) << 3) | 0x04));
            fl_emit8(e, 0x24);
            fl_emit(e, &disp, 4);
        }
    } else {
        if (disp == 0 && base != X_RBP && base != X_R13) {
            fl_emit8(e, (uint8_t)(0x00 | ((reg & 7) << 3) | (base & 7)));
        } else if (disp >= -128 && disp <= 127) {
            fl_emit8(e, (uint8_t)(0x40 | ((reg & 7) << 3) | (base & 7)));
            fl_emit8(e, (uint8_t)disp);
        } else {
            fl_emit8(e, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
            fl_emit(e, &disp, 4);
        }
    }
}


void fl_init(FlameEmitter *e, void *buf, size_t cap)
{
    memset(e, 0, sizeof(*e));
    e->buf = (uint8_t *)buf;
    e->cap = cap;
    e->ok = 1;
    fl_reg_init();
}

int fl_label(FlameEmitter *e)
{
    if (e->label_count >= FL_MAX_LABELS) { e->ok = 0; return 0; }
    e->labels[e->label_count].bound = 0;
    e->labels[e->label_count].target = e->len; /* creation offset (debug) */
    return e->label_count++;
}

void fl_bind(FlameEmitter *e, int label)
{
    e->labels[label].bound = 1;
    e->labels[label].target = e->len;
}

void fl_bind_offset(FlameEmitter *e, int label, size_t target)
{
    e->labels[label].bound = 1;
    e->labels[label].target = target;
}

static void fl_patch_add(FlameEmitter *e, int label, int cond)
{
    /* forward references are fine (fl_resolve patches them); only reject
     * out-of-range labels */
    if (label < 0 || label >= FL_MAX_LABELS) { e->ok = 0; return; }
    if (e->patch_count >= FL_MAX_PATCHES) { e->ok = 0; return; }
    e->patches[e->patch_count].at = e->len;
    e->patches[e->patch_count].label = label;
    e->patches[e->patch_count].cond = cond;
    e->patch_count++;
}

static const uint8_t fl_jcc_op[] = {
    [FL_EQ] = 0x84, [FL_NE] = 0x85, [FL_LT] = 0x8C, [FL_GE] = 0x8D,
    [FL_LE] = 0x8E, [FL_GT] = 0x8F, [FL_UB] = 0x82, [FL_UBE] = 0x86,
    [FL_UA] = 0x87, [FL_UAE] = 0x83, [FL_VS] = 0x80, [FL_VC] = 0x81,
};

void fl_jmp(FlameEmitter *e, int label)
{
    fl_emit8(e, 0xE9);
    fl_patch_add(e, label, -1);
    { uint32_t z = 0; fl_emit(e, &z, 4); }
}

void fl_jcc(FlameEmitter *e, FlCond cond, int label)
{
    fl_emit8(e, 0x0F);
    fl_emit8(e, fl_jcc_op[cond]);
    fl_patch_add(e, label, cond);
    { uint32_t z = 0; fl_emit(e, &z, 4); }
}

static void fl_resolve(FlameEmitter *e)
{
    int i;
    for (i = 0; i < e->label_count; i++) {
        if (!e->labels[i].bound) {
            e->ok = 0;
            return;
        }
    }
    for (i = 0; i < e->patch_count; i++) {
        size_t at = e->patches[i].at;
        int lab = e->patches[i].label;
        if (lab < 0 || lab >= e->label_count) { e->ok = 0; return; }
        {
            int32_t rel = (int32_t)(e->labels[lab].target - (at + 4));
            memcpy(e->buf + at, &rel, 4);
        }
    }
}

/* Called by the tier compiler after the last emission. */
void fl_finish(FlameEmitter *e)
{
    fl_resolve(e);
}


void fl_mov_rr(FlameEmitter *e, FlReg dst, FlReg src)
{
    int d = fl_reg_x64[dst], s = fl_reg_x64[src];
    fl_emit8(e, (uint8_t)(0x48 | (d >= 8 ? 4 : 0) | (s >= 8 ? 1 : 0)));
    fl_emit8(e, 0x89);           /* mov dst, src */
    fl_emit8(e, (uint8_t)(0xC0 | ((s & 7) << 3) | (d & 7)));
}

void fl_mov_ri32(FlameEmitter *e, FlReg dst, int32_t imm)
{
    int r = fl_reg_x64[dst];
    if (r >= 8) fl_emit8(e, 0x40 | (r - 8));
    fl_emit8(e, 0xB8 + (r & 7));  /* mov r32, imm32 (zero-extends to 64) */
    fl_emit(e, &imm, 4);
}

void fl_mov_ri64(FlameEmitter *e, FlReg dst, uint64_t imm)
{
    int r = fl_reg_x64[dst];
    /* B8+r uses the rm field, extended by REX.B; W=1 for 64-bit */
    fl_emit8(e, (uint8_t)(0x48 | (r >= 8 ? 1 : 0)));
    fl_emit8(e, 0xB8 + (r & 7));  /* movabs r64, imm64 */
    fl_emit(e, &imm, 8);
}

void fl_mov32_ri(FlameEmitter *e, FlReg dst, uint32_t imm)
{
    int r = fl_reg_x64[dst];
    if (r >= 8) fl_emit8(e, 0x41); /* REX.B */
    fl_emit8(e, 0xB8 + (r & 7));
    fl_emit(e, &imm, 4);
}

void fl_mov_rm(FlameEmitter *e, FlReg dst, FlReg base, int32_t disp)
{
    int r = fl_reg_x64[dst];
    fl_emit8(e, (uint8_t)(0x48 | (r >= 8 ? 4 : 0) |
                          (fl_reg_x64[base] >= 8 ? 1 : 0)));
    fl_emit8(e, 0x8B);
    fl_modrm(e, r, fl_reg_x64[base], disp);
}

void fl_mov_mr(FlameEmitter *e, FlReg base, int32_t disp, FlReg src)
{
    int r = fl_reg_x64[src];
    fl_emit8(e, (uint8_t)(0x48 | (r >= 8 ? 4 : 0) |
                          (fl_reg_x64[base] >= 8 ? 1 : 0)));
    fl_emit8(e, 0x89);
    fl_modrm(e, r, fl_reg_x64[base], disp);
}

void fl_mov32_rm(FlameEmitter *e, FlReg dst, FlReg base, int32_t disp)
{
    int r = fl_reg_x64[dst];
    int rex = 0x40 | (r >= 8 ? 4 : 0) | (fl_reg_x64[base] >= 8 ? 1 : 0);
    if (rex != 0x40) fl_emit8(e, (uint8_t)rex);
    fl_emit8(e, 0x8B);
    fl_modrm(e, r, fl_reg_x64[base], disp);
}

void fl_mov32_mr(FlameEmitter *e, FlReg base, int32_t disp, FlReg src)
{
    int r = fl_reg_x64[src];
    int rex = 0x40 | (r >= 8 ? 4 : 0) | (fl_reg_x64[base] >= 8 ? 1 : 0);
    if (rex != 0x40) fl_emit8(e, (uint8_t)rex);
    fl_emit8(e, 0x89);
    fl_modrm(e, r, fl_reg_x64[base], disp);
}

void fl_lea_rm(FlameEmitter *e, FlReg dst, FlReg base, int32_t disp)
{
    int r = fl_reg_x64[dst];
    fl_emit8(e, (uint8_t)(0x48 | (r >= 8 ? 4 : 0) |
                          (fl_reg_x64[base] >= 8 ? 1 : 0)));
    fl_emit8(e, 0x8D);
    fl_modrm(e, r, fl_reg_x64[base], disp);
}


void fl_add_rr(FlameEmitter *e, FlReg dst, FlReg src)
{
    int d = fl_reg_x64[dst], s = fl_reg_x64[src];
    fl_emit8(e, (uint8_t)(0x48 | (s >= 8 ? 4 : 0) | (d >= 8 ? 1 : 0)));
    fl_emit8(e, 0x01);
    fl_emit8(e, (uint8_t)(0xC0 | ((s & 7) << 3) | (d & 7)));
}

static void fl_addsub_ri(FlameEmitter *e, FlReg dst, int32_t imm, int opext)
{
    int r = fl_reg_x64[dst];
    fl_emit8(e, (uint8_t)(0x48 | (r >= 8 ? 1 : 0)));
    fl_emit8(e, 0x81);
    fl_emit8(e, (uint8_t)(0xC0 | (opext << 3) | (r & 7)));
    fl_emit(e, &imm, 4);
}

void fl_add_ri(FlameEmitter *e, FlReg dst, int32_t imm)
{ fl_addsub_ri(e, dst, imm, 0); }

void fl_sub_ri(FlameEmitter *e, FlReg dst, int32_t imm)
{ fl_addsub_ri(e, dst, imm, 5); }

void fl_add32_rr(FlameEmitter *e, FlReg dst, FlReg src)
{
    int d = fl_reg_x64[dst], s = fl_reg_x64[src];
    int rex = 0x40 | (s >= 8 ? 4 : 0) | (d >= 8 ? 1 : 0);
    if (rex != 0x40) fl_emit8(e, (uint8_t)rex);
    fl_emit8(e, 0x01);
    fl_emit8(e, (uint8_t)(0xC0 | ((s & 7) << 3) | (d & 7)));
}

void fl_mul32_rr(FlameEmitter *e, FlReg dst, FlReg src)
{
    int d = fl_reg_x64[dst], s = fl_reg_x64[src];
    int rex = 0x40 | (s >= 8 ? 4 : 0) | (d >= 8 ? 1 : 0);
    if (rex != 0x40) fl_emit8(e, (uint8_t)rex);
    fl_emit8(e, 0x0F);
    fl_emit8(e, 0xAF);           /* imul r32, r/m32 */
    fl_emit8(e, (uint8_t)(0xC0 | ((d & 7) << 3) | (s & 7)));
}

void fl_sub32_rr(FlameEmitter *e, FlReg dst, FlReg src)
{
    int d = fl_reg_x64[dst], s = fl_reg_x64[src];
    int rex = 0x40 | (s >= 8 ? 4 : 0) | (d >= 8 ? 1 : 0);
    if (rex != 0x40) fl_emit8(e, (uint8_t)rex);
    fl_emit8(e, 0x29);           /* sub r32, r/m32 */
    fl_emit8(e, (uint8_t)(0xC0 | ((s & 7) << 3) | (d & 7)));
}

void fl_not32(FlameEmitter *e, FlReg dst)
{
    int r = fl_reg_x64[dst];
    if (r >= 8) fl_emit8(e, 0x41);
    fl_emit8(e, 0xF7);
    fl_emit8(e, (uint8_t)(0xD0 | (r & 7)));  /* not r32 */
}

void fl_push(FlameEmitter *e, FlReg r)
{
    int x = fl_reg_x64[r];
    if (x >= 8) fl_emit8(e, 0x41);
    fl_emit8(e, (uint8_t)(0x50 + (x & 7)));
}

void fl_pop(FlameEmitter *e, FlReg r)
{
    int x = fl_reg_x64[r];
    if (x >= 8) fl_emit8(e, 0x41);
    fl_emit8(e, (uint8_t)(0x58 + (x & 7)));
}

static void fl_logic32(FlameEmitter *e, FlReg dst, FlReg src, uint8_t op)
{
    int d = fl_reg_x64[dst], s = fl_reg_x64[src];
    int rex = 0x40 | (s >= 8 ? 4 : 0) | (d >= 8 ? 1 : 0);
    if (rex != 0x40) fl_emit8(e, (uint8_t)rex);
    fl_emit8(e, op);
    fl_emit8(e, (uint8_t)(0xC0 | ((s & 7) << 3) | (d & 7)));
}

void fl_and32_rr(FlameEmitter *e, FlReg dst, FlReg src) { fl_logic32(e, dst, src, 0x21); }
void fl_or32_rr(FlameEmitter *e, FlReg dst, FlReg src)  { fl_logic32(e, dst, src, 0x09); }
void fl_xor32_rr(FlameEmitter *e, FlReg dst, FlReg src) { fl_logic32(e, dst, src, 0x31); }

void fl_shl32_ri(FlameEmitter *e, FlReg dst, int imm)
{
    int r = fl_reg_x64[dst];
    if (r >= 8) fl_emit8(e, 0x41);
    fl_emit8(e, 0xC1);
    fl_emit8(e, (uint8_t)(0xE0 | (r & 7)));  /* shl r32, imm8 */
    fl_emit8(e, (uint8_t)imm);
}

void fl_dec_mem32(FlameEmitter *e, FlReg ptr)
{
    int r = fl_reg_x64[ptr];
    if (r >= 8) fl_emit8(e, 0x41);
    fl_emit8(e, 0xFF);
    fl_emit8(e, (uint8_t)(0x08 | (r & 7)));  /* dec dword [ptr] (FF /1) */
}

void fl_sar32_cl(FlameEmitter *e, FlReg dst)
{
    int r = fl_reg_x64[dst];
    if (r >= 8) fl_emit8(e, 0x41);
    fl_emit8(e, 0xD3);
    fl_emit8(e, (uint8_t)(0xF8 | (r & 7)));
}

void fl_shl32_cl(FlameEmitter *e, FlReg dst)
{
    int r = fl_reg_x64[dst];
    if (r >= 8) fl_emit8(e, 0x41);
    fl_emit8(e, 0xD3);
    fl_emit8(e, (uint8_t)(0xE0 | (r & 7)));
}

void fl_shr32_cl(FlameEmitter *e, FlReg dst)
{
    int r = fl_reg_x64[dst];
    if (r >= 8) fl_emit8(e, 0x41);
    fl_emit8(e, 0xD3);
    fl_emit8(e, (uint8_t)(0xE8 | (r & 7)));
}

void fl_cmp_rr(FlameEmitter *e, FlReg a, FlReg b)
{
    int x = fl_reg_x64[a], y = fl_reg_x64[b];
    fl_emit8(e, (uint8_t)(0x48 | (y >= 8 ? 4 : 0) | (x >= 8 ? 1 : 0)));
    fl_emit8(e, 0x39);           /* cmp a, b */
    fl_emit8(e, (uint8_t)(0xC0 | ((y & 7) << 3) | (x & 7)));
}

void fl_cmp_ri(FlameEmitter *e, FlReg a, int32_t imm)
{
    int r = fl_reg_x64[a];
    fl_emit8(e, (uint8_t)(0x48 | (r >= 8 ? 1 : 0)));
    fl_emit8(e, 0x81);
    fl_emit8(e, (uint8_t)(0xF8 | (r & 7)));
    fl_emit(e, &imm, 4);
}

void fl_cmp_mem_ri(FlameEmitter *e, FlReg base, int32_t disp, int32_t imm)
{
    fl_emit8(e, (uint8_t)(0x48 | (fl_reg_x64[base] >= 8 ? 1 : 0)));
    fl_emit8(e, 0x81);
    fl_modrm(e, 7, fl_reg_x64[base], disp); /* /7 = cmp */
    fl_emit(e, &imm, 4);
}

void fl_cv64_32(FlameEmitter *e, FlReg dst, FlReg src)
{
    int d = fl_reg_x64[dst], s = fl_reg_x64[src];
    fl_emit8(e, (uint8_t)(0x48 | (d >= 8 ? 4 : 0) | (s >= 8 ? 1 : 0)));
    fl_emit8(e, 0x63);           /* movsxd */
    fl_emit8(e, (uint8_t)(0xC0 | ((d & 7) << 3) | (s & 7)));
}


void fl_call(FlameEmitter *e, void *func, FlReg argreg)
{
    /* movabs FL_RCALL, func ; mov argreg -> FL_RARG ; [pad] ; call FL_RCALL */
    fl_mov_ri64(e, FL_RCALL, (uint64_t)(uintptr_t)func);
    if (argreg != FL_RARG) {
        int d = fl_arg_reg(), s = fl_reg_x64[argreg];
        fl_emit8(e, (uint8_t)(0x48 | (d >= 8 ? 4 : 0) | (s >= 8 ? 1 : 0)));
        fl_emit8(e, 0x89);
        fl_emit8(e, (uint8_t)(0xC0 | ((s & 7) << 3) | (d & 7)));
    }
#ifndef FL_SYSV
    {   /* machine-rsp shadow store (NOT the value-stack register) */
        uint8_t sub_rsp[4] = { 0x48, 0x83, 0xEC, 0x20 };
        uint8_t add_rsp[4] = { 0x48, 0x83, 0xC4, 0x20 };
        fl_emit(e, sub_rsp, 4);
#endif
    {
        int r = fl_reg_x64[FL_RCALL];
        if (r >= 8) fl_emit8(e, 0x41);
        fl_emit8(e, 0xFF);
        fl_emit8(e, (uint8_t)(0xD0 | (r & 7)));
    }
#ifndef FL_SYSV
        fl_emit(e, add_rsp, 4);
    }
#endif
}

void fl_ret(FlameEmitter *e)
{
    fl_emit8(e, 0xC3);
}


/* prefix bytes + REX for a reg-reg xmm op */
static void fl_fprefix_rr(FlameEmitter *e, uint8_t pfx, int rd, int rs)
{
    uint8_t rex = 0x40 | ((rd >= 8) ? 4 : 0) | ((rs >= 8) ? 1 : 0);
    if (pfx == 0x66) fl_emit8(e, 0x66);
    if (pfx == 0xF2) fl_emit8(e, 0xF2);
    if (rex != 0x40) fl_emit8(e, rex);
}

void fl_fmov_rr(FlameEmitter *e, int vd, int vs)
{
    fl_fprefix_rr(e, 0xF2, vd, vs);
    fl_emit8(e, 0x0F);
    fl_emit8(e, 0x10);  /* movsd xmm_d, xmm_s */
    fl_emit8(e, (uint8_t)(0xC0 | (vd << 3) | vs));
}

void fl_fload(FlameEmitter *e, int vd, FlReg base, int32_t disp)
{
    int b = fl_reg_x64[base];
    uint8_t rex = 0x40 | ((vd >= 8) ? 4 : 0) | ((b >= 8) ? 1 : 0);
    fl_emit8(e, 0xF2);
    if (rex != 0x40) fl_emit8(e, rex);
    fl_emit8(e, 0x0F);
    fl_emit8(e, 0x10);  /* movsd xmm_d, [base+disp] */
    fl_modrm(e, vd, b, disp);
}

void fl_fstore(FlameEmitter *e, int vd, FlReg base, int32_t disp)
{
    int b = fl_reg_x64[base];
    uint8_t rex = 0x40 | ((vd >= 8) ? 4 : 0) | ((b >= 8) ? 1 : 0);
    fl_emit8(e, 0xF2);
    if (rex != 0x40) fl_emit8(e, rex);
    fl_emit8(e, 0x0F);
    fl_emit8(e, 0x11);  /* movsd [base+disp], xmm_d */
    fl_modrm(e, vd, b, disp);
}

static void fl_fop(FlameEmitter *e, int vd, int vs, uint8_t op)
{
    fl_fprefix_rr(e, 0xF2, vd, vs);
    fl_emit8(e, 0x0F);
    fl_emit8(e, op);
    fl_emit8(e, (uint8_t)(0xC0 | (vd << 3) | vs));
}

void fl_fadd(FlameEmitter *e, int vd, int vs) { fl_fop(e, vd, vs, 0x58); }
void fl_fsub(FlameEmitter *e, int vd, int vs) { fl_fop(e, vd, vs, 0x5C); }
void fl_fmul(FlameEmitter *e, int vd, int vs) { fl_fop(e, vd, vs, 0x59); }
void fl_fdiv(FlameEmitter *e, int vd, int vs) { fl_fop(e, vd, vs, 0x5E); }

void fl_fcmp(FlameEmitter *e, int va, int vb)
{
    /* comisd: unordered sets ZF=PF=CF=1 */
    fl_fprefix_rr(e, 0x66, va, vb);
    fl_emit8(e, 0x0F);
    fl_emit8(e, 0x2F);
    fl_emit8(e, (uint8_t)(0xC0 | (va << 3) | vb));
}

void fl_f2i32(FlameEmitter *e, FlReg dst, int vs)
{
    int r = fl_reg_x64[dst];
    uint8_t rex = 0x40 | ((r >= 8) ? 4 : 0);
    fl_emit8(e, 0xF2);
    if (rex != 0x40) fl_emit8(e, rex);
    fl_emit8(e, 0x0F);
    fl_emit8(e, 0x2C);  /* cvttsd2si r32, xmm */
    fl_emit8(e, (uint8_t)(0xC0 | ((r & 7) << 3) | vs));
}

void fl_i32_2f(FlameEmitter *e, int vd, FlReg src)
{
    int r = fl_reg_x64[src];
    uint8_t rex = 0x40 | ((vd >= 8) ? 4 : 0) | ((r >= 8) ? 1 : 0);
    fl_emit8(e, 0xF2);
    if (rex != 0x40) fl_emit8(e, rex);
    fl_emit8(e, 0x0F);
    fl_emit8(e, 0x2A);  /* cvtsi2sd xmm_d, r32 */
    fl_emit8(e, (uint8_t)(0xC0 | (vd << 3) | (r & 7)));
}

/* movq xmm_d, r64 — 66 REX.W 0F 6E /r */
void fl_movq_xmm_gp(FlameEmitter *e, int vd, FlReg src)
{
    int r = fl_reg_x64[src];
    uint8_t rex = (uint8_t)(0x48 | ((vd >= 8) ? 4 : 0) | ((r >= 8) ? 1 : 0));
    fl_emit8(e, 0x66);
    fl_emit8(e, rex);
    fl_emit8(e, 0x0F);
    fl_emit8(e, 0x6E);
    fl_emit8(e, (uint8_t)(0xC0 | (vd << 3) | (r & 7)));
}

/* movq r64_d, xmm_s — 66 REX.W 0F 7E /r */
void fl_movq_gp_xmm(FlameEmitter *e, FlReg dst, int vs)
{
    int r = fl_reg_x64[dst];
    uint8_t rex = (uint8_t)(0x48 | ((r >= 8) ? 4 : 0) | ((vs >= 8) ? 1 : 0));
    fl_emit8(e, 0x66);
    fl_emit8(e, rex);
    fl_emit8(e, 0x0F);
    fl_emit8(e, 0x7E);
    fl_emit8(e, (uint8_t)(0xC0 | ((r & 7) << 3) | vs));
}

void fl_fmov_ri64(FlameEmitter *e, int vd, uint64_t bits)
{
    fl_mov_ri64(e, FL_RTMP2, bits);
    fl_movq_xmm_gp(e, vd, FL_RTMP2);
}

const char *fl_arch_name(void) { return "x86_64"; }
