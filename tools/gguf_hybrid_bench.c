/* gguf_hybrid_bench.c — hybrid (slot+contiguous) vs contiguous-only
 * บน tensor distribution จริงจาก GGUF (Qwen3-4B: bimodal)
 *
 * SCOPE ที่ honest: เทนเซอร์ใหญ่ (>=64K) ใช้ path contiguous เหมือนกัน
 * ทั้งสอง layout → ตัดออก (cancels out) เหลือ benchmark เฉพาะ 145 tiny
 * tensors (<64K, 784KB) ที่เป็นตัวต่างจริง: slot direct-addr vs dense array
 *
 * วัด: write / read seq / read rand / tiny rand, lossless verify, overhead%
 * gcc -O2 -Wall -Icore -o build/gguf_hybrid_bench tools/gguf_hybrid_bench.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "../core/gguf_reader.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ns(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
}
#else
#include <time.h>
static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
#endif

#define SPLIT (64u * 1024u)   /* < 64K → slot, else → contiguous (ตัดออก) */

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <model.gguf>\n", argv[0]); return 1; }
    GgufReader r;
    if (gguf_open(argv[1], &r) != 0) { printf("cannot open gguf\n"); return 1; }

    uint32_t N = r.n_tensors;
    uint64_t total = 0;
    for (uint32_t i = 0; i < N; i++) total += r.sizes[i];

    /* ── classify: เฉพาะ split tensors เท่านั้น ── */
    uint32_t n_slot = 0;
    uint64_t tiny_bytes = 0, slot_window = 0;
    uint32_t *tiny_idx = (uint32_t*)malloc(sizeof(uint32_t) * N);
    uint32_t *tiny_cls = (uint32_t*)malloc(sizeof(uint32_t) * N);
    static const uint32_t classes[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
    static const uint32_t n_classes = 8;
    uint32_t class_count[8] = {0};

    for (uint32_t i = 0; i < N; i++) {
        uint64_t sz = r.sizes[i];
        if (sz < SPLIT) {
            uint32_t c = 0;
            for (uint32_t k = 0; k < n_classes; k++) { if (sz <= classes[k]) { c = k; break; } }
            tiny_cls[n_slot] = c;
            tiny_idx[n_slot] = i;
            class_count[c]++;
            n_slot++;
            tiny_bytes += sz;
            slot_window += classes[c];
        }
    }

    printf("GGUF %s: %u tensors, %llu B total\n", argv[1], N, (unsigned long long)total);
    printf("  SCOPE: เฉพาะ %u tiny tensors (<64K, %llu B) — ใหญ่ตัดออก (contiguous เท่ากันทั้งคู่)\n",
           n_slot, (unsigned long long)tiny_bytes);
    printf("  slot window = %llu B → overhead %.1f%%\n",
           (unsigned long long)slot_window,
           (double)(slot_window - tiny_bytes) / tiny_bytes * 100.0);
    fflush(stdout);

    /* ── layout maps ── */
    uint64_t class_base[8] = {0};
    uint64_t cb = 0;
    for (uint32_t c = 0; c < n_classes; c++) { class_base[c] = cb; cb += (uint64_t)class_count[c] * classes[c]; }
    uint64_t slot_pos[8] = {0};
    uint64_t *slot_off = (uint64_t*)malloc(sizeof(uint64_t) * n_slot);
    uint64_t *all_off = (uint64_t*)malloc(sizeof(uint64_t) * n_slot);
    uint64_t acc = 0;
    for (uint32_t i = 0; i < n_slot; i++) {
        uint32_t c = tiny_cls[i];
        slot_off[i] = class_base[c] + slot_pos[c];
        slot_pos[c] += classes[c];
        all_off[i] = acc;
        acc += (uint64_t)r.sizes[tiny_idx[i]];
    }

    /* ── cache tiny source ครั้งเดียว (แค่ ~784KB พอดี RAM) ── */
    uint8_t *src = (uint8_t*)malloc(tiny_bytes ? tiny_bytes : 1);
    uint32_t *sz = (uint32_t*)malloc(sizeof(uint32_t) * n_slot);
    uint64_t src_off = 0;
    for (uint32_t i = 0; i < n_slot; i++) {
        sz[i] = r.sizes[tiny_idx[i]];
        memcpy(src + src_off, r.base + r.data_offset + r.offsets[tiny_idx[i]], sz[i]);
        src_off += sz[i];
    }
    gguf_close(&r);   /* free mmap 2.5GB ไปก่อน — ไม่ต้องใช้แล้ว */

    /* ── layouts ── */
    uint8_t *slot_base = (uint8_t*)malloc(slot_window ? slot_window : 1);
    uint8_t *all_base  = (uint8_t*)malloc(tiny_bytes ? tiny_bytes : 1);

    double t0 = now_ns();
    for (uint32_t i = 0; i < n_slot; i++) {
        memcpy(slot_base + slot_off[i], src + all_off[i], (size_t)sz[i]);
    }
    double t_hyb_write = now_ns() - t0;

    t0 = now_ns();
    for (uint32_t i = 0; i < n_slot; i++)
        memcpy(all_base + all_off[i], src + all_off[i], (size_t)sz[i]);
    double t_cont_write = now_ns() - t0;

    printf("\n  write: hybrid %.3f GB/s  vs  contiguous %.3f GB/s\n",
           (double)tiny_bytes / 1e9 / (t_hyb_write / 1e9),
           (double)tiny_bytes / 1e9 / (t_cont_write / 1e9));
    fflush(stdout);

    /* ── read seq ── */
    int bad = 0;
    t0 = now_ns();
    for (uint32_t i = 0; i < n_slot; i++)
        if (memcmp(slot_base + slot_off[i], src + all_off[i], (size_t)sz[i]) != 0) bad++;
    double t_hyb_seq = now_ns() - t0;
    CHECK("HYBRID read seq lossless", bad == 0);

    bad = 0;
    t0 = now_ns();
    for (uint32_t i = 0; i < n_slot; i++)
        if (memcmp(all_base + all_off[i], src + all_off[i], (size_t)sz[i]) != 0) bad++;
    double t_cont_seq = now_ns() - t0;
    CHECK("CONTIG  read seq lossless", bad == 0);

    /* ── read rand ── */
    uint32_t *perm = (uint32_t*)malloc(sizeof(uint32_t) * n_slot);
    for (uint32_t i = 0; i < n_slot; i++) perm[i] = i;
    uint32_t s = 12345u;
    for (uint32_t i = n_slot - 1; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        uint32_t j = (s >> 8) % (i + 1);
        uint32_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }

    bad = 0;
    t0 = now_ns();
    for (uint32_t k = 0; k < n_slot; k++) {
        uint32_t i = perm[k];
        if (memcmp(slot_base + slot_off[i], src + all_off[i], (size_t)sz[i]) != 0) bad++;
    }
    double t_hyb_rand = now_ns() - t0;
    CHECK("HYBRID read rand lossless", bad == 0);

    bad = 0;
    t0 = now_ns();
    for (uint32_t k = 0; k < n_slot; k++) {
        uint32_t i = perm[k];
        if (memcmp(all_base + all_off[i], src + all_off[i], (size_t)sz[i]) != 0) bad++;
    }
    double t_cont_rand = now_ns() - t0;
    CHECK("CONTIG  read rand lossless", bad == 0);

    printf("\n  read seq : hybrid %.3f GB/s  vs  contiguous %.3f GB/s\n",
           (double)tiny_bytes / 1e9 / (t_hyb_seq / 1e9),
           (double)tiny_bytes / 1e9 / (t_cont_seq / 1e9));
    printf("  read rand: hybrid %.3f GB/s  vs  contiguous %.3f GB/s\n",
           (double)tiny_bytes / 1e9 / (t_hyb_rand / 1e9),
           (double)tiny_bytes / 1e9 / (t_cont_rand / 1e9));
    printf("  overhead: slot window %llu B = %.2fx ของ tiny logical %llu B\n",
           (unsigned long long)slot_window,
           (double)slot_window / (double)tiny_bytes,
           (unsigned long long)tiny_bytes);

    free(tiny_idx); free(tiny_cls); free(slot_off); free(all_off);
    free(perm); free(slot_base); free(all_base); free(src);
    printf("\nRESULT: %d PASS, %d FAIL\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}