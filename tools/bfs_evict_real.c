/*
 * bfs_evict_real.c — card #46 REAL proof: stream a full GGUF through BFS
 * ═══════════════════════════════════════════════════════════════════
 * Every tensor is sliced into <=20736 B chunks; each chunk is one BFS file
 * under a tight residency cap (default 8 blocks) with a disk spill backend.
 * Verify re-reads source bytes straight from the GGUF (independent oracle)
 * and memcmps every slice. Peak residency + mismatch count decide.
 *
 * The logical set (1000s of slice files, 100s of MB) vastly exceeds the
 * resident set (cap blocks) — the expert-streaming pattern for card #3.
 *
 * Usage: bfs_evict_real <model.gguf> <spill_dir> [max_tensors] [cap_blocks]
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -no-pie -o build/bfs_evict_real tools/bfs_evict_real.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "breathing_fs.h"
#include "gguf_reader.h"

#define SLICE_MAX_CAP 144u   /* slices never exceed the residency cap (set below) */

static char g_dir[512];

static void spill_path(char *out, size_t cap, const char *name) {
    snprintf(out, cap, "%s/%s.bin", g_dir, name);
}
static int disk_spill(const char *name, const int8_t *data, uint32_t size, void *u) {
    (void)u;
    char p[640];
    spill_path(p, sizeof(p), name);
    FILE *f = fopen(p, "wb");
    if (!f) return 1;
    size_t w = fwrite(data, 1, size, f);
    fclose(f);
    return w == size ? 0 : 1;
}
static int disk_fill(const char *name, int8_t **out, uint32_t *size, void *u) {
    (void)u;
    char p[640];
    spill_path(p, sizeof(p), name);
    FILE *f = fopen(p, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 1; }
    *out = (int8_t *)malloc((size_t)n);
    if (!*out) { fclose(f); return -1; }
    size_t r = fread(*out, 1, (size_t)n, f);
    fclose(f);
    if (r != (size_t)n) { free(*out); *out = NULL; return -1; }
    *size = (uint32_t)n;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: bfs_evict_real <model.gguf> <spill_dir> [max_tensors] [cap_blocks]\n");
        return 2;
    }
    const char *gguf_path = argv[1];
    snprintf(g_dir, sizeof(g_dir), "%s", argv[2]);
    uint32_t max_tensors = argc > 3 ? (uint32_t)atoi(argv[3]) : 0;
    uint32_t cap = argc > 4 ? (uint32_t)atoi(argv[4]) : 8;

#ifdef _WIN32
    mkdir(g_dir);
#else
    mkdir(g_dir, 0755);
#endif

    GgufReader r;
    memset(&r, 0, sizeof(r));
    if (gguf_open(gguf_path, &r) != 0) { printf("FAIL: gguf_open %s\n", gguf_path); return 1; }
    uint32_t nt = r.n_tensors;
    if (max_tensors && max_tensors < nt) nt = max_tensors;

    BreathingFS fs;
    bfs_init(&fs);
    bfs_set_spill(&fs, disk_spill, disk_fill, NULL);
    bfs_set_capacity(&fs, cap);
    /* one slice must fit inside the cap: slice <= cap blocks */
    uint32_t slice_max = cap * BFS_SLOTS_BLOCK;
    printf("model: %s  tensors=%u (of %u)  cap=%u blocks  slice<=%u B\n",
           gguf_path, nt, r.n_tensors, cap, slice_max);

    /* ── phase 1: stream every tensor in as slices ── */
    uint64_t total_bytes = 0, total_slices = 0;
    uint32_t peak_blocks = 0;
    uint8_t *tbuf = NULL;
    size_t tcap = 0;
    for (uint32_t t = 0; t < nt; t++) {
        uint32_t sz = r.sizes[t];
        if (sz > tcap) {
            free(tbuf);
            tbuf = (uint8_t *)malloc(sz);
            if (!tbuf) { printf("FAIL: OOM tensor %u (%u B)\n", t, sz); return 1; }
            tcap = sz;
        }
        if (gguf_read_tensor(gguf_path, &r, t, tbuf, sz) != 0) {
            printf("FAIL: read tensor %u\n", t);
            return 1;
        }
            uint32_t ns = (sz + slice_max - 1) / slice_max;
        for (uint32_t s = 0; s < ns; s++) {
            uint32_t off = s * slice_max;
            uint32_t n = sz - off > slice_max ? slice_max : sz - off;
            char nm[32];
            snprintf(nm, sizeof(nm), "t%04u_s%03u", t, s);
            if (bfs_write(&fs, nm, (const int8_t *)(tbuf + off), n) != 0) {
                printf("FAIL: bfs_write %s (tensor %u slice %u, %u B)\n", nm, t, s, n);
                return 1;
            }
            if (fs.n_blocks_used > peak_blocks) peak_blocks = fs.n_blocks_used;
            total_bytes += n;
            total_slices++;
        }
        if ((t + 1) % 50 == 0)
            printf("  streamed %u/%u tensors  slices=%llu  peak=%u blocks\n",
                   t + 1, nt, (unsigned long long)total_slices, peak_blocks);
    }
    printf("streamed: tensors=%u slices=%llu bytes=%llu peak_residency=%u blocks (cap %u)\n",
           nt, (unsigned long long)total_slices, (unsigned long long)total_bytes,
           peak_blocks, cap);

    /* ── phase 2: verify every slice against re-read source bytes ── */
    uint64_t vbytes = 0;
    uint32_t bad = 0, bad_first_t = 0, bad_first_s = 0;
    int8_t *vbuf = (int8_t *)malloc(slice_max);
    if (!vbuf) { printf("FAIL: OOM verify buf\n"); return 1; }
    for (uint32_t t = 0; t < nt; t++) {
        uint32_t sz = r.sizes[t];
        if (sz > tcap) {
            free(tbuf);
            tbuf = (uint8_t *)malloc(sz);
            if (!tbuf) { printf("FAIL: OOM re-read tensor %u\n", t); return 1; }
            tcap = sz;
        }
        if (gguf_read_tensor(gguf_path, &r, t, tbuf, sz) != 0) {
            printf("FAIL: re-read tensor %u\n", t);
            return 1;
        }
        uint32_t ns = (sz + slice_max - 1) / slice_max;
        for (uint32_t s = 0; s < ns; s++) {
            uint32_t off = s * slice_max;
            uint32_t n = sz - off > slice_max ? slice_max : sz - off;
            char nm[32];
            snprintf(nm, sizeof(nm), "t%04u_s%03u", t, s);
            uint32_t act = 0;
            int rc = bfs_read(&fs, nm, vbuf, slice_max, &act);
            if (rc != 0 || act != n || memcmp(vbuf, tbuf + off, n) != 0) {
                if (bad == 0) { bad_first_t = t; bad_first_s = s; }
                bad++;
            }
            vbytes += n;
        }
        if ((t + 1) % 50 == 0)
            printf("  verified %u/%u tensors  bad=%u\n", t + 1, nt, bad);
    }
    printf("verified: slices=%llu bytes=%llu mismatch=%u",
           (unsigned long long)total_slices, (unsigned long long)vbytes, bad);
    if (bad) printf("  first at tensor %u slice %u", bad_first_t, bad_first_s);
    printf("\n");
    uint32_t rb, rbytes, rcap;
    bfs_residency(&fs, &rb, &rbytes, &rcap);
    printf("final residency: %u blocks / %u bytes / cap %u\n", rb, rbytes, rcap);

    free(tbuf);
    free(vbuf);
    gguf_close(&r);
    if (bad == 0 && peak_blocks <= cap) {
        printf("REAL-PROOF PASS: %llu bytes lossless, peak %u <= cap %u\n",
               (unsigned long long)total_bytes, peak_blocks, cap);
        return 0;
    }
    printf("REAL-PROOF FAIL\n");
    return 1;
}
