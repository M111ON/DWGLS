/*
 * tests/test_codec_tess.c — TESS Codec roundtrip tests
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * T1  tess_info — codec info correct
 * T2  tess_payload_size — correct size computation
 * T3  tess_encode + tess_decode roundtrip (F32)
 * T4  tess_encode + tess_decode roundtrip (F16/BF16 cell size 2)
 * T5  tess_encode + tess_decode roundtrip (Q8_0 cell size 34)
 * T6  tess_verify — valid payload passes
 * T5  tess_verify — corrupt payload fails
 * T6  tess_resolve — stride-37 scatter matches gather inverse
 * T7  full DWGLS file: shell + codec payload roundtrip
 * T8  mmap-friendly: payload pointer arithmetic works
 * T9  codec registry finds TESS
 * T10 multiple encode/decode cycles produce identical results
 *
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -o build/test_codec_tess tests/test_codec_tess.c -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "../core/dwgls_shell.h"
#include "../core/dwgls_codec.h"
#include "../core/codec_tess.h"
#include "../core/geo_tess_container.h"

static int pass = 0, fail = 0;

#define CHECK(desc, cond) do { \
    if (cond) { pass++; printf("  [PASS] %s\n", desc); } \
    else { fail++; printf("  [FAIL] %s\n", desc); } \
} while(0)

/* Deterministic test data generator */
static void fill_test_data(void *buf, uint32_t n_elems, uint32_t cell_size, uint32_t seed)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t s = seed;
    for (uint32_t i = 0; i < n_elems * cell_size; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        p[i] = (uint8_t)(s & 0xFF);
    }
}

static int memcmp_data(const void *a, const void *b, uint32_t n)
{
    return memcmp(a, b, n);
}

int main(void)
{
    printf("═══════════════════════════════════════\n");
    printf("  TESS Codec Tests\n");
    printf("═══════════════════════════════════════\n\n");

    const uint32_t TOTAL_SLOTS = 20736;
    const uint32_t N_ELEMS = 1000;  /* test with subset */

    /* ── T1: tess_info ─────────────────────────────────────────────── */
    {
        DWGLS_CodecInfo info = tess_info();
        CHECK("T1a: name is 'tess'", strcmp(info.name, "tess") == 0);
        CHECK("T1b: codec_id is CODEC_TESS", info.codec_id == CODEC_TESS);
        CHECK("T1c: min_version is 1", info.min_version == 1);
        CHECK("T1d: has MMAP_FRIENDLY flag", info.flags & CODEC_FLAG_MMAP_FRIENDLY);
        CHECK("T1e: has RANDOM_ACCESS flag", info.flags & CODEC_FLAG_RANDOM_ACCESS);
        CHECK("T1f: has DERIVED_VIEWS flag", info.flags & CODEC_FLAG_DERIVED_VIEWS);
    }

    /* ── T2: tess_payload_size ─────────────────────────────────────── */
    {
        DWGLS_CodecCtx ctx = { .total_slots = TOTAL_SLOTS, .scale_factor = 65536 };
        ctx.user_data[0] = TESS_CELL_F32;  /* 4 bytes */
        ctx.user_data[1] = TESS_GGML_F32;

        uint32_t sz = tess_payload_size(N_ELEMS, &ctx);
        uint32_t expected = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + (TOTAL_SLOTS * 4) + TESS_CRC_SIZE;
        CHECK("T2a: payload size F32", sz == expected);

        ctx.user_data[0] = TESS_CELL_F16;  /* 2 bytes */
        sz = tess_payload_size(N_ELEMS, &ctx);
        expected = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + (TOTAL_SLOTS * 2) + TESS_CRC_SIZE;
        CHECK("T2b: payload size F16", sz == expected);

        ctx.user_data[0] = TESS_CELL_Q8_0;  /* 34 bytes */
        sz = tess_payload_size(N_ELEMS, &ctx);
        expected = TESS_HEADER_SIZE + TESS_FORMULA_SIZE + (TOTAL_SLOTS * 34) + TESS_CRC_SIZE;
        CHECK("T2c: payload size Q8_0", sz == expected);
    }

    /* ── T3: encode + decode roundtrip F32 ─────────────────────────── */
    {
        DWGLS_CodecCtx ctx = { .total_slots = TOTAL_SLOTS, .scale_factor = 65536 };
        ctx.user_data[0] = TESS_CELL_F32;
        ctx.user_data[1] = TESS_GGML_F32;
        ctx.user_data[2] = N_ELEMS;
        ctx.x_slots = TESS_X_SLOTS;
        ctx.y_slots = TESS_Y_SLOTS;
        ctx.z_slots = TESS_Z_SLOTS;

        uint32_t payload_sz = tess_payload_size(N_ELEMS, &ctx);
        uint8_t *src = (uint8_t *)malloc(N_ELEMS * 4);
        uint8_t *payload = (uint8_t *)malloc(payload_sz);
        uint8_t *dst = (uint8_t *)malloc(N_ELEMS * 4);

        fill_test_data(src, N_ELEMS, 4, 0xABCDEF01);

        int32_t enc = tess_encode(src, N_ELEMS, &ctx, payload, payload_sz);
        CHECK("T3a: encode returns positive bytes", enc > 0);
        CHECK("T3b: encode size matches payload_size", (uint32_t)enc == payload_sz);

        int32_t dec = tess_decode(payload, enc, &ctx, dst, N_ELEMS * 4);
        CHECK("T3c: decode returns positive bytes", dec > 0);
        CHECK("T3d: decode size matches", (uint32_t)dec == N_ELEMS * 4);
        CHECK("T3e: roundtrip data identical", memcmp_data(src, dst, N_ELEMS * 4) == 0);

        free(src); free(payload); free(dst);
    }

    /* ── T4: encode + decode roundtrip F16 (cell_size=2) ───────────── */
    {
        DWGLS_CodecCtx ctx = { .total_slots = TOTAL_SLOTS, .scale_factor = 65536 };
        ctx.user_data[0] = TESS_CELL_F16;
        ctx.user_data[1] = TESS_GGML_F16;
        ctx.user_data[2] = N_ELEMS;
        ctx.x_slots = TESS_X_SLOTS;
        ctx.y_slots = TESS_Y_SLOTS;
        ctx.z_slots = TESS_Z_SLOTS;

        uint32_t payload_sz = tess_payload_size(N_ELEMS, &ctx);
        uint8_t *src = (uint8_t *)malloc(N_ELEMS * 2);
        uint8_t *payload = (uint8_t *)malloc(payload_sz);
        uint8_t *dst = (uint8_t *)malloc(N_ELEMS * 2);

        fill_test_data(src, N_ELEMS, 2, 0x12345678);

        int32_t enc = tess_encode(src, N_ELEMS, &ctx, payload, payload_sz);
        CHECK("T4a: encode F16", enc > 0);

        int32_t dec = tess_decode(payload, enc, &ctx, dst, N_ELEMS * 2);
        CHECK("T4b: decode F16", dec > 0);
        CHECK("T4c: roundtrip F16 identical", memcmp_data(src, dst, N_ELEMS * 2) == 0);

        free(src); free(payload); free(dst);
    }

    /* ── T5: encode + decode roundtrip Q8_0 (cell_size=34) ─────────── */
    {
        DWGLS_CodecCtx ctx = { .total_slots = TOTAL_SLOTS, .scale_factor = 65536 };
        ctx.user_data[0] = TESS_CELL_Q8_0;
        ctx.user_data[1] = TESS_GGML_Q8_0;
        ctx.user_data[2] = N_ELEMS;
        ctx.x_slots = TESS_X_SLOTS;
        ctx.y_slots = TESS_Y_SLOTS;
        ctx.z_slots = TESS_Z_SLOTS;

        uint32_t payload_sz = tess_payload_size(N_ELEMS, &ctx);
        uint8_t *src = (uint8_t *)malloc(N_ELEMS * 34);
        uint8_t *payload = (uint8_t *)malloc(payload_sz);
        uint8_t *dst = (uint8_t *)malloc(N_ELEMS * 34);

        fill_test_data(src, N_ELEMS, 34, 0xFEEDFACE);

        int32_t enc = tess_encode(src, N_ELEMS, &ctx, payload, payload_sz);
        CHECK("T5a: encode Q8_0", enc > 0);

        int32_t dec = tess_decode(payload, enc, &ctx, dst, N_ELEMS * 34);
        CHECK("T5b: decode Q8_0", dec > 0);
        CHECK("T5c: roundtrip Q8_0 identical", memcmp_data(src, dst, N_ELEMS * 34) == 0);

        free(src); free(payload); free(dst);
    }

    /* ── T6: tess_verify valid payload ─────────────────────────────── */
    {
        DWGLS_CodecCtx ctx = { .total_slots = TOTAL_SLOTS, .scale_factor = 65536 };
        ctx.user_data[0] = TESS_CELL_F32;
        ctx.user_data[1] = TESS_GGML_F32;
        ctx.user_data[2] = N_ELEMS;
        ctx.x_slots = TESS_X_SLOTS;
        ctx.y_slots = TESS_Y_SLOTS;
        ctx.z_slots = TESS_Z_SLOTS;

        uint32_t payload_sz = tess_payload_size(N_ELEMS, &ctx);
        uint8_t *src = (uint8_t *)malloc(N_ELEMS * 4);
        uint8_t *payload = (uint8_t *)malloc(payload_sz);

        fill_test_data(src, N_ELEMS, 4, 0xBADF00D);
        tess_encode(src, N_ELEMS, &ctx, payload, payload_sz);

        int v = tess_verify(payload, payload_sz);
        CHECK("T6: verify passes on valid payload", v == 0);

        free(src); free(payload);
    }

    /* ── T7: tess_verify corrupt payload fails ─────────────────────── */
    {
        DWGLS_CodecCtx ctx = { .total_slots = TOTAL_SLOTS, .scale_factor = 65536 };
        ctx.user_data[0] = TESS_CELL_F32;
        ctx.user_data[1] = TESS_GGML_F32;
        ctx.user_data[2] = N_ELEMS;
        ctx.x_slots = TESS_X_SLOTS;
        ctx.y_slots = TESS_Y_SLOTS;
        ctx.z_slots = TESS_Z_SLOTS;

        uint32_t payload_sz = tess_payload_size(N_ELEMS, &ctx);
        uint8_t *src = (uint8_t *)malloc(N_ELEMS * 4);
        uint8_t *payload = (uint8_t *)malloc(payload_sz);

        fill_test_data(src, N_ELEMS, 4, 0xDEADBEEF);
        tess_encode(src, N_ELEMS, &ctx, payload, payload_sz);

        /* Corrupt the CubeData (after header + formula) */
        payload[TESS_HEADER_SIZE + TESS_FORMULA_SIZE + 10] ^= 0xFF;

        int v = tess_verify(payload, payload_sz);
        CHECK("T7: verify fails on corrupt payload", v == -3);

        free(src); free(payload);
    }

    /* ── T8: tess_resolve stride-37 scatter/gather ─────────────────── */
    {
        DWGLS_CodecCtx ctx = { .total_slots = TOTAL_SLOTS };

        /* Test that scatter and gather are inverses: gather(scatter(i)) == i */
        int all_ok = 1;
        for (uint32_t i = 0; i < N_ELEMS && all_ok; i++) {
            uint32_t scattered = tess_stride_scatter(i);
            uint32_t gathered = tess_stride_gather(scattered);
            if (gathered != i % TOTAL_SLOTS) {
                all_ok = 0;
            }
        }
        CHECK("T8a: stride-37 scatter/gather inverse (first N_ELEMS)", all_ok);

        /* Test resolve function */
        uint32_t r = codec_tess_resolve(100, &ctx);
        CHECK("T8b: resolve returns scatter index", r == tess_stride_scatter(100));
    }

    /* ── T9: full DWGLS file: shell + codec payload roundtrip ──────── */
    {
        DWGLS_CodecCtx ctx = { .total_slots = TOTAL_SLOTS, .scale_factor = 65536 };
        ctx.user_data[0] = TESS_CELL_F32;
        ctx.user_data[1] = TESS_GGML_F32;
        ctx.user_data[2] = N_ELEMS;
        ctx.x_slots = TESS_X_SLOTS;
        ctx.y_slots = TESS_Y_SLOTS;
        ctx.z_slots = TESS_Z_SLOTS;

        uint32_t payload_sz = tess_payload_size(N_ELEMS, &ctx);
        uint32_t file_sz = DWGLS_SHELL_SZ + payload_sz;

        uint8_t *file_buf = (uint8_t *)malloc(file_sz);
        uint8_t *src = (uint8_t *)malloc(N_ELEMS * 4);
        uint8_t *dst = (uint8_t *)malloc(N_ELEMS * 4);

        fill_test_data(src, N_ELEMS, 4, 0xCAFEBABE);

        /* Build file: shell + codec payload */
        DWGLS_Shell *shell = (DWGLS_Shell *)file_buf;
        uint8_t *payload = file_buf + DWGLS_SHELL_SZ;

        dwgls_shell_init(shell, CODEC_TESS, TOTAL_SLOTS, 65536, payload_sz, 4, INTEGRITY_CRC64);
        tess_encode(src, N_ELEMS, &ctx, payload, payload_sz);

        /* Compute and set checksum */
        uint64_t crc = dwgls_shell_compute_checksum(shell, payload);
        shell->checksum = crc;

        /* Verify file integrity */
        int v = dwgls_shell_verify_integrity(shell, payload, payload_sz);
        CHECK("T9a: full file verify passes", v == 0);

        /* Decode through shell + codec */
        const DWGLS_CodecVtable *codec = dwgls_codec_find(shell->codec_id);
        CHECK("T9b: codec found", codec != NULL);

        v = codec->verify(payload, payload_sz);
        CHECK("T9c: codec verify passes", v == 0);

        int32_t dec = codec->decode(payload, payload_sz, &ctx, dst, N_ELEMS * 4);
        CHECK("T9d: decode through shell works", dec > 0);
        CHECK("T9e: full file roundtrip identical", memcmp_data(src, dst, N_ELEMS * 4) == 0);

        free(file_buf); free(src); free(dst);
    }

    /* ── T10: mmap-friendly payload pointer arithmetic ─────────────── */
    {
        DWGLS_CodecCtx ctx = { .total_slots = TOTAL_SLOTS, .scale_factor = 65536 };
        ctx.user_data[0] = TESS_CELL_F32;
        ctx.user_data[1] = TESS_GGML_F32;
        ctx.user_data[2] = N_ELEMS;
        ctx.x_slots = TESS_X_SLOTS;
        ctx.y_slots = TESS_Y_SLOTS;
        ctx.z_slots = TESS_Z_SLOTS;

        uint32_t payload_sz = tess_payload_size(N_ELEMS, &ctx);
        uint8_t *payload = (uint8_t *)malloc(payload_sz);

        uint8_t *src = (uint8_t *)malloc(N_ELEMS * 4);
        fill_test_data(src, N_ELEMS, 4, 0x11223344);
        tess_encode(src, N_ELEMS, &ctx, payload, payload_sz);

        /* Direct pointer access to CubeData (after header + formula) */
        const TESS_Header *hdr = (const TESS_Header *)(payload);
        const uint8_t *cube_data = payload + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;

        CHECK("T10a: cube_data pointer aligned", ((uintptr_t)cube_data) % 8 == 0);
        CHECK("T10b: header accessible", hdr->magic == TESS_MAGIC);
        CHECK("T10c: cell_size correct", hdr->cell_size == 4);
        CHECK("T10d: total_slots correct", hdr->total_slots == TOTAL_SLOTS);

        free(payload); free(src);
    }

    /* ── T11: codec registry finds TESS ────────────────────────────── */
    {
        const DWGLS_CodecVtable *c = dwgls_codec_find(CODEC_TESS);
        CHECK("T11a: codec_find TESS returns non-NULL", c != NULL);
        CHECK("T11b: codec info matches", c->info().codec_id == CODEC_TESS);
        CHECK("T11c: encode function present", c->encode != NULL);
        CHECK("T11d: decode function present", c->decode != NULL);
        CHECK("T11e: verify function present", c->verify != NULL);
        CHECK("T11f: resolve function present", c->resolve != NULL);
    }

    /* ── T12: multiple encode/decode cycles ────────────────────────── */
    {
        DWGLS_CodecCtx ctx = { .total_slots = TOTAL_SLOTS, .scale_factor = 65536 };
        ctx.user_data[0] = TESS_CELL_F32;
        ctx.user_data[1] = TESS_GGML_F32;
        ctx.user_data[2] = N_ELEMS;
        ctx.x_slots = TESS_X_SLOTS;
        ctx.y_slots = TESS_Y_SLOTS;
        ctx.z_slots = TESS_Z_SLOTS;

        uint32_t payload_sz = tess_payload_size(N_ELEMS, &ctx);
        uint8_t *src = (uint8_t *)malloc(N_ELEMS * 4);
        uint8_t *payload = (uint8_t *)malloc(payload_sz);
        uint8_t *dst = (uint8_t *)malloc(N_ELEMS * 4);

        fill_test_data(src, N_ELEMS, 4, 0x55AA55AA);

        /* Encode → decode → encode → decode cycle */
        for (int cycle = 0; cycle < 3; cycle++) {
            int32_t enc = tess_encode(src, N_ELEMS, &ctx, payload, payload_sz);
            CHECK("T12a: encode cycle", enc > 0);

            int32_t dec = tess_decode(payload, enc, &ctx, dst, N_ELEMS * 4);
            CHECK("T12b: decode cycle", dec > 0);
            CHECK("T12c: cycle data identical", memcmp_data(src, dst, N_ELEMS * 4) == 0);

            /* Use decoded as next source */
            memcpy(src, dst, N_ELEMS * 4);
        }

        free(src); free(payload); free(dst);
    }

    printf("\n═══════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass, pass + fail);
    return fail ? 1 : 0;
}