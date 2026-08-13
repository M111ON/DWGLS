/* ═══════════════════════════════════════════════════════════════════════════
 * bench_mdim_timeline.c — cost of the hardened timeline API
 * ═══════════════════════════════════════════════════════════════════════════
 * state_at(F) = 1.3 MB base copy, then walk the journal newest→oldest,
 * CRC-verifying EVERY retained frame (per-frame CRC over up to ~8 KB of
 * ring), undoing changes newer than F. read_at(F) = malloc + state_at +
 * name probe + chain read.
 *
 * Two ring configurations fill the 128-slot journal to find the
 * worst-case per-version cost across a full ring:
 *   A — many small frames (1-byte files) → max retained frame COUNT
 *   B — few large frames  (100 KB file)  → max per-frame CRC span
 * The worst case is the deepest valid versioned read on the fullest ring.
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geofs_mdim.h"

#if defined(_WIN32)
  #include <windows.h>
  static double now_us(void) {
      LARGE_INTEGER f, c;
      QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&c);
      return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
  }
#else
  #include <time.h>
  static double now_us(void) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
  }
#endif

#define BENCH_ITERS 3000

/* cost of the full-undo walk (deepest retained version) */
static void chain_cost(MdimVolume *v, uint32_t *frames, uint32_t *crc_bytes,
                       uint32_t *undo_bytes) {
    *frames = 0; *crc_bytes = 0; *undo_bytes = 0;
    uint32_t f = v->journal_head;
    uint32_t prev_fn = MDIM_FRAME_NONE;
    while (f != MDIM_FRAME_NONE) {
        uint8_t *hdr = mdim_frame_hdr(v, f);
        if (hdr[MDIM_FH_MAGIC] != MDIM_JMAGIC) break;
        uint32_t n = mdim_frame_n_changes(hdr);
        uint32_t fn = mdim_u32(hdr, MDIM_FH_FRAME_NO);
        if (prev_fn != MDIM_FRAME_NONE && fn >= prev_fn) break;
        prev_fn = fn;
        (*frames)++;
        *crc_bytes += mdim_frame_span(n) * MDIM_SLOT_SZ;
        *undo_bytes += n * MDIM_SLOT_SZ;
        f = mdim_u32(hdr, MDIM_FH_PREV);
    }
}

static double bench_state_at(MdimVolume *v, uint32_t target, uint8_t *out,
                             double *out_min) {
    double best = 1e18, sum = 0;
    for (int i = 0; i < BENCH_ITERS; i++) {
        double t0 = now_us();
        if (mdim_state_at(v, target, out) != MDIM_OK) { printf("state_at err\n"); return -1; }
        double dt = now_us() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    *out_min = best;
    return sum / BENCH_ITERS;
}

static double bench_read_at(MdimVolume *v, const char *name, uint32_t target,
                            double *out_min) {
    uint8_t buf[64];
    uint32_t actual = 0;
    double best = 1e18, sum = 0;
    for (int i = 0; i < BENCH_ITERS; i++) {
        double t0 = now_us();
        if (mdim_read_at(v, name, target, buf, sizeof(buf), &actual) != MDIM_OK) {
            printf("read_at err\n"); return -1;
        }
        double dt = now_us() - t0;
        if (dt < best) best = dt;
        sum += dt;
    }
    *out_min = best;
    return sum / BENCH_ITERS;
}

static void report(const char *cfg, MdimVolume *v, const char *probe) {
    uint32_t frames = 0, crc_bytes = 0, undo_bytes = 0;
    chain_cost(v, &frames, &crc_bytes, &undo_bytes);
    uint32_t ckpt = v->checkpoint_frame, last = mdim_last_frame(v);

    uint8_t *snap = (uint8_t *)malloc(MDIM_VOL_BYTES);
    if (!snap) { printf("malloc fail\n"); return; }
    memset(snap, 0, MDIM_VOL_BYTES);

    double mn;
    double newest = bench_state_at(v, last, snap, &mn);      /* no undo — floor */
    double newest_min = mn;
    double mid    = bench_state_at(v, ckpt + 1 + (last - ckpt) / 2, snap, &mn);
    double mid_min = mn;
    double deep   = bench_state_at(v, ckpt + 1, snap, &mn);  /* undo all — worst */
    double deep_min = mn;
    double read_d = bench_read_at(v, probe, ckpt + 1, &mn);
    double read_min = mn;

    printf("┌─ %s\n", cfg);
    printf("│  ring: %u retained frames · checkpoint %u → last %u ·\n", frames, ckpt, last);
    printf("│  full-undo walk: %u frames CRC'd (%u B) + %u B undo memcpy\n",
           frames, crc_bytes, undo_bytes);
    printf("│  state_at @ newest   %7.1f µs (min %6.1f)   base copy + head-frame CRC\n",
           newest, newest_min);
    printf("│  state_at @ mid      %7.1f µs (min %6.1f)\n", mid, mid_min);
    printf("│  state_at @ oldest   %7.1f µs (min %6.1f)   ← worst-case per-version\n",
           deep, deep_min);
    printf("│  read_at  @ oldest   %7.1f µs (min %6.1f)   malloc + state_at + probe\n",
           read_d, read_min);
    printf("│  CRC+undo overhead   %7.1f µs   (deepest − newest)\n", deep - newest);
    printf("└──────────────────────────────────────────────────────────\n");
    free(snap);
}

int main(void) {
    printf("GeoFS MDIM — timeline hardening benchmark\n");
    printf("state_at = %.1f MB base copy + per-frame CRC walk; %d iters, avg µs/call\n\n",
           MDIM_VOL_BYTES / 1048576.0, BENCH_ITERS);

    /* config A: many small frames — max retained frame count. Pack the
     * ring to capacity: keep committing 7-slot frames while one more
     * would still fit (stop just before the wrap compaction). */
    {
        MdimVolume v;
        mdim_volume_init(&v, NULL);
        for (int i = 0; i < 64; i++) {
            char name[24];
            snprintf(name, sizeof(name), "s%02d.bin", i);
            uint8_t one = (uint8_t)(i + 1);
            int err = MDIM_OK;
            mdim_summon(&v, name, &one, 1, &err);
        }
        int extra = 0;
        while (v.jrnl_cursor + 7u <= MDIM_JRNL_END) {   /* one more 7-slot frame fits */
            char name[24];
            snprintf(name, sizeof(name), "x%03d.bin", extra);
            uint8_t one = (uint8_t)(extra + 1);
            int err = MDIM_OK;
            mdim_summon(&v, name, &one, 1, &err);
            extra++;
        }
        uint8_t buf[8]; uint32_t actual = 0;
        uint32_t ck = v.checkpoint_frame + 1;
        if (mdim_read_at(&v, "s00.bin", ck, buf, sizeof(buf), &actual) != MDIM_OK ||
            actual != 1 || buf[0] != 1) {
            printf("config A correctness FAILED\n"); return 1;
        }
        report("A — small frames packed to ring capacity (1-byte files)", &v, "s00.bin");
        mdim_volume_free(&v);
    }

    /* config B: one large file — max per-frame CRC span */
    {
        MdimVolume v;
        mdim_volume_init(&v, NULL);
        uint32_t sz = 100 * 1024;
        uint8_t *big = (uint8_t *)malloc(sz);
        for (uint32_t i = 0; i < sz; i++) big[i] = (uint8_t)(i * 13 + 5);
        int err = MDIM_OK;
        mdim_summon(&v, "big.bin", big, sz, &err);
        uint8_t buf[8]; uint32_t actual = 0;
        uint32_t last = mdim_last_frame(&v);
        if (last == MDIM_FRAME_NONE ||
            mdim_read_at(&v, "big.bin", last, buf, sizeof(buf), &actual) != MDIM_OK) {
            printf("config B correctness FAILED\n"); free(big); return 1;
        }
        report("B — 1 large file (100 KB, 123-slot frames)", &v, "big.bin");
        free(big);
        mdim_volume_free(&v);
    }
    return 0;
}
