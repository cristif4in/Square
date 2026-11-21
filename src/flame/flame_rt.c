/*
 * Flame — per-runtime state, tiering policy, executable memory.
 *
 * Each JSRuntime owns one FlameRT (JSRuntime.flame_rt). It tracks the
 * JIT code cache (one allocation block per compiled function), tiering
 * statistics, and the per-runtime enable flag. Executable memory uses
 * VirtualAlloc on Windows and mmap(PROT_EXEC) elsewhere; on ARM64 the
 * instruction cache is flushed after every emission round.
 */
#include "flame_internal.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif


uint64_t flame_stat_compiles;       /* successful JIT compilations */
uint64_t flame_stat_compile_fails;  /* unsupported bytecode / OOM */
uint64_t flame_stat_jit_calls;      /* activations that ran JIT code */
uint64_t flame_stat_deopts;         /* guard failures -> interpreter */
uint64_t flame_stat_osr;            /* on-stack replacement entries */
uint64_t flame_stat_native_bytes;   /* emitted machine code bytes */


void *flame_exec_alloc(size_t size)
{
#if defined(_WIN32)
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                        PAGE_EXECUTE_READWRITE);
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

void flame_exec_free(void *ptr, size_t size)
{
#if defined(_WIN32)
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

void flame_exec_flush_icache(void *ptr, size_t size)
{
#if defined(__aarch64__) && !defined(_WIN32)
    __builtin___clear_cache((char *)ptr, (char *)ptr + size);
#elif defined(__aarch64__)
    FlushInstructionCache(GetCurrentProcess(), ptr, size);
#else
    (void)ptr; (void)size; /* x86/x64 needs no icache flush */
#endif
}



/* Process-wide kill-switch (SQRL_NO_JIT / flame_set_global_enabled). */
static int flame_global_enabled = 1;

int flame_global_enabled_flag(void)
{
    return flame_global_enabled;
}

void flame_set_global_enabled(int enabled)
{
    flame_global_enabled = !!enabled;
}

typedef struct FlameRT {
    int enabled;                 /* per-runtime gate (default on) */
    FlameCompiledFunc *funcs;    /* linked list of compiled functions */
    int func_count;
    /* per-compile exec blocks (freed with the runtime; simple v1 policy:
     * JIT code is never freed while the runtime lives — code blocks are
     * small and bound by the number of hot functions) */
    FlameExecBlock *blocks;
    int block_count, block_cap;
} FlameRT;

static FlameRT *flame_rt_get(JSRuntime *rt)
{
    FlameRT *frt = (FlameRT *)rt->flame_rt;
    if (!frt) {
        frt = js_mallocz_rt(rt, sizeof(FlameRT));
        if (!frt)
            return NULL;
        frt->enabled = 1;
        rt->flame_rt = frt;
    }
    return frt;
}

void flame_rt_dispose(JSRuntime *rt)
{
    FlameRT *frt = (FlameRT *)rt->flame_rt;
    if (!frt)
        return;
    {
        FlameCompiledFunc *f = frt->funcs;
        while (f) {
            FlameCompiledFunc *n = f->next;
            js_free_rt(rt, f);
            f = n;
        }
    }
    {
        int i;
        for (i = 0; i < frt->block_count; i++)
            flame_exec_free(frt->blocks[i].base, frt->blocks[i].size);
        js_free_rt(rt, frt->blocks);
    }
    js_free_rt(rt, frt);
    rt->flame_rt = NULL;
}


int flame_rt_is_enabled(JSRuntime *rt)
{
    FlameRT *frt;
    if (!flame_global_enabled_flag())
        return 0;
    if (!rt)
        return 1;
    frt = (FlameRT *)rt->flame_rt;
    return frt ? frt->enabled : 1; /* lazily-created => default on */
}

int flame_set_enabled(JSRuntime *rt, int enabled)
{
    FlameRT *frt = flame_rt_get(rt);
    int prev;
    if (!frt)
        return flame_rt_is_enabled(rt);
    prev = frt->enabled;
    frt->enabled = !!enabled;
    flame_log(FLAME_LOG_TIER, "runtime %p JIT %s", (void *)rt,
              enabled ? "enabled" : "disabled");
    return prev;
}

int flame_is_enabled(JSRuntime *rt)
{
    return flame_rt_is_enabled(rt);
}


FlameExecBlock *flame_rt_new_block(JSRuntime *rt, size_t size)
{
    FlameRT *frt = flame_rt_get(rt);
    FlameExecBlock *blk;
    void *base;
    if (!frt)
        return NULL;
    base = flame_exec_alloc(size);
    if (!base)
        return NULL;
    if (frt->block_count == frt->block_cap) {
        int cap = frt->block_cap ? frt->block_cap * 2 : 4;
        FlameExecBlock *nb = js_realloc_rt(rt, frt->blocks, cap * sizeof(*nb));
        if (!nb) {
            flame_exec_free(base, size);
            return NULL;
        }
        frt->blocks = nb;
        frt->block_cap = cap;
    }
    blk = &frt->blocks[frt->block_count++];
    blk->base = (uint8_t *)base;
    blk->size = size;
    blk->used = 0;
    flame_log(FLAME_LOG_VERBOSE, "exec block #%d: %zu bytes at %p",
              frt->block_count - 1, size, base);
    return blk;
}

void flame_rt_register_func(JSRuntime *rt, JSFunctionBytecode *b,
                            void *code, size_t code_size, uint8_t tier,
                            const uint32_t *osr_bc, void **osr_entry,
                            int osr_count, void **slot)
{
    FlameRT *frt = flame_rt_get(rt);
    FlameCompiledFunc *f;
    int i;
    if (!frt)
        return;
    f = js_mallocz_rt(rt, sizeof(*f));
    if (!f)
        return;
    f->b = b;
    f->code = (uint8_t *)code;
    f->code_size = code_size;
    f->tier = tier;
    f->osr_count = osr_count > 8 ? 8 : osr_count;
    for (i = 0; i < f->osr_count; i++) {
        f->osr[i].bc_off = osr_bc[i];
        f->osr[i].entry = osr_entry[i];
    }
    f->next = frt->funcs;
    frt->funcs = f;
    frt->func_count++;
    *slot = f;
    flame_stat_compiles++;
    flame_stat_native_bytes += code_size;
}


void flame_dump_stats(JSRuntime *rt)
{
    FlameRT *frt = rt ? (FlameRT *)rt->flame_rt : NULL;
    fprintf(stderr,
            "[flame %s] stats: compiled=%llu failed=%llu jit_calls=%llu "
            "deopts=%llu osr=%llu native_bytes=%llu ic_hits=%llu "
            "ic_misses=%llu live_funcs=%d\n",
            FLAME_VERSION,
            (unsigned long long)flame_stat_compiles,
            (unsigned long long)flame_stat_compile_fails,
            (unsigned long long)flame_stat_jit_calls,
            (unsigned long long)flame_stat_deopts,
            (unsigned long long)flame_stat_osr,
            (unsigned long long)flame_stat_native_bytes,
            (unsigned long long)flame_stat_ic_hits,
            (unsigned long long)flame_stat_ic_misses,
            frt ? frt->func_count : 0);
}

void flame_reset_stats(JSRuntime *rt)
{
    (void)rt;
    flame_stat_compiles = 0;
    flame_stat_compile_fails = 0;
    flame_stat_jit_calls = 0;
    flame_stat_deopts = 0;
    flame_stat_osr = 0;
    flame_stat_native_bytes = 0;
    flame_stat_ic_hits = 0;
    flame_stat_ic_misses = 0;
}
