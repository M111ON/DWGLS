/*
 * ggf_mmap_bench.c — benchmark: ggs_load vs lazy (GGFReader) vs mmap (GGFMap)
 * ═══════════════════════════════════════════════════════════════════════
 *
 * T1.2l — เทียบ read path 3 แบบบน .ggf ไฟล์จริง (ไฟล์เดียว ข้อมูลเดียว):
 *   1. ggs_load      — โหลดทั้งไฟล์ (fread ต่อเนื่อง, buffer ทั้งไฟล์)
 *   2. lazy seek     — GGFReader: fseek+fread ต่อ node (RAM คงที่)
 *   3. mmap          — GGFMap: อ่านตรงจากเพจ (zero-copy pointer / memcpy)
 *
 * วัด 2 รูปแบบ:
 *   sequential — อ่านทุก node เรียงลำดับ (workload ของการอ่านทั้ง tensor)
 *   random     — อ่าน node สุ่ม 100K ครั้ง (workload ของ random access)
 *
 * BUILD: make ggf_bench
 * RUN:   ./build/ggf_mmap_bench <file.ggf>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../core/geo_goldberg_file.h"

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint64_t rng = 999;
static uint64_t rnd(void)
{
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return rng >> 33;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s <file.ggf>\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];

    /* ขนาด + จำนวน node */
    FILE *f = fopen(path, "rb");
    GGFHeader h;
    if (!f || fread(&h, sizeof h, 1, f) != 1) { printf("cannot read header\n"); return 1; }
    fclose(f);
    uint64_t n_chunks = h.n_chunks;
    uint64_t data_bytes = h.n_bytes;
    const uint64_t NSEQ = 1000 * 1000 / 8;   /* 125K reads สำหรับ timing (กันจุก) */

    printf("═══ ggf_mmap_bench — %s ═══\n", path);
    printf("n_bytes: %llu (%.1f MB) · chunks: %llu · level: %u\n\n",
           (unsigned long long)data_bytes, data_bytes / (1024.0 * 1024.0),
           (unsigned long long)n_chunks, h.level);

    uint8_t *out = (uint8_t *)malloc(GGS_CHUNK);
    uint64_t total_read = 0;

    /* ── 1. ggs_load — full file ────────────────────────────────── */
    {
        uint64_t need = n_chunks * GGS_CHUNK;
        uint8_t *buf = (uint8_t *)malloc(need);
        double t0 = now_sec();
        uint64_t got = 0;
        int rc = ggs_load(path, buf, need, &got);
        double dt = now_sec() - t0;
        printf("1. ggs_load   (full buffer)      : %8.1f MB/s  (%.2f s, rc=%d, %llu B)\n",
               data_bytes / (1024.0 * 1024.0) / (dt ? dt : 1e-9), dt, rc,
               (unsigned long long)got);
        total_read += data_bytes;
        free(buf);
    }

    /* ── 2. lazy seek (GGFReader) ───────────────────────────────── */
    {
        GGFReader r;
        if (ggf_open(path, &r) != 0) { printf("2. lazy: open fail\n"); return 1; }
        /* sequential — อ่านจนครบไฟล์ (จำกัด NSEQ กันจุก) */
        uint64_t n = n_chunks < NSEQ ? n_chunks : NSEQ;
        double t0 = now_sec();
        for (uint64_t k = 0; k < n; k++) ggf_chunk(&r, k, out);
        double dt = now_sec() - t0;
        printf("2. lazy seek (fseek+fread/node) : %8.1f MB/s  (%.2f s, %llu nodes)\n",
               (n * GGS_CHUNK) / (1024.0 * 1024.0) / (dt ? dt : 1e-9), dt,
               (unsigned long long)n);
        total_read += n * GGS_CHUNK;
        /* random 100K */
        t0 = now_sec();
        for (uint64_t i = 0; i < 100000; i++) ggf_chunk(&r, rnd() % n_chunks, out);
        dt = now_sec() - t0;
        printf("   lazy random 100K nodes        : %8.1f MB/s  (%.2f s)\n",
               (100000.0 * GGS_CHUNK) / (1024.0 * 1024.0) / (dt ? dt : 1e-9), dt);
        total_read += 100000ULL * GGS_CHUNK;
        ggf_close(&r);
    }

    /* ── 3. mmap (GGFMap) ───────────────────────────────────────── */
    {
        GGFMap m;
        if (ggf_map(path, &m) != 0) { printf("3. mmap: map fail\n"); return 1; }
        /* sequential — zero-copy pointer (ไม่ copy) */
        uint64_t n = n_chunks < NSEQ ? n_chunks : NSEQ;
        double t0 = now_sec();
        for (uint64_t k = 0; k < n; k++) {
            const uint8_t *p = ggf_map_node(&m, k, NULL);
            if (!p) { printf("mmap node NULL @ %llu\n", (unsigned long long)k); break; }
        }
        double dt = now_sec() - t0;
        printf("3. mmap ptr   (zero-copy)       : %8.1f MB/s  (%.2f s, %llu nodes)\n",
               (n * GGS_CHUNK) / (1024.0 * 1024.0) / (dt ? dt : 1e-9), dt,
               (unsigned long long)n);
        total_read += n * GGS_CHUNK;
        /* sequential — copy 64B ต่อ node (เทียบเท่า workload จริง) */
        t0 = now_sec();
        for (uint64_t k = 0; k < n; k++) ggf_map_chunk(&m, k, out);
        dt = now_sec() - t0;
        printf("   mmap copy  (memcpy/node)     : %8.1f MB/s  (%.2f s)\n",
               (n * GGS_CHUNK) / (1024.0 * 1024.0) / (dt ? dt : 1e-9), dt);
        total_read += n * GGS_CHUNK;
        /* random 100K zero-copy */
        t0 = now_sec();
        for (uint64_t i = 0; i < 100000; i++) {
            const uint8_t *p = ggf_map_node(&m, rnd() % n_chunks, NULL);
            if (!p) { printf("mmap random NULL\n"); break; }
        }
        dt = now_sec() - t0;
        printf("   mmap random 100K (ptr)       : %8.1f MB/s  (%.2f s)\n",
               (100000.0 * GGS_CHUNK) / (1024.0 * 1024.0) / (dt ? dt : 1e-9), dt);
        total_read += 100000ULL * GGS_CHUNK;
        ggf_unmap(&m);
    }

    printf("\ntotal bytes read: %llu (%.1f MB)\n",
           (unsigned long long)total_read, total_read / (1024.0 * 1024.0));
    free(out);
    printf("\n═══ DONE ═══\n");
    return 0;
}
