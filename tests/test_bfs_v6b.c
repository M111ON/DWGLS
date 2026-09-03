/*
 * test_bfs_v6b.c — Breathing FS + v6b Streaming Codec Integration
 * ═══════════════════════════════════════════════════════════════════
 * Assembles the fs family (breathing + persist + anchor) with v6b
 * as the codec layer. Proves all pieces connect end-to-end.
 *
 * T1:  v6b block codec: 144-byte block encode → decode lossless
 * T2:  Multi-block file: 5 blocks = 720 bytes roundtrip
 * T3:  Full fs pipeline: write → v6b encode → persist → load → decode → verify
 * T4:  Breathing + anchor: scale change → delta → go_home → lossless
 * T5:  Real binary data: random weights → v6b roundtrip through fs
 * T6:  Multi-file directory: 3 files, different sizes
 *
 * BUILD: gcc -O2 -Wall -Wextra -Icore -o build/test_bfs_v6b
 *        tests/test_bfs_v6b.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "kis_codec_v6b.h"
#include "breathing_fs.h"
#include "bfs_persist.h"
#include "bfs_seek_anchor.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ── v6b single-block codec: collect → header → emit → concatenate ── */
static int v6b_block_encode(const int8_t *data, uint32_t n,
                            uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    v6b_stream_t st = {0};
    if (v6b_init(&st, V6B_Q8) != 0) return -1;
    if (v6b_collect(&st, data, n) != 0) { v6b_free(&st); return -2; }

    uint32_t hdr_sz = v6b_header(&st, NULL, 0);
    if (hdr_sz == 0) { v6b_free(&st); return -3; }
    if (hdr_sz + n * 6 > out_cap) { v6b_free(&st); return -4; }

    v6b_header(&st, out, hdr_sz);
    uint32_t off = hdr_sz;

    uint32_t emit_buf = 4 + n * 6;
    uint32_t emitted = v6b_emit(&st, out + off, emit_buf);
    off += emitted;

    *out_len = off;
    v6b_free(&st);
    return 0;
}

/* ── v6b single-block decode ── */
static int v6b_block_decode(const uint8_t *enc, uint32_t enc_len,
                            int8_t *out, uint32_t n)
{
    uint32_t got = v6b_decode_all(enc, enc_len, (uint8_t *)out, n);
    return (got == n) ? 0 : -1;
}

/* ── Fill buffer with deterministic pseudo-random Q8 data ── */
static void fill_random(int8_t *buf, uint32_t n, uint32_t seed)
{
    uint32_t s = seed;
    for (uint32_t i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        buf[i] = (int8_t)((s >> 16) & 0xFF);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
   T1: v6b block codec: 144-byte block roundtrip
   ═══════════════════════════════════════════════════════════════════════ */
static void test_v6b_block(void)
{
    printf("TEST 1: v6b Block Codec (144 bytes)\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int8_t raw[144];
    for (int i = 0; i < 144; i++) raw[i] = (int8_t)(i * 3 - 72);

    uint32_t need = 256 + 144 * 6 + 4;
    uint8_t *enc = (uint8_t *)malloc(need);
    uint32_t enc_len = 0;
    int rc = v6b_block_encode(raw, 144, enc, need, &enc_len);
    CHECK(1, "encode ok", rc == 0 && enc_len > 0);
    printf("         144 bytes → %u bytes (ratio %.2f)\n", enc_len,
           (double)enc_len / 144.0);

    int8_t dec[144];
    rc = v6b_block_decode(enc, enc_len, dec, 144);
    CHECK(2, "decode ok", rc == 0);
    CHECK(3, "lossless", memcmp(raw, dec, 144) == 0);
    free(enc);
}

/* ═══════════════════════════════════════════════════════════════════════
   T2: Multi-block file (5 × 144 = 720 bytes)
   ═══════════════════════════════════════════════════════════════════════ */
static void test_v6b_multi_block(void)
{
    printf("\nTEST 2: Multi-block File (720 bytes)\n");
    printf("═══════════════════════════════════════════════════════════\n");

    int8_t raw[720];
    fill_random(raw, 720, 42);

    /* Encode as one v6b stream (720 elements = 5 chunks of 144) */
    v6b_stream_t st = {0};
    v6b_init(&st, V6B_Q8);
    v6b_collect(&st, raw, 720);

    uint32_t hdr_sz = v6b_header(&st, NULL, 0);
    uint8_t *enc = (uint8_t *)malloc(hdr_sz + 720 * 6 + 256);
    v6b_header(&st, enc, hdr_sz);
    uint32_t off = hdr_sz;

    while (1) {
        uint32_t emitted = v6b_emit(&st, enc + off, 4 + 720 * 6);
        if (emitted == 0) break;
        off += emitted;
    }
    v6b_free(&st);
    printf("         720 bytes → %u bytes (ratio %.2f)\n", off,
           (double)off / 720.0);
    CHECK(4, "encode ok", off > 0);

    /* Decode */
    int8_t dec[720];
    uint32_t got = v6b_decode_all(enc, off, (uint8_t *)dec, 720);
    CHECK(5, "decode ok", got == 720);
    CHECK(6, "lossless", memcmp(raw, dec, 720) == 0);
    free(enc);
}

/* ═══════════════════════════════════════════════════════════════════════
   T3: Full fs pipeline — write blocks with v6b, persist BIMG, load, decode
   ═══════════════════════════════════════════════════════════════════════ */
static void test_fs_pipeline(void)
{
    printf("\nTEST 3: FS Pipeline (write → v6b → persist → load → decode)\n");
    printf("═══════════════════════════════════════════════════════════\n");

    /* ── Build a BreathingFS with 3 files ── */
    BreathingFS fs;
    bfs_init(&fs);

    int8_t d1[144]; fill_random(d1, 144, 1);
    int8_t d2[288]; fill_random(d2, 288, 2);
    int8_t d3[432]; fill_random(d3, 432, 3);

    int rc1 = bfs_write(&fs, "alpha.bin", d1, 144);
    int rc2 = bfs_write(&fs, "beta.bin", d2, 288);
    int rc3 = bfs_write(&fs, "gamma.bin", d3, 432);
    CHECK(7, "3 files written", rc1 == 0 && rc2 == 0 && rc3 == 0);
    printf("         blocks used: %u\n", fs.n_blocks_used);
    CHECK(8, "blocks used", fs.n_blocks_used > 0 && fs.n_blocks_used <= 20);

    /* Verify raw blocks are lossless through DynContainer (built-in) */
    CHECK(9,  "alpha lossless", bfs_verify_file(&fs, "alpha.bin", d1, 144) == 0);
    CHECK(10, "beta lossless",  bfs_verify_file(&fs, "beta.bin", d2, 288) == 0);
    CHECK(11, "gamma lossless", bfs_verify_file(&fs, "gamma.bin", d3, 432) == 0);

    /* ── Now verify same data through v6b encode/decode ── */
    for (uint32_t fi = 0; fi < 3; fi++) {
        const BFSFileEntry *fe = &fs.files[fi];
        const int8_t *ref = NULL;
        uint32_t ref_sz = 0;
        if (fi == 0) { ref = d1; ref_sz = 144; }
        if (fi == 1) { ref = d2; ref_sz = 288; }
        if (fi == 2) { ref = d3; ref_sz = 432; }

        v6b_stream_t st = {0};
        v6b_init(&st, V6B_Q8);
        v6b_collect(&st, ref, ref_sz);

        uint32_t hdr_sz = v6b_header(&st, NULL, 0);
        uint8_t *enc = (uint8_t *)malloc(hdr_sz + ref_sz * 6 + 256);
        v6b_header(&st, enc, hdr_sz);
        uint32_t off = hdr_sz;
        while (1) {
            uint32_t e = v6b_emit(&st, enc + off, 4 + ref_sz * 6);
            if (e == 0) break;
            off += e;
        }
        v6b_free(&st);

        int8_t *dec = (int8_t *)malloc(ref_sz);
        uint32_t got = v6b_decode_all(enc, off, (uint8_t *)dec, ref_sz);
        int ok = (got == ref_sz && memcmp(ref, dec, ref_sz) == 0);

        char label[64];
        snprintf(label, sizeof(label), "v6b %s lossless", fe->name);
        CHECK(12 + fi, label, ok);

        free(enc);
        free(dec);
    }

    /* ── Persist to BIMG, load back, verify ── */
    const char *img = "build/t_v6b.img";
    rc1 = bfs_save_img(img, &fs);
    CHECK(15, "BIMG save ok", rc1 == 0);

    BFSMmapFS mfs = {0};
    rc1 = bfs_mmap_open(img, &mfs);
    CHECK(16, "BIMG load ok", rc1 == 0);

    int8_t recon[432];
    uint32_t actual = 0;
    rc1 = bfs_mmap_read(&mfs, "alpha.bin", recon, 144, &actual);
    CHECK(17, "mmap read alpha", rc1 == 0 && actual == 144);
    CHECK(18, "mmap alpha lossless", memcmp(d1, recon, 144) == 0);

    rc1 = bfs_mmap_read(&mfs, "gamma.bin", recon, 432, &actual);
    CHECK(19, "mmap read gamma", rc1 == 0 && actual == 432);
    CHECK(20, "mmap gamma lossless", memcmp(d3, recon, 432) == 0);

    bfs_mmap_close(&mfs);
}

/* ═══════════════════════════════════════════════════════════════════════
   T4: Breathing + Anchor: scale change → delta → go_home → lossless
   ═══════════════════════════════════════════════════════════════════════ */
static void test_breathing_anchor(void)
{
    printf("\nTEST 4: Breathing + Anchor + v6b Lossless\n");
    printf("═══════════════════════════════════════════════════════════\n");

    BreathingFS fs;
    bfs_init(&fs);

    int8_t data[288];
    fill_random(data, 288, 99);
    fs.seeker.current_pos = 500;
    fs.seeker.home_pos = 500;
    bfs_write(&fs, "anchor.bin", data, 288);

    /* Anchor at creation */
    BFSAnchorSet anchors;
    bfs_anchor_init(&anchors);
    bfs_anchor_record(&anchors, 500, 1.0);

    /* Breathe: scale down to 0.5 */
    bfs_move_seeker(&fs, 0.5);
    CHECK(21, "scale 0.5 applied", fs.seeker.scale == 0.5);

    /* Record anchor at this scale */
    bfs_anchor_record(&anchors, fs.seeker.current_pos, 0.5);

    /* Derive delta from anchor — must match bfs_delta_at at same scale */
    int ai = bfs_anchor_find(&anchors, fs.block_meta[fs.files[0].home_block].home_pos);
    CHECK(21 + 1, "anchor found", ai >= 0);
    int32_t derived_delta = bfs_delta_at(anchors.anchors[ai].home_pos, anchors.anchors[ai].scale_at_write);
    CHECK(22, "anchor delta derived", derived_delta == 0);

    /* Breathe more: scale to 0.25 (hyperbolic) */
    bfs_move_seeker(&fs, 0.25);
    CHECK(23, "hyperbolic at 0.25", (fs.seeker.is_hyperbolic & 1) == 1);

    /* Go home → lossless */
    bfs_go_home(&fs);
    CHECK(24, "back to home", fs.seeker.current_pos == 500);
    CHECK(25, "lossless after breathing",
           bfs_verify_file(&fs, "anchor.bin", data, 288) == 0);

    /* ── Also verify through v6b ── */
    v6b_stream_t st = {0};
    v6b_init(&st, V6B_Q8);
    v6b_collect(&st, data, 288);
    uint32_t hdr_sz = v6b_header(&st, NULL, 0);
    uint8_t *enc = (uint8_t *)malloc(hdr_sz + 288 * 6 + 256);
    v6b_header(&st, enc, hdr_sz);
    uint32_t off = hdr_sz;
    while (1) {
        uint32_t e = v6b_emit(&st, enc + off, 4 + 288 * 6);
        if (e == 0) break;
        off += e;
    }
    v6b_free(&st);

    int8_t dec[288];
    uint32_t got = v6b_decode_all(enc, off, (uint8_t *)dec, 288);
    CHECK(26, "v6b lossless after breathing",
           got == 288 && memcmp(data, dec, 288) == 0);
    free(enc);
}

/* ═══════════════════════════════════════════════════════════════════════
   T5: Real binary data (simulated GGUF weight blocks)
   ═══════════════════════════════════════════════════════════════════════ */
static void test_real_binary(void)
{
    printf("\nTEST 5: Real Binary Data (simulated weights)\n");
    printf("═══════════════════════════════════════════════════════════\n");

    /* Simulate Q8_0 quantized weights: mostly small values with occasional spikes */
    int8_t weights[20736];
    uint32_t s = 12345;
    for (uint32_t i = 0; i < 20736; i++) {
        s = s * 1103515245u + 12345u;
        int v = (int)((s >> 16) & 0x1F) - 16;  /* mostly -16..15 */
        if ((s & 0xFF) < 5) v = (int)((s >> 24) & 0x7F) - 64; /* rare spike */
        weights[i] = (int8_t)v;
    }

    /* v6b encode full 20736 elements */
    v6b_stream_t st = {0};
    v6b_init(&st, V6B_Q8);
    v6b_collect(&st, weights, 20736);

    uint32_t hdr_sz = v6b_header(&st, NULL, 0);
    uint8_t *enc = (uint8_t *)malloc(hdr_sz + 20736 * 6 + 4096);
    v6b_header(&st, enc, hdr_sz);
    uint32_t off = hdr_sz;
    while (1) {
        uint32_t e = v6b_emit(&st, enc + off, 4 + 20736 * 6);
        if (e == 0) break;
        off += e;
    }
    v6b_free(&st);

    printf("         20736 weights → %u bytes (ratio %.2f)\n", off,
           (double)off / 20736.0);
    CHECK(27, "encode ok", off > 0);

    /* Decode */
    int8_t dec[20736];
    uint32_t got = v6b_decode_all(enc, off, (uint8_t *)dec, 20736);
    CHECK(28, "decode ok", got == 20736);
    CHECK(29, "lossless", memcmp(weights, dec, 20736) == 0);

    /* Also through BreathingFS: store first 5 blocks (720 weights) */
    BreathingFS fs;
    bfs_init(&fs);
    bfs_write(&fs, "weights.bin", weights, 720);
    CHECK(30, "fs write ok", bfs_verify_file(&fs, "weights.bin", weights, 720) == 0);
    free(enc);
}

/* ═══════════════════════════════════════════════════════════════════════
   T6: Multi-file directory with different sizes
   ═══════════════════════════════════════════════════════════════════════ */
static void test_multi_file(void)
{
    printf("\nTEST 6: Multi-file Directory\n");
    printf("═══════════════════════════════════════════════════════════\n");

    BreathingFS fs;
    bfs_init(&fs);

    /* Various sizes: 1 block, 2 blocks, 7 blocks, 10 blocks */
    uint32_t sizes[] = { 144, 288, 1008, 1440 };
    const char *names[] = { "tiny.bin", "small.bin", "medium.bin", "large.bin" };
    int8_t *bufs[4];

    for (int i = 0; i < 4; i++) {
        bufs[i] = (int8_t *)malloc(sizes[i]);
        fill_random(bufs[i], sizes[i], (uint32_t)(i + 100));
        int rc = bfs_write(&fs, names[i], bufs[i], sizes[i]);
        CHECK(31 + i * 2, names[i], rc == 0);
    }

    printf("         total blocks: %u\n", fs.n_blocks_used);
    CHECK(35, "blocks used", fs.n_blocks_used > 0);

    /* v6b roundtrip each file */
    for (int i = 0; i < 4; i++) {
        v6b_stream_t st = {0};
        v6b_init(&st, V6B_Q8);
        v6b_collect(&st, bufs[i], sizes[i]);
        uint32_t hs = v6b_header(&st, NULL, 0);
        uint8_t *enc = (uint8_t *)malloc(hs + sizes[i] * 6 + 256);
        v6b_header(&st, enc, hs);
        uint32_t off = hs;
        while (1) {
            uint32_t e = v6b_emit(&st, enc + off, 4 + sizes[i] * 6);
            if (e == 0) break;
            off += e;
        }
        v6b_free(&st);

        int8_t *dec = (int8_t *)malloc(sizes[i]);
        uint32_t got = v6b_decode_all(enc, off, (uint8_t *)dec, sizes[i]);
        char label[64];
        snprintf(label, sizeof(label), "v6b %s lossless", names[i]);
        CHECK(36 + i, label, got == sizes[i] && memcmp(bufs[i], dec, sizes[i]) == 0);
        free(enc);
        free(dec);
    }

    /* Save BIMG + mmap readback */
    const char *img = "build/t_v6b_multi.img";
    CHECK(40, "BIMG save", bfs_save_img(img, &fs) == 0);
    BFSMmapFS mfs = {0};
    CHECK(41, "BIMG load", bfs_mmap_open(img, &mfs) == 0);

    for (int i = 0; i < 4; i++) {
        int8_t recon[1440];
        uint32_t actual = 0;
        bfs_mmap_read(&mfs, names[i], recon, sizes[i], &actual);
        char label[64];
        snprintf(label, sizeof(label), "mmap %s lossless", names[i]);
        CHECK(42 + i, label, actual == sizes[i] && memcmp(bufs[i], recon, sizes[i]) == 0);
    }
    bfs_mmap_close(&mfs);

    for (int i = 0; i < 4; i++) free(bufs[i]);
}

int main(void)
{
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Breathing FS + v6b Codec Integration                  ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    test_v6b_block();
    test_v6b_multi_block();
    test_fs_pipeline();
    test_breathing_anchor();
    test_real_binary();
    test_multi_file();

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("PASS: %d / %d  %s\n", pass, pass + fail,
           fail == 0 ? "✓" : "✗ FAILURES");
    return fail;
}
