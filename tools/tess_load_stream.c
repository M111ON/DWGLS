/* tess_load_stream.c — Streaming Capo Reader (lazy per-capo decode)
 * ═══════════════════════════════════════════════════════════════════════
 * Opens one capo at a time, decodes only requested elements.
 * No big buffer. For inference: load only the slice needed.
 *
 * Usage:
 *   tess_stream <tensor.tess> load <idx>       — decode single element
 *   tess_stream <tensor.tess> range <start> <n> — decode range
 *   tess_stream <tensor.tess> scan              — scan all capos, verify CRC
 *   tess_stream <tensor.tess> info              — show capo metadata
 *
 * Multi-capo: <tensor.tess> is the _capo0.tess base name.
 * ═══════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "geo_tess_container.h"

static int do_info(const char *path) {
    TESS_CapoReader r;
    if (tess_capo_open(&r, path) != 0) {
        fprintf(stderr, "Cannot open: %s\n", path);
        return 1;
    }
    const TESS_Formula *f = r.fml;
    printf("── Capo Info: %s ──\n", path);
    printf("  cell_size:    %u\n", r.cell_size);
    printf("  total_slots:  %u\n", r.hdr->total_slots);
    printf("  tensor_count: %u\n", r.n_elems);
    printf("  capo_id:      %u\n", f->capo_id);
    printf("  capo_total:   %u\n", f->capo_total);
    printf("  mirror_axis_x: %u\n", f->mirror_axis_x);
    printf("  cube_checksum: 0x%016llx\n", (unsigned long long)r.hdr->cube_checksum);
    int crc_ok = tess_capo_verify_crc(&r);
    printf("  crc_valid:    %s\n", crc_ok ? "YES" : "NO");
    tess_capo_close(&r);
    return 0;
}

static int do_load(const char *path, uint32_t idx) {
    TESS_CapoReader r;
    if (tess_capo_open(&r, path) != 0) {
        fprintf(stderr, "Cannot open: %s\n", path);
        return 1;
    }
    if (idx >= r.n_elems) {
        fprintf(stderr, "idx %u out of range (max %u)\n", idx, r.n_elems);
        tess_capo_close(&r);
        return 1;
    }
    uint8_t cell[TESS_CELL_Q4_K];
    int n = tess_capo_load_elem(&r, idx, cell);
    if (n == 0) {
        fprintf(stderr, "Failed to load elem %u\n", idx);
        tess_capo_close(&r);
        return 1;
    }
    printf("elem[%u] = ", idx);
    for (int i = 0; i < n; i++) {
        printf("%02x", cell[i]);
        if (i >= 15) { printf("..."); break; }
    }
    printf("  (%d bytes)\n", n);
    tess_capo_close(&r);
    return 0;
}

static int do_range(const char *path, uint32_t start, uint32_t n) {
    TESS_CapoReader r;
    if (tess_capo_open(&r, path) != 0) {
        fprintf(stderr, "Cannot open: %s\n", path);
        return 1;
    }
    if (start + n > r.n_elems) {
        fprintf(stderr, "range [%u, +%u) out of range (max %u)\n", start, n, r.n_elems);
        tess_capo_close(&r);
        return 1;
    }
    uint8_t *buf = malloc((size_t)n * r.cell_size);
    if (!buf) { tess_capo_close(&r); return 1; }

    clock_t t0 = clock();
    int bytes = tess_capo_load_range(&r, start, n, buf);
    clock_t t1 = clock();

    printf("range[%u, +%u): %d bytes decoded in %.3f ms\n",
           start, n, bytes, (double)(t1 - t0) / CLOCKS_PER_SEC * 1000.0);

    /* show first few elements */
    for (uint32_t i = 0; i < n && i < 4; i++) {
        printf("  elem[%u] = ", start + i);
        for (uint32_t j = 0; j < r.cell_size && j < 8; j++)
            printf("%02x", buf[i * r.cell_size + j]);
        printf("\n");
    }

    free(buf);
    tess_capo_close(&r);
    return 0;
}

static int do_scan(const char *path) {
    printf("── Scanning capos: %s ──\n", path);

    uint32_t count = 0, passed = 0, failed = 0;
    for (uint32_t c = 0; c < 512; c++) {
        char p[1024];
        tess_capo_make_path(p, sizeof(p), path, c);
        FILE *f = fopen(p, "rb");
        if (!f) break;
        fclose(f);

        TESS_CapoReader r;
        if (tess_capo_open(&r, p) != 0) {
            printf("  capo %u: OPEN FAIL\n", c);
            failed++; count++; continue;
        }
        int crc_ok = tess_capo_verify_crc(&r);

        printf("  capo %u: n=%u capo_id=%u capo_total=%u crc=%s\n",
               c, r.n_elems,
               r.fml->capo_id, r.fml->capo_total,
               crc_ok ? "PASS" : "FAIL");

        if (crc_ok) passed++; else failed++;
        count++;
        tess_capo_close(&r);
    }
    printf("Total: %u capos, %u PASS, %u FAIL\n", count, passed, failed);
    return failed > 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: tess_stream <tensor.tess> <cmd> [args]\n");
        fprintf(stderr, "  info              — show capo metadata\n");
        fprintf(stderr, "  load <idx>        — decode single element\n");
        fprintf(stderr, "  range <start> <n> — decode range\n");
        fprintf(stderr, "  scan              — scan all capos, verify CRC\n");
        return 1;
    }
    const char *path = argv[1];
    const char *cmd  = argv[2];

    if (strcmp(cmd, "info") == 0)   return do_info(path);
    if (strcmp(cmd, "load") == 0) {
        if (argc < 4) { fprintf(stderr, "load needs <idx>\n"); return 1; }
        return do_load(path, (uint32_t)atoi(argv[3]));
    }
    if (strcmp(cmd, "range") == 0) {
        if (argc < 5) { fprintf(stderr, "range needs <start> <n>\n"); return 1; }
        return do_range(path, (uint32_t)atoi(argv[3]), (uint32_t)atoi(argv[4]));
    }
    if (strcmp(cmd, "scan") == 0) return do_scan(path);

    fprintf(stderr, "Unknown command: %s\n", cmd);
    return 1;
}
