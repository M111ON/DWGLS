/* tesspack_assemble.c — .tesspack → standalone GGUF
 * ═══════════════════════════════════════════════════════════════════════════
 * General assembler: reads .tesspack, extracts embedded GGUF header,
 * decodes ALL tensor types (ONION, SCATTER, RESIDUAL), writes valid GGUF.
 *
 * Streaming design: one tensor decoded → written → freed at a time.
 * O(max_tensor_size) memory, not O(total_model_size).
 *
 * NEW: Optional GGUF fallback — pass original.gguf as 3rd arg to fill
 * tensors missing from the tesspack (filtered during pack creation).
 *
 * BUILD: gcc -O2 -Wall -I../core -o tesspack_assemble.exe tesspack_assemble.c -lm
 * RUN:   ./tesspack_assemble <model.tesspack> <output.gguf> [original.gguf]
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
    if (type == 41) return 6;
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
    return (double)ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
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
        fprintf(stderr, "Usage: tesspack_assemble <input.tesspack> <output.gguf> [original.gguf]\n");
        return 1;
    }
    const char *pack_path = argv[1];
    const char *out_path  = argv[2];
    const char *fallback_path = (argc >= 4) ? argv[3] : NULL;

    double t0 = now_ms();
    fprintf(stderr, "=== tesspack_assemble ===\n");
    fprintf(stderr, "Input:  %s\n", pack_path);
    fprintf(stderr, "Output: %s\n", out_path);
    if (fallback_path)
        fprintf(stderr, "Fallback: %s (fill missing tensors from original)\n", fallback_path);

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

    /* Write header to temp file so gguf_open can parse it
     * Use output directory (not %TEMP%) to avoid C: drive space issues */
    char tmp_path[512];
    {
        /* Find last separator in out_path to get directory */
        const char *last_sep = strrchr(out_path, '\\');
        if (!last_sep) last_sep = strrchr(out_path, '/');
        if (last_sep) {
            size_t dir_len = (size_t)(last_sep - out_path);
            snprintf(tmp_path, sizeof(tmp_path), "%.*s\\tesspack_hdr.tmp",
                     (int)dir_len, out_path);
        } else {
            snprintf(tmp_path, sizeof(tmp_path), "tesspack_hdr.tmp");
        }
    }
    FILE *tf = fopen(tmp_path, "wb");
    if (!tf) { fprintf(stderr, "FATAL: cannot create temp header at %s\n", tmp_path); tess_pack_close(&pi); return 1; }
    size_t written = fwrite(raw_hdr, 1, (size_t)hdr_sz, tf);
    fclose(tf);
    if (written != (size_t)hdr_sz) {
        fprintf(stderr, "FATAL: temp header truncated (%u / %u bytes)\n",
                (unsigned)written, (unsigned)hdr_sz);
        remove(tmp_path);
        tess_pack_close(&pi);
        return 1;
    }

    GgufReader gguf;
    if (gguf_open(tmp_path, &gguf) != 0) {
        fprintf(stderr, "FATAL: gguf_open failed on embedded header (%s)\n", tmp_path);
        remove(tmp_path);
        tess_pack_close(&pi);
        return 1;
    }
    remove(tmp_path);

    fprintf(stderr, "GGUF metadata: %u tensors, data_offset=%u\n",
            gguf.n_tensors, (unsigned)gguf.data_offset);

    /* ── Step 3: open fallback GGUF if provided ── */
    GgufReader fallback;
    int have_fallback = 0;
    if (fallback_path) {
        if (gguf_open(fallback_path, &fallback) != 0) {
            fprintf(stderr, "WARNING: cannot open fallback %s, continuing without\n", fallback_path);
        } else {
            have_fallback = 1;
            fprintf(stderr, "Fallback GGUF: %u tensors\n", fallback.n_tensors);
        }
    }

    /* ── Step 4: patch header offsets to sequential layout ──
     * Original GGUF may have non-sequential tensor offsets (gaps for alignment).
     * We rewrite them as sequential with GGUF_ALIGN padding, matching tess_assemble.c.
     * Copy header to mutable buffer first (raw_hdr is mmap'd read-only). */
    uint8_t *hdr_buf = (uint8_t *)malloc((size_t)hdr_sz);
    if (!hdr_buf) { fprintf(stderr, "FATAL: OOM for header copy\n"); return 1; }
    memcpy(hdr_buf, raw_hdr, (size_t)hdr_sz);
    {
        /* Walk KV block to find tensor info start in header */
        const uint8_t *p = hdr_buf + 24;
        const uint8_t *hdr_end = hdr_buf + hdr_sz;
        uint64_t nkv;
        memcpy(&nkv, hdr_buf + 16, 8);
        for (uint64_t k = 0; k < nkv; k++) {
            if (p + 8 > hdr_end) break;
            uint64_t klen; memcpy(&klen, p, 8); p += 8 + klen;
            if (p + 4 > hdr_end) break;
            uint32_t vtype; memcpy(&vtype, p, 4); p += 4;
            switch (vtype) {
            case 0: case 1: case 7: p += 1; break;
            case 2: case 3: p += 2; break;
            case 4: case 5: case 6: p += 4; break;
            case 10: case 11: case 12: p += 8; break;
            case 8: { uint64_t sl; memcpy(&sl, p, 8); p += 8 + sl; } break;
            case 9: {
                uint32_t et; uint64_t na; memcpy(&et, p, 4); memcpy(&na, p+4, 8); p += 12;
                if (et == 8) { for (uint64_t a=0;a<na;a++){uint64_t sl;memcpy(&sl,p,8);p+=8+sl;} }
                else { static const uint8_t esz[]={1,1,2,2,4,4,4,1,0,0,8,8,8}; p+=esz[et<13?et:0]*na; }
            } break;
            }
        }

        /* Patch tensor offsets to sequential layout */
        uint64_t data_off = 0;
        uint64_t pos = (uint64_t)(p - hdr_buf);
        for (uint64_t i = 0; i < gguf.n_tensors; i++) {
            uint64_t nlen; memcpy(&nlen, hdr_buf+pos, 8); pos += 8 + nlen;
            uint32_t nd;   memcpy(&nd, hdr_buf+pos, 4);   pos += 4 + 8*nd;
            pos += 4; /* dtype */
            uint32_t align = (uint32_t)((GGUF_ALIGN - (data_off % GGUF_ALIGN)) % GGUF_ALIGN);
            data_off += align;
            /* Write new sequential offset */
            memcpy(hdr_buf + pos, &data_off, 8);
            data_off += gguf.sizes[i];
            pos += 8;
        }
        fprintf(stderr, "Patched %u tensor offsets to sequential layout\n", gguf.n_tensors);
    }

    /* ── Step 5: write output GGUF ── */
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "FATAL: cannot create %s\n", out_path);
        free(hdr_buf);
        if (have_fallback) gguf_close(&fallback);
        gguf_close(&gguf);
        tess_pack_close(&pi);
        return 1;
    }

    /* Write the patched header */
    fwrite(hdr_buf, 1, (size_t)gguf.data_offset, f);

    fprintf(stderr, "Writing %u tensors...\n", gguf.n_tensors);

    /* Allocate decode buffer */
    uint32_t max_tensor = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++)
        if (gguf.sizes[i] > max_tensor) max_tensor = gguf.sizes[i];
    uint8_t *decode_buf = (uint8_t *)malloc((size_t)max_tensor + 256);
    uint8_t zeros[32] = {0};

    uint64_t b_written = 0;
    uint32_t n_onion = 0, n_scatter = 0, n_residual = 0, n_fallback = 0, n_error = 0;

    uint64_t data_off = 0;
    for (uint32_t i = 0; i < gguf.n_tensors; i++) {
        const char *name = gguf.names[i];
        uint32_t need = gguf.sizes[i];

        /* Align to GGUF_ALIGN and pad */
        uint32_t align = (uint32_t)((GGUF_ALIGN - (data_off % GGUF_ALIGN)) % GGUF_ALIGN);
        data_off += align;
        while ((uint64_t)ftell(f) < gguf.data_offset + data_off) {
            size_t gap = (gguf.data_offset + data_off) - (size_t)ftell(f);
            fwrite(zeros, 1, gap > 32 ? 32 : gap, f);
        }

        int wrote = 0;

        /* ── Try ONION first ── */
        if (!wrote) {
            const uint8_t *onion = NULL;
            uint32_t onion_sz = 0;
            if (tess_pack_find_onion(&pi, name, &onion, &onion_sz) == 0) {
                if (onion_sz == need) {
                    fwrite(onion, 1, need, f);
                    b_written += need;
                    n_onion++;
                    wrote = 1;
                    if (i < 5 || (i + 1) % 50 == 0)
                        fprintf(stderr, "  [%4u] %-40s ONION   %u bytes\n", i, name, need);
                }
                /* f16→f32 conversion */
                if (!wrote && gguf.dtypes[i] == 0 && onion_sz == need / 2) {
                    const uint16_t *f16 = (const uint16_t *)onion;
                    float *f32 = (float *)decode_buf;
                    uint32_t n = need / 4;
                    for (uint32_t k = 0; k < n; k++)
                        f32[k] = fp16_to_float(f16[k]);
                    fwrite(decode_buf, 1, need, f);
                    b_written += need;
                    n_onion++;
                    wrote = 1;
                    if (i < 5 || (i + 1) % 50 == 0)
                        fprintf(stderr, "  [%4u] %-40s ONION-F16 %u bytes\n", i, name, need);
                }
            }
        }

        /* ── Try SCATTER (capo decode) ── */
        if (!wrote) {
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
                    wrote = 1;
                    if (i < 5 || (i + 1) % 50 == 0)
                        fprintf(stderr, "  [%4u] %-40s SCATTER %u bytes (%u capos)\n",
                                i, name, need, capos_needed);
                }
            }
        }

        /* ── Try RESIDUAL redirect ── */
        if (!wrote) {
            const TESS_ResidualEntry *re = tess_pack_find_residual(&pi, name);
            if (re) {
                uint32_t n_elems = 1;
                for (uint8_t d = 0; d < gguf.n_dims[i]; d++)
                    n_elems *= (uint32_t)gguf.dims[i*4+d];
                int wr = tess_pack_apply_residual(&pi, re, n_elems, decode_buf);
                if (wr > 0) {
                    fwrite(decode_buf, 1, (size_t)wr, f);
                    b_written += (uint32_t)wr;
                    n_residual++;
                    wrote = 1;
                    if (i < 5 || (i + 1) % 50 == 0)
                        fprintf(stderr, "  [%4u] %-40s RESIDUAL %d bytes\n", i, name, wr);
                }
            }
        }

        /* ── Fallback: read from original GGUF if provided ── */
        if (!wrote && have_fallback) {
            int fi = -1;
            for (uint32_t j = 0; j < fallback.n_tensors; j++) {
                if (strcmp(fallback.names[j], name) == 0) { fi = j; break; }
            }
            if (fi >= 0 && fallback.sizes[fi] == need) {
                const uint8_t *src = fallback.base + fallback.data_offset + fallback.offsets[fi];
                fwrite(src, 1, need, f);
                b_written += need;
                n_fallback++;
                wrote = 1;
                if (i < 5 || (i + 1) % 50 == 0)
                    fprintf(stderr, "  [%4u] %-40s FALLBACK %u bytes\n", i, name, need);
            }
        }

        /* ── Zero-fill for missing tensors ── */
        if (!wrote) {
            memset(decode_buf, 0, need);
            fwrite(decode_buf, 1, need, f);
            b_written += need;
            n_error++;
            fprintf(stderr, "  [%4u] %-40s MISSING → zero-fill %u bytes\n", i, name, need);
        }

        data_off += need;
    }

    fclose(f);
    free(decode_buf);
    free(hdr_buf);

    double elapsed = now_ms() - t0;
    fprintf(stderr, "\n=== Done in %.1f ms ===\n", elapsed);
    fprintf(stderr, "ONION: %u  SCATTER: %u  RESIDUAL: %u  FALLBACK: %u  MISSING: %u  TOTAL: %u\n",
            n_onion, n_scatter, n_residual, n_fallback, n_error, gguf.n_tensors);
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

    if (have_fallback) gguf_close(&fallback);
    gguf_close(&gguf);
    tess_pack_close(&pi);
    return n_error > 0 ? 1 : 0;
}
