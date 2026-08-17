/* test_hybrid_kv.c — hybrid layout: MAP slots (KV) + contiguous store (weights)
 * ═══════════════════════════════════════════════════════════════════════════
 * ที่มา: docs/BENCH_GEOMETRIC_FS_SPEED-2026-08-17.md (workload 4 — GGUF จริง)
 *   - tiny random-access (KV cache 256B block) → MAP ชนะ CLASSIC 54×
 *   - chunky tensor → contiguous ชนะ/เสมอ
 * → hybrid: วาง KV ผ่าน DtSlotRegion (direct address, O(1), collision-free)
 *           วาง weights ผ่าน DRamTileStore (contiguous + directory)
 *
 * พิสูจน์:
 *   A. slot region: put/get ครบ (layer,pos) → lossless (ทุก block เท่าเดิม)
 *   B. collision-free: KV จริงมี 24×2048=49152 blocks > hash 512 slots
 *      → ถ้าใช้ dt_put_kv แบบ hash จะทับกัน (พิสูจน์ว่าจำเป็นต้องใช้ slot)
 *   C. hybrid: weights ใน store + KV ใน slots อยู่พร้อมกัน อ่านได้ทั้งคู่
 *   D. speed: read KV ผ่าน slot (pointer ตรง) vs CLASSIC (fseek+fread)
 *      → ต้องเห็น MAP เร็วกว่า (หลักฐาน = จริง ไม่ใช่ claim)
 *   E. twin slot region (disk mmap) → reopen → ข้อมูลยังครบ lossless
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -I. -Icore -Icore/infra \
 *        -o build/test_hybrid_kv tests/test_hybrid_kv.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "../core/infra/dramtile_store.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

#if defined(_WIN32)
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

#define N_LAYERS 24
#define N_CTX    2048
#define KV_BLOCK 256          /* Qwen2.5-0.5B: n_kv_head(7) × head_dim(128) × fp16(2)/... = 7×128×2/7 */
#define KV_N     (N_LAYERS * N_CTX)

static uint8_t *blk_data[KV_N];

int main(void) {
    printf("HYBRID LAYOUT — KV (MAP slots) + weights (contiguous)\n");
    printf("  KV: %d layers × %d ctx = %d blocks × %d B\n",
           N_LAYERS, N_CTX, KV_N, KV_BLOCK);

    for (int i = 0; i < KV_N; i++) {
        blk_data[i] = (uint8_t*)malloc(KV_BLOCK);
        uint8_t *p = blk_data[i];
        for (size_t j = 0; j < KV_BLOCK; j++)
            p[j] = (uint8_t)((j * 2654435761u) >> 24) ^ (uint8_t)i;
    }

    /* ── A. slot region: direct address, put/get lossless ── */
    DtSlotRegion kv;
    CHECK("A1 slot region init (49152 slots × 256B)", dt_slot_init(&kv, KV_N, KV_BLOCK) == 0);
    for (int l = 0; l < N_LAYERS; l++)
        for (uint32_t pos = 0; pos < N_CTX; pos++) {
            uint32_t a = dt_slot_kv_addr(l, pos, N_CTX);
            uint8_t *p = dt_slot_put(&kv, a, blk_data[l * N_CTX + pos], KV_BLOCK);
            if (!p) { fail_count++; printf("  T: FAIL — A2 put (l=%d,pos=%u)\n", l, pos); goto done; }
        }
    CHECK("A2 put ครบ 49152 blocks (no NULL)", 1);
    int bad = 0;
    for (int l = 0; l < N_LAYERS && bad == 0; l++)
        for (uint32_t pos = 0; pos < N_CTX && bad == 0; pos++) {
            uint32_t a = dt_slot_kv_addr(l, pos, N_CTX);
            if (memcmp(kv.base + (size_t)a * KV_BLOCK, blk_data[l * N_CTX + pos], KV_BLOCK) != 0)
                bad++;
        }
    CHECK("A3 read ครบ lossless (49152 blocks เท่าเดิม)", bad == 0);

    /* ── B. collision: hash-based dt_put_kv ชนที่ 512 slots ── */
    {
        DRamTileStore store;
        dt_store_init(&store, 4UL << 20);
        store.kv_base   = (uint8_t*)malloc(4UL << 20);
        store.kv_capacity = 4UL << 20;
        int collide = 0, overwritten = 0;
        for (int l = 0; l < N_LAYERS; l++)
            for (uint32_t pos = 0; pos < N_CTX; pos++) {
                char name[64];
                snprintf(name, sizeof(name), "blk.%d.kv.%u", l, pos);
                if (!dt_put_kv(&store, name, blk_data[l * N_CTX + pos], KV_BLOCK))
                    collide++;
            }
        /* check: address ที่ยังอ่านคืนได้ถูกต้อง (ไม่โดนทับ) */
        for (int i = 0; i < 1024; i++) {
            char name[64];
            snprintf(name, sizeof(name), "blk.%d.kv.%u", i % N_LAYERS, i % N_CTX);
            uint8_t *p = dt_get(&store, name);
            if (p && memcmp(p, blk_data[i % N_LAYERS * N_CTX + i % N_CTX], KV_BLOCK) != 0)
                overwritten++;
        }
        free(store.kv_base);
        dt_store_destroy(&store);
        CHECK("B1 hash-based put (512 slots) ต้องมี NULL/ทับ (collision เกิดจริง)", collide > 0 || overwritten > 0);
    }

    /* ── C. hybrid: weights ผ่าน DRamTileStore + KV ผ่าน slots ── */
    {
        DRamTileStore store;
        dt_store_init(&store, 8UL << 20);
        uint8_t w[4096];
        for (size_t j = 0; j < sizeof(w); j++) w[j] = (uint8_t)(j * 7);
        CHECK("C1 weight put (contiguous path)", dt_put(&store, "blk.0.attn_w", w, sizeof(w)) != NULL);
        uint8_t *r = dt_get(&store, "blk.0.attn_w");
        CHECK("C2 weight get (contiguous path)", r && memcmp(r, w, sizeof(w)) == 0);
        uint32_t a = dt_slot_kv_addr(3, 777, N_CTX);
        uint8_t *p = dt_slot_ptr(&kv, a);
        CHECK("C3 KV slot อ่านพร้อมกัน (l=3,pos=777)", p && memcmp(p, blk_data[3 * N_CTX + 777], KV_BLOCK) == 0);
        dt_store_destroy(&store);
    }

    /* ── D. speed: slot pointer vs CLASSIC fseek+fread ── */
    {
        FILE *f = fopen("build/hybrid_kv_cls.bin", "wb");
        for (int l = 0; l < N_LAYERS; l++)
            for (uint32_t pos = 0; pos < N_CTX; pos++)
                fwrite(blk_data[l * N_CTX + pos], 1, KV_BLOCK, f);
        fclose(f);
        f = fopen("build/hybrid_kv_cls.bin", "rb");
        uint8_t *scratch = (uint8_t*)malloc(KV_BLOCK);
        double t0 = now_ns();
        for (int l = 0; l < N_LAYERS; l++)
            for (uint32_t pos = 0; pos < N_CTX; pos++) {
                uint32_t a = dt_slot_kv_addr(l, pos, N_CTX);
                uint8_t *p = kv.base + (size_t)a * KV_BLOCK;
                if (memcmp(p, blk_data[l * N_CTX + pos], KV_BLOCK) != 0) bad++;
            }
        double t_map = now_ns() - t0;
        t0 = now_ns();
        for (int l = 0; l < N_LAYERS; l++)
            for (uint32_t pos = 0; pos < N_CTX; pos++) {
                long off = (long)(((size_t)l * N_CTX + pos) * KV_BLOCK);
                fseek(f, off, SEEK_SET);
                fread(scratch, 1, KV_BLOCK, f);
                if (memcmp(scratch, blk_data[l * N_CTX + pos], KV_BLOCK) != 0) bad++;
            }
        double t_cls = now_ns() - t0;
        fclose(f);
        free(scratch);
        printf("  D speed: MAP %.2f GB/s (%6.0f ns/blk) vs CLASSIC %.2f GB/s (%6.0f ns/blk) → %.1fx\n",
               (double)(KV_N * KV_BLOCK) / 1e9 / (t_map / 1e9), t_map / KV_N,
               (double)(KV_N * KV_BLOCK) / 1e9 / (t_cls / 1e9), t_cls / KV_N,
               t_cls / t_map);
        CHECK("D1 MAP read lossless", bad == 0);
        CHECK("D2 MAP เร็วกว่า CLASSIC (speed หลักฐานจริง)", t_map < t_cls);
    }

    /* ── E. twin slot region → reopen → lossless ── */
    {
        DtSlotRegion kv2;
        CHECK("E1 twin slot init (disk mmap)", dt_slot_init_twin(&kv2, "build/hybrid_kv_slots.bin", KV_N, KV_BLOCK) == 0);
        for (int l = 0; l < N_LAYERS; l++)
            for (uint32_t pos = 0; pos < N_CTX; pos++)
                dt_slot_put(&kv2, dt_slot_kv_addr(l, pos, N_CTX), blk_data[l * N_CTX + pos], KV_BLOCK);
        dt_slot_destroy(&kv2);
        CHECK("E2 twin destroy (flush)", 1);
        DtSlotRegion kv3;
        CHECK("E3 twin reopen (reload จาก disk)", dt_slot_init_twin(&kv3, "build/hybrid_kv_slots.bin", KV_N, KV_BLOCK) == 0);
        int bad2 = 0;
        for (int l = 0; l < N_LAYERS && bad2 == 0; l++)
            for (uint32_t pos = 0; pos < N_CTX && bad2 == 0; pos++) {
                uint32_t a = dt_slot_kv_addr(l, pos, N_CTX);
                if (memcmp(kv3.base + (size_t)a * KV_BLOCK, blk_data[l * N_CTX + pos], KV_BLOCK) != 0)
                    bad2++;
            }
        CHECK("E4 reload lossless (disk→mmap→read เท่าเดิม)", bad2 == 0);
        dt_slot_destroy(&kv3);
    }

done:
    dt_slot_destroy(&kv);
    for (int i = 0; i < KV_N; i++) free(blk_data[i]);
    printf("\nRESULT: %d PASS, %d FAIL\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}