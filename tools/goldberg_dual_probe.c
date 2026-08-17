/*
 * goldberg_dual_probe.c — GGUF จริง → decagram-Goldberg storage + dual view
 * ═══════════════════════════════════════════════════════════════════════
 *
 * รวม chain (T1.2f + T1.2g) ลงไฟล์จริง ผ่าน API ระบบ geo_goldberg_store.h:
 *   1. GGUF tensor bytes → pointer ตรงเข้า mmap (zero-copy, gguf_box.h)
 *   2. ggs_store: ห่อ 64B chunks → gp_lens_write ที่ decagram tile_id
 *        (tile = 12 + sector·(n²−1) + offset, sector = decagram 0..9)
 *        → เขียน→verify→destroy ทีละ sphere (streaming, RAM ~1 sphere)
 *   3. ggs_store verify ภายใน (memcmp กับต้นฉบับ) = lossless
 *   4. dual view: tensor bytes (as float) → geo_codec
 *        dodeca(12) / icosa(20) / compound_144 / goldberg_192
 *        → payload เท่ากัน + decode lossless (container เลือกรูปทรงได้)
 *   5. benchmark: MB/s write+read ผ่าน gp_lens
 *
 * BUILD: make goldberg_probe
 * RUN:   ./build/goldberg_dual_probe <model.gguf> [--all]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#endif
#include "../core/gguf_box.h"
#include "../core/geo_goldberg_store.h"
#include "../core/geo_goldberg_file.h"
#include "../core/geo_ggf_walk.h"
#include "../core/tied_dedup.h"
#include "../core/geo_param_grid.h"

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    /* lazy cache เปิด .ggf พร้อมกันหลายไฟล์ (1 ต่อ home) — Windows
     * จำกัด 512 handles → ขยาย (Kokoro 775 tensors เกิน) */
    _setmaxstdio(2048);
#endif
    if (argc < 2) {
        printf("usage: %s <model.gguf> [--all] [--save <dir>] [--walk]\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];
    int want_all = (argc > 2 && strcmp(argv[2], "--all") == 0);
    const char *save_dir = NULL;
    int want_walk = 0;
    for (int a = 2; a < argc; a++) {
        if (strcmp(argv[a], "--save") == 0 && a + 1 < argc) save_dir = argv[a + 1];
        if (strcmp(argv[a], "--walk") == 0) want_walk = 1;
    }

    GGUFBox box;
    memset(&box, 0, sizeof box);
    if (gguf_box_open(&box, path) != 0) {
        printf("FAIL: cannot open %s\n", path);
        return 1;
    }

    printf("═══ goldberg_dual_probe — GGUF จริง → decagram-Goldberg + dual view ═══\n\n");
    printf("file: %s\n", path);
    printf("tensors: %u · zero-copy: %s\n", box.n_tensors,
           box.reader.base ? "YES (mmap pointer)" : "no");

    /* ── 1. เก็บทุก tensor ผ่าน geo_goldberg_store (API ระบบ) ─────── */
    GoldbergStore gs;
    ggs_init(&gs, 8);                      /* GP(8,0) = 642 faces */

    uint32_t n_ok = 0, n_skip = 0, n_fail = 0;
    uint64_t total_bytes = 0;
    double t0 = now_sec();

    for (uint32_t i = 0; i < box.n_tensors && (want_all || i < 60); i++) {
        const GGUFBoxEntry *e = &box.entries[i];
        if (!e->data || e->size == 0) { n_skip++; continue; }

        /* ggs_store: streaming multi-sphere — RAM = 1 sphere (~1.3MB)
         * แม้ tensor ใหญ่ 144MB (449 spheres) — §15.83 */
        int rc = ggs_store(&gs, e->data, e->size);
        if (rc == 0) {
            n_ok++;
            total_bytes += e->size;
        } else {
            n_fail++;
            printf("  [FAIL] tensor %u %s (%u B, rc=%d)\n", i, e->name, e->size, rc);
        }
    }
    double t1 = now_sec();

    printf("\n── 1. decagram-Goldberg storage (geo_goldberg_store.h, GP(8,0)=642) ──\n");
    printf("tensors stored lossless: %u · skipped: %u · fail: %u (streaming multi-sphere)\n",
           n_ok, n_skip, n_fail);
    printf("bytes stored: %llu (%.1f MB) · chunks: %llu · time: %.3f s · write+read: %.1f MB/s\n",
           (unsigned long long)total_bytes, total_bytes / (1024.0 * 1024.0),
           (unsigned long long)gs.chunks_stored, t1 - t0,
           2.0 * total_bytes / (1024.0 * 1024.0) / ((t1 - t0) ? (t1 - t0) : 1e-9));

    /* ── 1b. persist .ggf ไฟล์จริง (geo_goldberg_file.h) + อ่านกลับ ──
     * --walk: dedup registry (tied_dedup_scan) → save เฉพาะ home — dup
     * ไม่มีไฟล์ของตัวเอง (dedup ระดับไฟล์) */
    uint32_t n_save = 0, n_save_fail = 0, n_save_skip = 0;
    int32_t *home_of = NULL;
    uint64_t dup_bytes = 0;
    if (save_dir) {
        if (want_walk) {
            home_of = (int32_t *)malloc(box.n_tensors * sizeof(int32_t));
            const uint8_t **ptrs = (const uint8_t **)malloc(box.n_tensors * sizeof(void *));
            uint32_t *sizes = (uint32_t *)malloc(box.n_tensors * sizeof(uint32_t));
            for (uint32_t i = 0; i < box.n_tensors; i++) {
                ptrs[i] = box.entries[i].data;
                sizes[i] = box.entries[i].size;
            }
            dup_bytes = tied_dedup_scan(ptrs, sizes, box.n_tensors, home_of);
            free(ptrs); free(sizes);
        }
        double tf0 = now_sec();
        printf("\n── 1b. persist .ggf (ggs_save → file → ggs_load → memcmp) ──\n");
        if (want_walk)
            printf("     dedup registry: dup_bytes = %llu (%.1f MB) — save เฉพาะ home\n",
                   (unsigned long long)dup_bytes, dup_bytes / (1024.0 * 1024.0));
        for (uint32_t i = 0; i < box.n_tensors && (want_all || i < 60); i++) {
            const GGUFBoxEntry *e = &box.entries[i];
            if (!e->data || e->size == 0) continue;
            if (want_walk && home_of[i] != (int32_t)i) { n_save_skip++; continue; }
            /* ชื่อไฟล์ปลอดภัย: sanitize เฉพาะ stem ของ tensor (จุด/สแลช→_) */
            char stem[220], fname[256];
            size_t sl = 0;
            for (const char *q = e->name; *q && sl < sizeof(stem) - 1; q++) {
                char c = *q;
                if (c == '.' || c == '/' || c == '\\' || c == ':') c = '_';
                stem[sl++] = c;
            }
            stem[sl] = 0;
            snprintf(fname, sizeof fname, "%s/t%05u_%s.ggf", save_dir, i, stem);
            int rc = ggs_save(e->data, e->size, 8, fname);
            if (rc != 0) {
                n_save_fail++;
                if (n_save_fail <= 3)
                    printf("  [SAVE-FAIL] %s (rc=%d)\n", e->name, rc);
                continue;
            }
            /* อ่านกลับ + verify byte-for-byte */
            uint64_t need = ((uint64_t)e->size + GGS_CHUNK - 1) / GGS_CHUNK * GGS_CHUNK;
            uint8_t *rb = (uint8_t *)malloc(need);
            uint64_t got = 0;
            int lrc = ggs_load(fname, rb, need, &got);
            int ok = (lrc == 0 && got == (uint64_t)e->size &&
                      memcmp(rb, e->data, e->size) == 0);
            free(rb);
            if (ok) n_save++; else { n_save_fail++; }
        }
        double tf1 = now_sec();
        printf("tensors saved+reloaded lossless: %u · skipped (dup): %u · fail: %u · time: %.3f s\n",
               n_save, n_save_skip, n_save_fail, tf1 - tf0);
    }

    /* ── 1c. walk-clock resolve: state (seed, round, tick) → tensor ── */
    uint32_t n_walk = 0, n_walk_fail = 0;
    if (save_dir && want_walk) {
        double tw0 = now_sec();
        printf("\n── 1c. single read path: state (seed, round, tick) → .ggf (GGFReader) ──\n");
        uint32_t ticks = 12, cycles = 144, seed = 42;
        const char **paths = (const char **)malloc(box.n_tensors * sizeof(char *));
        uint32_t *sizes = (uint32_t *)malloc(box.n_tensors * sizeof(uint32_t));
        uint32_t *rq = (uint32_t *)malloc(box.n_tensors * sizeof(uint32_t));
        char *fname_buf = (char *)malloc(box.n_tensors * 256);
        for (uint32_t i = 0; i < box.n_tensors; i++) {
            char stem[220];
            size_t sl = 0;
            for (const char *q = box.entries[i].name; *q && sl < sizeof(stem) - 1; q++) {
                char c = *q;
                if (c == '.' || c == '/' || c == '\\' || c == ':') c = '_';
                stem[sl++] = c;
            }
            stem[sl] = 0;
            snprintf(fname_buf + i * 256, 256, "%s/t%05u_%s.ggf", save_dir, i, stem);
            paths[i] = fname_buf + i * 256;
            sizes[i] = box.entries[i].size;
        }
        GgfWalkTable tbl;
        ggf_walk_init(&tbl, seed, ticks, cycles, box.n_tensors,
                      paths, sizes, home_of, rq);
        /* read path = GGFMap (zero-copy — เปิด mapping ต่อ home file) */
        GGFMap *cache = (GGFMap *)calloc(box.n_tensors, sizeof(GGFMap));
        uint32_t max_sz = 0;
        for (uint32_t i = 0; i < box.n_tensors; i++)
            if (sizes[i] > max_sz) max_sz = sizes[i];
        uint8_t *scratch = (uint8_t *)malloc(max_sz ? max_sz : 1);
        uint32_t open_cnt = 0;
        uint64_t zero_copy_hits = 0;
        for (uint32_t i = 0; i < box.n_tensors; i++) {
            const GGUFBoxEntry *e = &box.entries[i];
            if (!e->data || e->size == 0) continue;
            FiboWalkPos start = { 7, 3, 0 };        /* enter-anywhere */
            FiboWalkPos end;
            if (!ggf_walk_to(&tbl, start, i, &end)) { n_walk_fail++; continue; }
            uint64_t got = 0;
            int rc = ggf_walk_read_map(&tbl, i, cache, scratch, max_sz, &got);
            if (rc != 0 || got != e->size ||
                memcmp(scratch, e->data, e->size) != 0) {
                n_walk_fail++;
                if (n_walk_fail <= 3)
                    printf("  [WALK-FAIL] %s (rc=%d) path=%s\n", e->name, rc, paths[i]);
            } else {
                n_walk++;
                zero_copy_hits += e->size / 64;      /* nodes อ่านผ่าน mmap */
            }
        }
        for (uint32_t i = 0; i < box.n_tensors; i++)
            if (cache[i].base) open_cnt++;
        double tw1 = now_sec();
        printf("tensors resolved by state (seed=%u, ticks=%u, cycles=%u): %u · fail: %u · "
               "dedup bytes saved: %llu (%.1f MB) · mmap: %u home files mapped (จาก %u)\n",
               seed, ticks, cycles, n_walk, n_walk_fail,
               (unsigned long long)dup_bytes, dup_bytes / (1024.0 * 1024.0),
               open_cnt, n_save + n_save_skip);
        printf("walk-clock resolve+read (mmap zero-copy, %llu nodes): %.1f MB/s\n",
               (unsigned long long)zero_copy_hits,
               (double)total_bytes / (1024.0 * 1024.0) / ((tw1 - tw0) ? (tw1 - tw0) : 1e-9));
        for (uint32_t i = 0; i < box.n_tensors; i++) ggf_unmap(&cache[i]);
        free(cache); free(scratch); free(paths); free(sizes); free(rq); free(fname_buf);
    }

    /* ── 2. dual view: tensor bytes (as float) → geo_codec 4 views ─── */
    printf("\n── 2. dual view (container เลือกรูปทรงได้) ──\n");
    const GGUFBoxEntry *src_e = NULL;
    for (uint32_t i = 0; i < box.n_tensors; i++)
        if (box.entries[i].data && box.entries[i].size >= 4096) { src_e = &box.entries[i]; break; }
    if (!src_e) {
        printf("no tensor with ≥4KB data — skip dual view\n");
    } else {
        /* Q8 bytes ≠ float — แปลง byte → 0..255 lossless (ข้อมูลจริงจากไฟล์) */
        uint32_t nf = src_e->size;
        if (nf > 5000) nf = 5000;
        float *wf = (float *)malloc(nf * sizeof(float));
        for (uint32_t i = 0; i < nf; i++) wf[i] = (float)src_e->data[i];

        GeoCodec views[4];
        GeoType  types[4] = { GEO_DODEC_BASE, GEO_ICO_BASE, GEO_COMPOUND_144, GEO_GOLDBERG_192 };
        const char *tnames[4] = { "dodeca(12)", "icosa(20)", "compound_144", "goldberg_192" };
        int init_all = 1;
        for (int v = 0; v < 4; v++)
            if (geo_codec_init(&views[v], types[v], wf, nf) != 0) init_all = 0;

        int payload_same = init_all;
        for (int v = 1; v < 4; v++)
            if (views[v].n_uniq != views[0].n_uniq ||
                memcmp(views[v].uniq, views[0].uniq, views[0].n_uniq * sizeof(float)) != 0 ||
                memcmp(views[v].idx, views[0].idx, nf * sizeof(uint32_t)) != 0)
                payload_same = 0;

        int lossless_all = init_all;
        float *r0 = (float *)malloc(nf * sizeof(float));
        for (int v = 0; v < 4 && lossless_all; v++) {
            float *rr = (float *)malloc(nf * sizeof(float));
            if (geo_codec_decode(&views[v], rr, nf) != 0) lossless_all = 0;
            for (uint32_t i = 0; i < nf && lossless_all; i++)
                if (rr[i] != wf[i]) lossless_all = 0;
            if (v == 0) memcpy(r0, rr, nf * sizeof(float));
            free(rr);
        }
        int same_result = init_all;
        {
            float *rr = (float *)malloc(nf * sizeof(float));
            if (geo_codec_decode(&views[1], rr, nf) == 0)
                if (memcmp(r0, rr, nf * sizeof(float)) != 0) same_result = 0;
            free(rr);
        }

        printf("tensor: %s (%u B, first %u floats)\n", src_e->name, src_e->size, nf);
        printf("payload (codebook+idx) เท่ากัน 4 views: %s\n", payload_same ? "YES" : "NO");
        printf("decode lossless ทุก view: %s\n", lossless_all ? "YES" : "NO");
        printf("decode(dodeca) == decode(icosa): %s\n", same_result ? "YES" : "NO");
        for (int v = 0; v < 4; v++) {
            printf("  %-14s distinct=%u idx_bits=%u mask=%lluB total=%lluB ratio=%.2fx\n",
                   tnames[v], views[v].n_uniq, views[v].idx_bits,
                   (unsigned long long)views[v].mask_len,
                   (unsigned long long)views[v].total_len, views[v].ratio);
        }
        for (int v = 0; v < 4; v++) geo_codec_free(&views[v]);
        free(wf); free(r0);
    }

    gguf_box_close(&box);
    free(home_of);
    printf("\n═══ DONE ═══\n");
    return (n_fail == 0 && n_save_fail == 0 && n_walk_fail == 0) ? 0 : 1;
}
