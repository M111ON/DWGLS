/* test_codec_tess.c — DWGLS TESS Codec Adapter Tests
 *
 * BUILD: gcc -O2 -Wall -Wextra -Icore -o build/test_codec_tess.exe tests/test_codec_tess.c -lm
 * RUN:   build/test_codec_tess.exe
 *
 * Tests:
 *   T1: info() returns correct metadata
 *   T2: encode/decode roundtrip (uint8_t, cell=1)
 *   T3: encode/decode roundtrip (cell=4, Q8_0-ish)
 *   T4: payload_size matches actual encode output
 *   T5: verify() detects corrupted payload
 *   T6: resolve() = stride_scatter for all 20736 slots
 *   T7: encode rejects n_elems > 20736
 *   T8: shell_from_header roundtrip
 *   T9: resolve_octant = scatter(resolve_octant_raw(slot))
 *  T10: vtable is properly populated (all function pointers non-NULL)
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "dwgls_codec.h"
#include "dwgls_codec_tess.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { printf("  T%02d %-44s ", __COUNTER__ + 1, name); } while(0)

#define PASS() \
    do { printf("PASS\n"); tests_passed++; } while(0)

#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

/* ── T1: info() ─────────────────────────────────────────────── */
static void test_info(void)
{
    TEST("info() returns correct metadata");
    DWGLS_CodecInfo info = DWGLS_CODEC_TESS.info();
    if (info.codec_id != CODEC_TESS) { FAIL("codec_id"); return; }
    if (strcmp(info.name, "tess") != 0) { FAIL("name"); return; }
    if (info.min_version != 1) { FAIL("min_version"); return; }
    if (!(info.flags & CODEC_FLAG_RANDOM_ACCESS)) { FAIL("missing RANDOM_ACCESS flag"); return; }
    if (!(info.flags & CODEC_FLAG_DERIVED_VIEWS)) { FAIL("missing DERIVED_VIEWS flag"); return; }
    PASS();
}

/* ── T2: encode/decode roundtrip (cell=1) ───────────────────── */
static void test_roundtrip_cell1(void)
{
    TEST("encode/decode roundtrip (cell=1)");
    DWGLS_CodecCtx ctx = dwgls_ctx_default(65536);
    TESS_CTX_CELL_SIZE(&ctx) = 1;
    TESS_CTX_GGUF_TYPE(&ctx) = TESS_GGML_Q8_0;
    TESS_CTX_OCTANT_MASK(&ctx) = 0xFF;

    uint32_t n = 256;
    uint8_t src[256];
    for (uint32_t i = 0; i < n; i++) src[i] = (uint8_t)(i * 37 + 13);

    uint8_t payload[64 + 256 + 64 + 8]; /* formula + cubedata + padding + CRC */
    uint32_t cap = sizeof(payload);

    int32_t written = DWGLS_CODEC_TESS.encode(src, n, &ctx, payload, cap);
    if (written < 0) { FAIL("encode returned negative"); return; }
    if ((uint32_t)written != 64 + n + 8) { FAIL("encode size mismatch"); return; }

    uint8_t dst[256];
    int32_t decoded = DWGLS_CODEC_TESS.decode(payload, (uint32_t)written, &ctx, dst, sizeof(dst));
    if (decoded < 0) { FAIL("decode returned negative"); return; }
    if ((uint32_t)decoded != n) { FAIL("decoded size mismatch"); return; }

    for (uint32_t i = 0; i < n; i++) {
        if (dst[i] != src[i]) {
            FAIL("data mismatch"); return;
        }
    }
    PASS();
}

/* ── T3: encode/decode roundtrip (cell=4) ───────────────────── */
static void test_roundtrip_cell4(void)
{
    TEST("encode/decode roundtrip (cell=4)");
    DWGLS_CodecCtx ctx = dwgls_ctx_default(65536);
    TESS_CTX_CELL_SIZE(&ctx) = 4;

    uint32_t n = 64;
    uint32_t src[64];
    for (uint32_t i = 0; i < n; i++) src[i] = 0xDEAD0000u + i;

    uint32_t payload[64/4 + 64 + 32]; /* formula in uint32_t units + cubedata + CRC */
    uint32_t cap = sizeof(payload) * sizeof(uint32_t);

    uint8_t *pb = (uint8_t *)payload;
    int32_t written = DWGLS_CODEC_TESS.encode(src, n, &ctx, pb, cap);
    if (written < 0) { FAIL("encode returned negative"); return; }

    uint32_t dst[64];
    int32_t decoded = DWGLS_CODEC_TESS.decode(pb, (uint32_t)written, &ctx, dst, sizeof(dst));
    if (decoded < 0) { FAIL("decode returned negative"); return; }

    for (uint32_t i = 0; i < n; i++) {
        if (dst[i] != src[i]) { FAIL("data mismatch"); return; }
    }
    PASS();
}

/* ── T4: payload_size matches encode ────────────────────────── */
static void test_payload_size(void)
{
    TEST("payload_size() matches encode output");
    DWGLS_CodecCtx ctx = dwgls_ctx_default(65536);
    TESS_CTX_CELL_SIZE(&ctx) = 1;

    uint32_t n = 1024;
    uint32_t expected = DWGLS_CODEC_TESS.payload_size(n, &ctx);
    uint32_t expected_formula = (uint32_t)sizeof(TESS_Formula);
    if (expected != expected_formula + n * 1 + 8) {
        FAIL("payload_size formula mismatch"); return;
    }

    /* Actually encode and compare */
    uint8_t src[256];
    memset(src, 0xAA, 256);
    uint8_t payload[64 + 256 + 8];
    int32_t written = DWGLS_CODEC_TESS.encode(src, 256, &ctx, payload, sizeof(payload));
    uint32_t actual = DWGLS_CODEC_TESS.payload_size(256, &ctx);
    if ((uint32_t)written != actual) { FAIL("written != payload_size"); return; }
    PASS();
}

/* ── T5: verify detects corruption ──────────────────────────── */
static void test_verify_corruption(void)
{
    TEST("verify() detects corrupted payload");
    DWGLS_CodecCtx ctx = dwgls_ctx_default(65536);
    TESS_CTX_CELL_SIZE(&ctx) = 1;

    uint8_t src[128];
    memset(src, 0x42, 128);
    uint8_t payload[64 + 128 + 8];
    int32_t written = DWGLS_CODEC_TESS.encode(src, 128, &ctx, payload, sizeof(payload));
    if (written < 0) { FAIL("encode failed"); return; }

    /* Verify before corruption */
    int v = DWGLS_CODEC_TESS.verify(payload, (uint32_t)written);
    if (v != 0) { FAIL("verify failed on clean payload"); return; }

    /* Corrupt a byte in cubedata */
    payload[70] ^= 0xFF;
    v = DWGLS_CODEC_TESS.verify(payload, (uint32_t)written);
    if (v == 0) { FAIL("verify did not detect corruption"); return; }

    PASS();
}

/* ── T6: resolve() matches stride_scatter ───────────────────── */
static void test_resolve(void)
{
    TEST("resolve() = stride_scatter for all 20736 slots");
    DWGLS_CodecCtx ctx = dwgls_ctx_default(65536);
    for (uint32_t slot = 0; slot < TESS_TOTAL_SLOTS; slot++) {
        uint32_t r = DWGLS_CODEC_TESS.resolve(slot, &ctx);
        uint32_t expected = tess_stride_scatter(slot);
        if (r != expected) {
            FAIL("mismatch at slot"); return;
        }
    }
    PASS();
}

/* ── T7: encode rejects n_elems > 20736 ────────────────────── */
static void test_encode_overflow(void)
{
    TEST("encode rejects n_elems > 20736");
    DWGLS_CodecCtx ctx = dwgls_ctx_default(65536);
    TESS_CTX_CELL_SIZE(&ctx) = 1;
    uint8_t buf[64];
    int32_t r = DWGLS_CODEC_TESS.encode(buf, 20737, &ctx, buf, 64);
    if (r != -2) { FAIL("expected -2 overflow error"); return; }
    PASS();
}

/* ── T8: shell_from_header roundtrip ────────────────────────── */
static void test_shell_header_roundtrip(void)
{
    TEST("shell ↔ header conversion roundtrip");
    TESS_Header th;
    tess_header_init(&th, TESS_GGML_Q8_0, TESS_CELL_Q8_0);

    DWGLS_Shell s;
    tess_shell_from_header(&s, &th);

    if (s.magic != DWGLS_SHELL_MAGIC) { FAIL("magic"); return; }
    if (s.codec_id != CODEC_TESS) { FAIL("codec_id"); return; }
    if (s.total_slots != 20736) { FAIL("total_slots"); return; }
    if (s.cell_size != TESS_CELL_Q8_0) { FAIL("cell_size"); return; }

    /* Reverse */
    TESS_Header th2;
    tess_header_from_shell(&th2, &s, TESS_GGML_Q8_0);
    if (th2.magic != TESS_MAGIC) { FAIL("th2.magic"); return; }
    if (th2.total_slots != 20736) { FAIL("th2.total_slots"); return; }
    if (th2.cell_size != TESS_CELL_Q8_0) { FAIL("th2.cell_size"); return; }

    PASS();
}

/* ── T9: resolve_octant = scatter(resolve_octant_raw) ──────── */
static void test_resolve_octant(void)
{
    TEST("resolve_octant = scatter(resolve_octant_raw)");
    TESS_Header h;
    tess_header_init(&h, TESS_GGML_F32, TESS_CELL_F32);

    /* Test a sample of slots and all octants */
    uint32_t slots[] = {0, 1, 100, 6911, 6912, 13823, 13824, 20735};
    for (int si = 0; si < 8; si++) {
        for (uint8_t oct = 0; oct < 8; oct++) {
            uint32_t expected = tess_stride_scatter(
                tess_resolve_octant(slots[si], oct, &h));
            uint32_t actual = tess_codec_resolve_octant(slots[si], oct, &h);
            if (actual != expected) { FAIL("mismatch"); return; }
        }
    }
    PASS();
}

/* ── T10: vtable populated ──────────────────────────────────── */
static void test_vtable_populated(void)
{
    TEST("vtable has all function pointers set");
    if (!DWGLS_CODEC_TESS.info) { FAIL("info"); return; }
    if (!DWGLS_CODEC_TESS.encode) { FAIL("encode"); return; }
    if (!DWGLS_CODEC_TESS.decode) { FAIL("decode"); return; }
    if (!DWGLS_CODEC_TESS.payload_size) { FAIL("payload_size"); return; }
    if (!DWGLS_CODEC_TESS.verify) { FAIL("verify"); return; }
    if (!DWGLS_CODEC_TESS.resolve) { FAIL("resolve"); return; }
    PASS();
}

/* ════════════════════════════════════════════════════════════════
   MAIN
   ════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("═══ DWGLS TESS Codec Adapter Tests ═══\n\n");

    test_info();
    test_roundtrip_cell1();
    test_roundtrip_cell4();
    test_payload_size();
    test_verify_corruption();
    test_resolve();
    test_encode_overflow();
    test_shell_header_roundtrip();
    test_resolve_octant();
    test_vtable_populated();

    printf("\n═══ Results: %d PASS, %d FAIL ═══\n",
           tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
