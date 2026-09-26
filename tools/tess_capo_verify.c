/* tess_capo_verify — spot-check .tesspack roundtrip without full GGUF write.
 * Ported 2026-09-26 from the stale .capo-file + hash-guess version to the
 * current API: GgufReader (source bytes) + TESS_PackIndex (pack bytes).
 *
 * Resolution order per tensor mirrors tools/tesspack_server.c load_pack_tensor:
 *   1. ONION (raw f16/f32 entry, exact-size memcpy)
 *   2. scatter capos (tess_pack_get_capo_mmap + tess_capo_load_range)
 *   3. residual entry (tess_pack_find_residual + tess_pack_apply_residual)
 * Decoded bytes are memcmp'd against the GGUF source in full — no hashing,
 * no header-offset guessing. Sampling is deterministic stride, not rand().
 *
 * Pass: every sampled tensor byte-identical. Fail: any mismatch/unresolved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "llama.h"
#include "ggml-backend.h"
/* ggml.h's enum-typed ggml_type_size/blck_size win; suppress the header's
 * int-typed forward decls (would conflict). Same pattern as tesspack_server.c. */
#define GGML_TYPE_SIZE_DECL
#include "../core/gguf_reader.h"
#include "../core/geo_tess_container.h"

static uint32_t cell_size_of(enum ggml_type type) {
    static const uint32_t C[16] = {
        4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
    };
    int idx = (int)type;
    if (idx < 0 || idx >= 16) return 0;
    return C[idx];
}

/* Resolve one tensor's full bytes from the pack. Returns malloc'd buffer
 * (*out_sz = byte count) or NULL on failure. Sets *path to 1/2/3. */
static uint8_t *resolve_tensor(TESS_PackIndex *pi, const char *name,
                               uint32_t dtype, uint64_t total_sz,
                               uint32_t *out_sz, int *path) {
    uint32_t csz = cell_size_of((enum ggml_type)dtype);
    if (csz == 0 || total_sz % csz != 0) return NULL;
    uint64_t total_cells = total_sz / csz;

    uint8_t *dst = (uint8_t *)malloc((size_t)total_sz);
    if (!dst) return NULL;

    /* 1. ONION */
    {
        const uint8_t *onion = NULL;
        uint32_t onion_sz = 0;
        if (tess_pack_find_onion(pi, name, &onion, &onion_sz) == 0 &&
            onion_sz == total_sz) {
            memcpy(dst, onion, (size_t)total_sz);
            *out_sz = (uint32_t)total_sz;
            *path = 1;
            return dst;
        }
    }
    /* 2. scatter capos (dense ids 0..count-1; stop at first miss) */
    {
        uint64_t cells_left = total_cells;
        uint32_t c = 0;
        int ok = 1;
        while (cells_left > 0) {
            TESS_CapoReader cr;
            if (tess_pack_get_capo_mmap(pi, &cr, name, c) != 0) { ok = 0; break; }
            uint32_t cells = (cells_left >= TESS_TOTAL_SLOTS)
                           ? TESS_TOTAL_SLOTS : (uint32_t)cells_left;
            uint8_t *dst_c = dst + (uint64_t)c * TESS_TOTAL_SLOTS * csz;
            uint32_t got = (uint32_t)tess_capo_load_range(&cr, 0, cells, dst_c);
            if (got != cells * csz) { ok = 0; break; }
            cells_left -= cells;
            c++;
            if (c > 4096) { ok = 0; break; }
        }
        if (ok && cells_left == 0) {
            *out_sz = (uint32_t)total_sz;
            *path = 2;
            return dst;
        }
    }
    /* 3. residual entry */
    {
        const TESS_ResidualEntry *re = tess_pack_find_residual(pi, name);
        if (re) {
            uint32_t dst_csz = (uint32_t)ggml_type_size(re->dst_type);
            if (dst_csz != 0 && total_sz % dst_csz == 0) {
                uint32_t n_elems = (uint32_t)(total_sz / dst_csz);
                int wrote = tess_pack_apply_residual(pi, re, n_elems, dst);
                if (wrote == (int)total_sz) {
                    *out_sz = (uint32_t)total_sz;
                    *path = 3;
                    return dst;
                }
            }
        }
    }
    free(dst);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <model.gguf> <pack.tesspack> [--sample N]\n", argv[0]);
        return 1;
    }
    int sample_count = 20;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--sample") && i + 1 < argc) sample_count = atoi(argv[++i]);
    }

    GgufReader g;
    if (gguf_open(argv[1], &g) != 0) {
        fprintf(stderr, "FAIL: cannot open %s\n", argv[1]);
        return 1;
    }
    TESS_PackIndex pi;
    if (tess_pack_open(&pi, argv[2]) != 0) {
        fprintf(stderr, "FAIL: cannot open %s\n", argv[2]);
        gguf_close(&g);
        return 1;
    }

    uint32_t n = g.n_tensors;
    printf("GGUF: %u tensors | pack: %u capos\n", n, pi.n_capos);
    if (sample_count > (int)n) sample_count = (int)n;
    if (sample_count < 1) sample_count = 1;

    int pass = 0, fail = 0;
    int via_onion = 0, via_scatter = 0, via_residual = 0;
    /* deterministic stride sampling: covers head, middle, tail */
    for (int s = 0; s < sample_count; s++) {
        uint32_t i = (uint32_t)((uint64_t)s * n / (uint32_t)sample_count);
        const char *name = g.names[i];
        uint64_t off = g.data_offset + g.offsets[i];
        uint64_t tsz = g.sizes[i];

        uint32_t got_sz = 0;
        int path = 0;
        uint8_t *dec = resolve_tensor(&pi, name, g.dtypes[i], tsz, &got_sz, &path);
        if (!dec) {
            fail++;
            printf("UNRESOLVED: %s\n", name);
            continue;
        }
        if (got_sz == tsz && memcmp(dec, g.base + off, (size_t)tsz) == 0) {
            pass++;
            if (path == 1) via_onion++;
            else if (path == 2) via_scatter++;
            else via_residual++;
        } else {
            fail++;
            printf("MISMATCH: %s (path=%d)\n", name, path);
        }
        free(dec);
    }

    printf("\n=== VERIFY: %d PASS, %d FAIL (of %d sampled / %u tensors) "
           "[onion=%d scatter=%d residual=%d] ===\n",
           pass, fail, sample_count, n, via_onion, via_scatter, via_residual);
    tess_pack_close(&pi);
    gguf_close(&g);
    return fail > 0 ? 1 : 0;
}
