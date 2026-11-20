# Flame — the Square JIT engine

Flame is Square's JavaScript JIT. It tiers QuickJS bytecode from the
profiling interpreter up to native code, with the interpreter always
remaining the semantic fallback: a compile failure or guard failure
deoptimizes back into QuickJS with identical results.

## Tiers

| Tier | What runs | How it gets there |
|------|-----------|-------------------|
| 0 | QuickJS interpreter (unmodified semantics) | default |
| 1 | Profiled interpreter | 100 entries on one bytecode → `FlameProfile` side table attached: call-site frequencies, argument/callee kinds, arithmetic operand-kind feedback (one nibble pair per bytecode offset), loop back-edge counters, inline-cache sites |
| 2 | **Baseline JIT** (x86_64) | 200 entries or a 200-back-edge hot loop → whole-function compile; per-bytecode emission, helper calls into QuickJS's own slow paths, inline int fast paths; loop-header **OSR stubs** transfer a live interpreter activation mid-loop |
| 3 | **Optimizing JIT** (experimental, `FLAME_TIER3=1`) | 5000 entries or a 50k-back-edge loop → feedback-specialized code: speculative int/double arithmetic, int equality/relational, dense-array element loads; any guard mismatch **deoptimizes** — the live frame is handed to the interpreter and execution resumes at the failed opcode. 3 deopts permanently revert to tier 2 |

## Architecture

Flame is compiled **inside the QuickJS translation unit** (`quickjs.c`
includes `flame_all.h` at the bottom). The tiers need byte-exact layouts of
`JSValue`, `JSObject`, `JSFunctionBytecode` and the interpreter frame, plus
direct calls to internal helpers (`js_add_slow`, `JS_GetProperty`,
`JS_CallInternal`, ...). One translation unit guarantees all of that with
zero duplication and zero ABI drift.

- `flame_bridge.h` — included near the top of `quickjs.c`; declares the
  interpreter hook points.
- `flame_debug.c` — `FLAME_DEBUG` parsing, logging, public enable/stat API.
- `flame_profiler.c` — tier-1 side tables (`FlameProfile`), tiering triggers.
- `flame_ic.c` — inline caches: shape-validated field ICs (epoch-checked,
  no references held), dense-array element ICs with element-kind feedback.
- `flame_rt.c` — per-runtime state, executable memory
  (`VirtualAlloc`/`mmap`), code cache, global stats.
- `flame_emitter.h` + `emitter_x64.c` / `emitter_arm64.c` — the small
  virtual ISA the tiers emit against (labels, moves, tag tests, int/SSE
  math, helper calls). x86_64 implements SysV + Windows ABIs; aarch64 is a
  stub that fails compilation cleanly (interpreter fallback).
- `flame_baseline.c` — the baseline + optimizing compilers, the JIT frame,
  helper implementations, OSR transfer, deopt resume.

## JIT frame

`[arg_count args | var_count vars | stack]` of 16-byte `JSValue` slots —
mirrors the interpreter frame exactly, so OSR can alias the interpreter's
live frame (`st->sfp`) and deopt can transfer it to a heap-resumed
interpreter frame. The value-stack pointer lives in a callee-saved register;
every helper call publishes/reloads it through `FlameJitState`.

Helpers are always `void (*)(FlameJitState *)` — one pointer argument — so
call sites are identical under both x64 ABIs. Per-site constants travel
through `st->param` / `st->param2`.

## Debugging (the legendary part)

| Env var | Effect |
|---------|--------|
| `FLAME_DEBUG=1` | tiering events, compiles, deopts, reverts |
| `FLAME_DEBUG=2` | + verbose codegen diagnostics |
| `FLAME_DEBUG=trace` | + per-opcode emission trace |
| `FLAME_DEBUG=stats` | + stats dump at exit (also `FLAME_DEBUG=1,stats`) |
| `FLAME_DUMP=<dir>` | write each compiled function's native code to `<dir>_<fn>.bin` (disassemble with `objdump -D -b binary -m i386:x86-64`) |
| `FLAME_NO_HOOKS=1` | disable all tier-1 instrumentation |
| `FLAME_NO_IC=1` | disable inline caches |
| `FLAME_NO_COMPILE=1` | disable all compilation |
| `FLAME_TIER3=1` | enable the experimental optimizing tier |
| `SQRL_NO_JIT=1` | kill-switch: everything runs interpreted |
| `SQRL_GC_THRESHOLD=<bytes>` | override the engine GC threshold |

`flame_dump_stats(rt)` / `flame_set_enabled(rt, 0)` are also callable from C.

Stats counters: compiles, compile failures, jit_calls, deopts, osr,
native_bytes, ic_hits, ic_misses.

## Known issues / limitations

- Functions containing property stores (`put_field` family) are
  containment-gated out of JIT compilation and OSR until the exotic-setter
  teardown interaction is root-caused; those functions run interpreted.
  Pure-JS hot code (arithmetic, loops, closures, string building, calls)
  is fully JIT'd and verified.
- A bounded number of DOM-wrapper objects can remain reachable at runtime
  teardown on DOM-heavy pages (reported via `[flame] teardown:` at exit,
  never fatal, results unaffected).
- Tier 3 is opt-in (`FLAME_TIER3=1`): the deopt-resume handoff has a heap
  corruption issue under some workloads.
- ARM64 emitter is a stub — ARM targets run everything interpreted.
- Functions using try/catch, generators/async, `with`, `for-in/of` protocol
  ops, eval, class fields and similar are refused at compile time
  (`FLAME_DEBUG=2` prints the offending opcode) and run interpreted.
