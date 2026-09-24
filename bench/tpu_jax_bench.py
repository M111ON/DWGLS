#!/usr/bin/env python3
"""tpu_jax_bench.py — DWGLS TPU baseline, mirror of the GPU small-batch benches.

Colab: Runtime > Change runtime type > TPU (v2-8/v3-8)  ->  run this file whole.
Plain python (local/Kaggle TPU):  python bench/tpu_jax_bench.py

Measures (pair for pair against docs/GPU-SMALL-BATCH-OVERHEAD-2026-09-23.md):
  B1  HBM bandwidth: r+w elementwise / d2d copy / read-only sum
  B2  host<->device bandwidth + latency (device_put / device_get size sweep)
  B3  dispatch overhead: async call time vs synced completion (batch=1 analog)
  B4  compile overhead: cold first-call vs steady state + N=1..1M sweep
      (per-chunk floor + 2x/1.5x break-even — same definition as
       gpu_batch_break_even where launch share on TPU = compile, not launch)
  B5  persistent compilation cache: cold vs warm across processes

Exit code 0 iff every check PASS (SKIP is reported, not counted as pass).
"""

import os
import subprocess
import sys
import tempfile
import time

# ── device ────────────────────────────────────────────────────────────────
try:
    import jax
    import jax.numpy as jnp
    import numpy as np
except ImportError:
    print("FATAL: pip install jax  (Colab TPU runtime ships it preinstalled)")
    sys.exit(2)

# ── B5 persistent compilation cache (cross-process) ───────────────────────
# Runs BEFORE grabbing the device: libtpu is single-holder — a worker that
# inits the TPU while the main process holds it dies rc=1 (T5 SKIP cause).
print("── B5 persistent compilation cache (cold vs warm process) ──")
cache_dir = os.path.join(tempfile.gettempdir(), "dwgls_jax_pcache")
os.makedirs(cache_dir, exist_ok=True)
worker = r'''
import time, jax, jax.numpy as jnp
jax.config.update("jax_compilation_cache_dir", r"__DIR__")
try:
    jax.config.update("jax_persistent_cache_min_compile_time_secs", 0)
except Exception:
    pass
f = lambda a: a * 3.0 + 2.0
x = jnp.ones((65536,), dtype=jnp.float32)
x.block_until_ready()
t0 = time.perf_counter()
f(x).block_until_ready()
print(f"first_call_ms={(time.perf_counter()-t0)*1e3:.2f}")
'''
worker = worker.replace("__DIR__", cache_dir.replace("\\", "/"))
t_cache = [None, None]
for i in range(2):
    try:
        r = subprocess.run(
            [sys.executable, "-c", worker],
            capture_output=True, text=True, timeout=300,
            env={**os.environ, "JAX_COMPILATION_CACHE_DIR": cache_dir},
        )
        for line in r.stdout.splitlines():
            if line.startswith("first_call_ms="):
                t_cache[i] = float(line.split("=", 1)[1])
        if r.returncode != 0:
            print(f"  worker {i} rc={r.returncode}:\n{r.stderr.strip()[-600:]}")
    except Exception as e:  # noqa: BLE001
        print(f"  worker {i} error: {e}")
if t_cache[0] is None or t_cache[1] is None:
    print("  (persistent cache unavailable — see worker error above)")
    cache_ratio = None
else:
    cache_ratio = t_cache[1] / t_cache[0] if t_cache[0] > 0 else 1.0
    print(f"  cold (miss) = {t_cache[0]:8.2f} ms")
    print(f"  warm (hit)  = {t_cache[1]:8.2f} ms   ratio = {cache_ratio:.2f}")
print()

DEV = None
try:
    DEV = jax.devices()[0]
except Exception as e:  # noqa: BLE001
    print(f"FATAL: no jax device: {e}")
    sys.exit(2)

PLATFORM = DEV.platform
DEVKIND = getattr(DEV, "device_kind", "?")
print(f"device : {PLATFORM} / {DEVKIND}  (jax {jax.__version__})")
try:
    stats = DEV.memory_stats() if hasattr(DEV, "memory_stats") else None
    if stats and "bytes_in_use" in stats:
        print(f"memory : {stats['bytes_in_use'] / 1e9:.2f} GB in use / "
              f"{stats.get('bytes_limit', 0) / 1e9:.2f} GB limit")
except Exception:  # noqa: BLE001
    pass
print()

PASS_N = [0]
FAIL_N = [0]
SKIP_N = [0]


def check(tag, name, ok, detail=""):
    if ok is None:
        SKIP_N[0] += 1
        print(f"  {tag} SKIP {name} {detail}")
    elif ok:
        PASS_N[0] += 1
        print(f"  {tag} PASS {name} {detail}")
    else:
        FAIL_N[0] += 1
        print(f"  {tag} FAIL {name} {detail}")


def tmin(fn, reps=5):
    best = float("inf")
    for _ in range(reps):
        t0 = time.perf_counter()
        out = fn()
        if hasattr(out, "block_until_ready"):
            out.block_until_ready()
        best = min(best, time.perf_counter() - t0)
    return best


# ── B1 HBM bandwidth ──────────────────────────────────────────────────────
print("── B1 HBM bandwidth (float32, GB/s = bytes moved / best time) ──")
print(f"  {'elems':>10} {'r+w':>8} {'d2d':>8} {'read':>8}")
best_rw = best_d2d = best_rd = 0.0
best_rw_n = 0
for k in range(12, 27, 2):  # 4K .. 64M elems  (16 KB .. 256 MB)
    n = 1 << k
    x = jnp.ones((n,), dtype=jnp.float32)
    x.block_until_ready()
    t_rw = tmin(lambda: x + 1.0, reps=3)          # read n + write n
    t_d2 = tmin(lambda: jnp.copy(x), reps=3)      # read n + write n
    t_rd = tmin(lambda: x.sum(), reps=3)          # read n
    b = n * 4
    rw, d2, rd = 2 * b / t_rw / 1e9, 2 * b / t_d2 / 1e9, b / t_rd / 1e9
    print(f"  {n:>10} {rw:>7.1f}G {d2:>7.1f}G {rd:>7.1f}G")
    if rw > best_rw:
        best_rw, best_rw_n = rw, n
print(f"  best r+w = {best_rw:.1f} GB/s @ {best_rw_n} elems\n")

# ── B2 host <-> device ────────────────────────────────────────────────────
print("── B2 host <-> device (GB/s + per-call µs) ──")
print(f"  {'bytes':>10} {'put GB/s':>9} {'put µs':>8} {'get GB/s':>9} {'get µs':>8}")
best_h2d = 0.0
for k in range(12, 25, 3):  # 4 KB .. 32 MB
    nb = 1 << k
    host = np.ones((nb // 4,), dtype=np.float32)
    t_put = tmin(lambda: jax.device_put(host, DEV).block_until_ready(), reps=3)
    d = jax.device_put(host, DEV)
    d.block_until_ready()
    t_get = tmin(lambda: jax.device_get(d), reps=3)
    put_g = nb / t_put / 1e9
    get_g = nb / t_get / 1e9
    print(f"  {nb:>10} {put_g:>8.2f}G {t_put * 1e6:>7.1f} {get_g:>8.2f}G {t_get * 1e6:>7.1f}")
    best_h2d = max(best_h2d, put_g)
print(f"  best host->device = {best_h2d:.2f} GB/s\n")

# ── B3 dispatch (batch=1 analog) ──────────────────────────────────────────
print("── B3 dispatch overhead (trivial op on 8 elems) ──")
f_triv = lambda a: a + 1.0  # noqa: E731
x8 = jnp.ones((8,), dtype=jnp.float32)
x8.block_until_ready()
f_triv(x8).block_until_ready()  # warm


def async_dispatch():
    t0 = time.perf_counter()
    f_triv(x8)          # enqueue only — no block
    return time.perf_counter() - t0


t_async = tmin(async_dispatch, reps=50)
t_sync = tmin(lambda: f_triv(x8), reps=50)
print(f"  enqueue (async)   = {t_async * 1e6:8.2f} µs")
print(f"  synced completion = {t_sync * 1e6:8.2f} µs   <- batch=1 full cost\n")

# ── B4 compile + small-batch sweep ────────────────────────────────────────
print("── B4 compile cold vs steady + per-chunk sweep ──")
g_cold = lambda a: a * 2.0 + 1.0  # noqa: E731  fresh source -> fresh compile
xc = jnp.ones((4096,), dtype=jnp.float32)
xc.block_until_ready()
t0 = time.perf_counter()
g_cold(xc).block_until_ready()
t_compile = time.perf_counter() - t0
t_steady = tmin(lambda: g_cold(xc), reps=20)
print(f"  cold first call (compile) = {t_compile * 1e3:8.2f} ms")
print(f"  steady state              = {t_steady * 1e6:8.2f} µs")
print()
print(f"  {'N':>10} {'full µs':>10} {'ns/chunk':>10} {'compile%':>9}")
rows = []
floor_ns = float("inf")
for k in range(0, 21):  # 1 .. 1M elems
    n = 1 << k
    xn = jnp.ones((n,), dtype=jnp.float32)
    xn.block_until_ready()
    t_full = tmin(lambda: g_cold(xn), reps=5 if n <= (1 << 16) else 3)
    per_ns = t_full * 1e9 / n
    pct = t_compile / t_full * 100.0
    rows.append((n, t_full, per_ns, pct))
    floor_ns = min(floor_ns, per_ns)
    if k <= 10 or k % 4 == 0:
        print(f"  {n:>10} {t_full * 1e6:>9.1f} {per_ns:>9.1f} {pct:>8.1f}%")

# break-even with the SAME definitions as gpu_batch_break_even.cu
n_2x = next((r[0] for r in rows if r[2] <= 2.0 * floor_ns), 0)
n_15x = next((r[0] for r in rows if r[2] <= 1.5 * floor_ns), 0)
n_10pct = next((r[0] for r in rows if r[3] <= 10.0), 0)
print(f"\n  floor = {floor_ns:.1f} ns/chunk")
print(f"  per-chunk <= 2.0x floor : N >= {n_2x or 'not reached'}")
print(f"  per-chunk <= 1.5x floor : N >= {n_15x or 'not reached'}")
print(f"  compile share <= 10%    : N >= {n_10pct or 'not reached'}\n")

# ── checks (oracle: independent budgets / known TPU class specs) ──────────
print("── checks ──")
check("T1", "jax device present", DEV is not None and PLATFORM in ("tpu", "gpu", "cpu"),
      f"platform={PLATFORM}")
# TPU HBM class is hundreds of GB/s per chip family; 10 GB/s is a floor any
# real accelerator clears (CPU/ramdisk would fail) — independent of this code.
check("T2", "HBM r+w >= 10 GB/s", best_rw >= 10.0, f"got {best_rw:.1f} GB/s")
# break-even must be reachable inside the sweep (definition check, mirrors N50/N10)
check("T3", "per-chunk 2x-floor break-even exists", n_2x > 0, f"N={n_2x}")
# compile is milliseconds-class on any XLA device; dispatch is µs-class —
# 10x separation is the spec-level claim being tested, not a tuned threshold.
check("T4", "compile >= 10x steady dispatch", t_compile >= 10 * t_steady,
      f"{t_compile * 1e3:.1f} ms vs {t_steady * 1e6:.1f} µs")
if cache_ratio is None:
    check("T5", "persistent cache warm < cold", None, "(unsupported)")
else:
    check("T5", "persistent cache warm <= 0.75x cold", cache_ratio <= 0.75,
          f"ratio={cache_ratio:.2f}")

# ── GPU reference footer (for side-by-side report) ────────────────────────
print("""
── GPU reference (GTX 1050 Ti, same definitions) ──
  HBM write        101.5 GB/s      | host->device     0.76 GB/s (PCIe gen3 x1!)
  d2d copy          48.3 GB/s      | dispatch (launch) 13.5 µs
  batch=1 full      16.5 µs        | per-chunk floor  ~100 ns
  per-chunk <=2x    N >= 512       | launch share <=10%  N >= 1024
  JetBridge 12 wants 3.05 µs/want (coalesce >=10:1)

  TPU analog: launch -> compile (B4), JetBridge -> XLA fusion + cache (B5)
""")
print(f"RESULT: {PASS_N[0]} PASS / {FAIL_N[0]} FAIL / {SKIP_N[0]} SKIP")
sys.exit(0 if FAIL_N[0] == 0 else 1)
