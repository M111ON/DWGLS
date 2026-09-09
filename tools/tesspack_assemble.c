/* tesspack_assemble.c — .tesspack → standalone GGUF (no source GGUF needed)
 * ═══════════════════════════════════════════════════════════════════════════
 * General assembler: reads .tesspack, extracts embedded GGUF header,
 * decodes ALL tensor types (ONION, SCATTER, RESIDUAL), writes valid GGUF.
 *
 * Streaming design: one tensor decoded → written → freed at a time.
 * O(max_tensor_size) memory, not O(total_model_size).
 *
 * BUILD: gcc -O2 -Wall -I../core -o tesspack_assemble.exe tesspack_assemble.c -lm
 * RUN:   ./tesspack_assemble.exe <model.tesspack> <output.gguf>
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/geo_tess_container.h"
#include "../core/gguf_reader.h"

/* Stub ggml functions needed by tess_pack_apply_residual */
int ggml_type_size(int type) {
    static const int sz[] = {4,2,18,20,0,0,22,24,34,36,84,110,144,176,210,292};
    return (type >= 0 && type < 16) ? sz[type] : 0;
}
int ggml_blck_size(int type) {
    static const int bl[] = {1,1,32,32,0,0,32,32,32,32,256,256,256,256,256,256};
    return (type >= 0 && type < 16) ? bl[type] : 0;
}

static double now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static uint32_t gguf_cell_size(uint32_t dtype) {
    static const uint32_t table[] = {
        4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
    };
    if (dtype < sizeof(table)/sizeof(table[0])) return table[dtype];
    if (dtype == 41) return 6;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: tesspack_assemble <input.tesspack> <output.gguf>\n");
        return 1;
    }
    const char *pack_path = argv[1];
    const char *out_path  = argv[2];

    double t0 = now_ms();
    fprintf(stderr, "=== tesspack_assemble ===\n");
    fprintf(stderr, "Input:  %s\n", pack_path);
    fprintf(stderr, "Output: %s\n", out_path);

    /* ── Step 1: mmap .tesspack ── */
    TESS_PackIndex pi;
    if (tess_pack_open_mmap(&pi, pack_path) != 0) {
        fprintf(stderr, "FATAL: cannot open %s\n", pack_path);
        return 1;
    }
    fprintf(stderr, "Pack: %u capos, %u bytes\n", pi.n_capos, (unsigned)pi.file_sz);

    /* ── Step 2: extract embedded GGUF header ── */
    uint64_t hdr_sz = 0;
    const uint8_t *raw_hdr = tess_pack_get_gguf_header(&pi, &hdr_sz);
    if (!raw_hdr || hdr_sz < 20) {
        fprintf(stderr, "FATAL: no __gguf_header__ in pack (was it created by tess_gguf_pack?)\n");
        tess_pack_close(&pi);
        return 1;
    }
    fprintf(stderr, "Embedded GGUF header: %u bytes\n", (unsigned)hdr_sz);

    /* Write header to temp file so gguf_open can parse it */
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.hdr.tmp", out_path);
    FILE *tf = fopen(tmp_path, "wb");
    if (!tf) { fprintf(stderr, "FATAL: cannot create temp header\n"); tess_pack_close(&pi); return 1; }
    fwrite(raw_hdr, 1, (size_t)hdr_sz, tf);
    fclose(tf);

    GgufReader gguf;
    if (gguf_open(tmp_path, &gguf) != 0) {
        fprintf(stderr, "FATAL: gguf_open failed on embedded header\n");
        remove(tmp_path);
        tess_pack_close(&pi);
        return 1;
    }
    remove(tmp_path);

    fprintf(stderr, "GGUF metadata: %u tensors, data_offset=%u\n",
            gguf.n_tensors, (unsigned)gguf.data_offset);

    /* ── Step 3: write output GGUF ── */
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "FATAL: cannot create %s\n", out_path);
        gguf_close(&gguf);
        tess_pack_close(&pi);
        return 1;
    }

    /* Write the exact embedded header + pad to data_offset */
    fwrite(raw_hdr, 1, (size_t)gguf.data_offset, f);

    fprintf(stderr, "Writing %u tensors...\n", gguf.n_tensors);

    /* Allocate decode buffer */
    uint32_t max_tensor = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++)
        if (gguf.sizes[i] > max_tensor) max_tensor = gguf.sizes[i];
    uint8_t *decode_buf = (uint8_t *)malloc((size_t)max_tensor + 256);

    uint64_t b_written = 0;
    uint32_t n_onion = 0, n_scatter = 0, n_residual = 0, n_error = 0;

    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        const char *name = gguf.names[i];
        uint32_t need = gguf.sizes[i];

        /* ── Try ONION first ── */
        {
            const uint8_t *onion = NULL;
            uint32_t onion_sz = 0;
            if (tess_pack_find_onion(&pi, name, &onion, &onion_sz) == 0) {
                if (onion_sz == need) {
                    fwrite(onion, 1, need, f);
                    b_written += need;
                    n_onion++;
                    if (i < 5 || (i + 1) % 50 == 0)
                        fprintf(stderr, "  [%4u] %-40s ONION   %u bytes\n", i, name, need);
                    continue;
                }
                /* f16→f32 conversion */
                if (gguf.dtypes[i] == 0 && onion_sz == need / 2) {
                    const uint16_t *f16 = (const uint16_t *)onion;
                    float *f32 = (float *)decode_buf;
                    uint32_t n = need / 4;
                    for (uint32_t k = 0; k < n; k++)
                        f32[k] = fp16_to_float(f16[k]);
                    fwrite(decode_buf, 1, need, f);
                    b_written += need;
                    n_onion++;
                    if (i < 5 || (i + 1) % 50 == 0)
                        fprintf(stderr, "  [%4u] %-40s ONION-F16 %u bytes\n", i, name, need);
                    continue;
                }
            }
        }

        /* ── Try SCATTER (capo decode) ── */
        {
            uint32_t csz = gguf_cell_size(gguf.dtypes[i]);
            if (csz > 0 && need >= csz) {
                uint32_t total_cells = need / csz;
                uint32_t capos_needed = (total_cells + TESS_TOTAL_SLOTS - 1) / TESS_TOTAL_SLOTS;
                uint32_t cells_left = total_cells;
                uint8_t *dst = decode_buf;
                int ok = 1;

                for (uint32_t c = 0; c < capos_needed && cells_left > 0; c++) {
                    TESS_CapoReader cr;
                    if (tess_pack_get_capo_mmap(&pi, &cr, name, c) != 0) {
                        ok = 0; break;
                    }
                    uint32_t cells = (cells_left >= TESS_TOTAL_SLOTS)
                                   ? TESS_TOTAL_SLOTS : cells_left;
                    uint32_t got = (uint32_t)tess_capo_load_range(&cr, 0, cells,
                                                                   dst + (uint64_t)c * TESS_TOTAL_SLOTS * csz);
                    if (got != cells * csz) { ok = 0; break; }
                    cells_left -= cells;
                }

                if (ok && cells_left == 0) {
                    fwrite(decode_buf, 1, need, f);
                    b_written += need;
                    n_scatter++;
                    if (i < 5 || (i + 1) % 50 == 0)
                        fprintf(stderr, "  [%4u] %-40s SCATTER %u bytes (%u capos)\n",
                                i, name, need, capos_needed);
                    continue;
                }
            }
        }

        /* ── Try RESIDUAL redirect ── */
        {
            const TESS_ResidualEntry *re = tess_pack_find_residual(&pi, name);
            if (re) {
                uint32_t n_elems = 1;
                for (uint8_t d = 0; d < gguf.n_dims[i]; d++)
                    n_elems *= (uint32_t)gguf.dims[i*4+d];
                int written = tess_pack_apply_residual(&pi, re, n_elems, decode_buf);
                if (written > 0) {
                    fwrite(decode_buf, 1, (size_t)written, f);
                    b_written += (uint32_t)written;
                    n_residual++;
                    if (i < 5 || (i + 1) % 50 == 0)
                        fprintf(stderr, "  [%4u] %-40s RESIDUAL %d bytes\n", i, name, written);
                    continue;
                }
            }
        }

        /* ── Zero-fill for missing tensors ── */
        memset(decode_buf, 0, need);
        fwrite(decode_buf, 1, need, f);
        b_written += need;
        n_error++;
        fprintf(stderr, "  [%4u] %-40s MISSING → zero-fill %u bytes\n", i, name, need);
    }

    fclose(f);
    free(decode_buf);

    double elapsed = now_ms() - t0;
    fprintf(stderr, "\n=== Done in %.1f ms ===\n", elapsed);
    fprintf(stderr, "ONION: %u  SCATTER: %u  RESIDUAL: %u  MISSING: %u  TOTAL: %u\n",
            n_onion, n_scatter, n_residual, n_error, gguf.n_tensors);
    fprintf(stderr, "Written: %u bytes (header %u + data %u)\n",
            (unsigned)(gguf.data_offset + b_written),
            (unsigned)gguf.data_offset,
            (unsigned)b_written);

    /* Verify output with gguf_open */
    GgufReader verify;
    if (gguf_open(out_path, &verify) == 0) {
        fprintf(stderr, "Verify: %u tensors, data_offset=%u ✓\n",
                verify.n_tensors, (unsigned)verify.data_offset);
        gguf_close(&verify);
    } else {
        fprintf(stderr, "Verify: FAILED to open output GGUF\n");
    }

    gguf_close(&gguf);
    tess_pack_close(&pi);
    return n_error > 0 ? 1 : 0;
}
