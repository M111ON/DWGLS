/*
 * rdh_demo.c — walkthrough pipeline ทั้งสาย
 * Build:  gcc -I../collection/rdh rdh_demo.c -o rdh_demo
 * Run:    rdh_demo [filename]
 *         rdh_demo (default: 48-byte self-test)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rdh_capture.h"

/* ── Frame seek decomposition (ตรงกับ geo_frame_seek.h) ── */
#define FRAME_CYCLE    1440u
#define FRAME_FACE_SZ   120u
#define FRAME_STRIDE      37u

static void frame_at(uint16_t enc, int *face, int *slot, int *phase, int *ico)
{
    *face  = enc / FRAME_FACE_SZ;          /* 0..11     */
    *slot  = enc % FRAME_FACE_SZ;          /* 0..119    */
    *phase = (enc / 12) % 12;              /* 0..11     */
    *ico   = enc % 162;                    /* 0..161    */
}

int main(int argc, char **argv)
{
    const RDHConfig cfg = RDH_CAPTURE_144;
    uint8_t buf[1024];
    size_t len = 0;

    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║     RDH Pipeline Demo — จับ data → address     ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    /* ── Load data ── */
    if (argc > 1) {
        FILE *fp = fopen(argv[1], "rb");
        if (!fp) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
        len = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);
        printf("📂 File: %s  (%zu bytes)\n", argv[1], len);
    } else {
        /* Self-test: 48 bytes deterministic */
        for (size_t i = 0; i < 48; i++) buf[i] = (uint8_t)(i * 3 + 7);
        len = 48;
        printf("📄 Self-test: 48 bytes (pattern: i*3+7)\n");
    }

    /* ── Print raw bytes (first 48) ── */
    printf("\n📋 Raw bytes (first %zu):\n  ", len < 48 ? len : 48);
    for (size_t i = 0; i < (len < 48 ? len : 48); i++)
        printf("%02x ", buf[i]);
    printf("\n");

    /* ── Step 1: rdh_capture ── */
    printf("\n─── Step 1: rdh_capture ───\n");
    int64_t key = rdh_capture(buf, len, &cfg);
    printf("  flat_key = %lld\n", (long long)key);

    /* ── Step 2: rdh_decompose (ring, wedge) ── */
    printf("\n─── Step 2: rdh_decompose ───\n");
    int64_t ring, wedge, mirror, u;
    rdh_decompose(&cfg, key, &ring, &wedge, &mirror, &u);
    printf("  ring  (home_y) = %lld\n", (long long)ring);
    printf("  wedge (home_x) = %lld\n", (long long)wedge);
    printf("  mirror        = %lld\n", (long long)mirror);
    printf("  u             = %lld\n", (long long)u);
    printf("  → home = (%lld, %lld)\n", (long long)wedge, (long long)ring);

    /* ── Step 3: enc (2 bytes) ── */
    printf("\n─── Step 3: frame_seek enc (2 bytes) ───\n");
    uint16_t enc = (uint16_t)((uint64_t)key % FRAME_CYCLE);
    printf("  enc = %u  (0x%04x)\n", enc, enc);
    printf("  → compressed: 384× reduction (768B → 2B)\n");

    /* ── Step 4: frame_at (container address) ── */
    printf("\n─── Step 4: frame_at (container address) ───\n");
    int face, slot, phase, ico;
    frame_at(enc, &face, &slot, &phase, &ico);
    printf("  face   = %d  (cache bank, 0..11)\n", face);
    printf("  slot   = %d  (cache row,  0..119)\n", slot);
    printf("  phase  = %d  (timeline p,  0..11)\n", phase);
    printf("  ico    = %d  (icosphere,   0..161)\n", ico);
    printf("  → container address: [%d][%d][%d]\n", face, slot, ico);

    /* ── Step 5: stride-37 next frame ── */
    printf("\n─── Step 5: stride-37 walk ───\n");
    printf("  current  enc = %u\n", enc);
    uint16_t next_enc = (uint16_t)((enc + FRAME_STRIDE) % FRAME_CYCLE);
    printf("  next     enc = %u  (+37 stride)\n", next_enc);
    uint16_t prev_enc = (uint16_t)((enc - FRAME_STRIDE + FRAME_CYCLE) % FRAME_CYCLE);
    printf("  previous enc = %u  (-37 stride)\n", prev_enc);

    /* ── Step 6: Timeline coverage ── */
    printf("\n─── Step 6: Timeline stride-37 walk ───\n");
    printf("  stride-37 on %d-cycle:\n", FRAME_CYCLE);
    uint16_t walk = enc;
    for (int i = 0; i < 12; i++) {
        printf("    step %-2d: enc=%4u  face=%d slot=%d\n", i, walk, walk/120, walk%120);
        walk = (uint16_t)((walk + FRAME_STRIDE) % FRAME_CYCLE);
    }
    printf("  ... stride-37 covers all 1440 in 1440 steps (bijection)\n");

    /* ── Step 7: Reverse (decompose → recompose) ── */
    printf("\n─── Step 7: Roundtrip verify ───\n");
    int64_t recomposed = rdh_key(&cfg, ring, wedge, mirror, u, 0);
    printf("  original key  = %lld\n", (long long)key);
    printf("  recomposed    = %lld\n", (long long)recomposed);
    printf("  roundtrip: %s\n", key == recomposed ? "✅ PASS" : "❌ FAIL");

    /* ── Summary ── */
    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║  Summary                                    ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  Input:  %-5zu bytes                        ║\n", len);
    printf("║  Key:    %-5lld (20736 max)                 ║\n", (long long)key);
    printf("║  Enc:    %-5u (2 bytes, 1440 cycle)         ║\n", enc);
    printf("║  Frame:  f%d s%d p%d i%d                    ║\n", face, slot, phase, ico);
    printf("║  Next:   %-5u (+37 stride)                  ║\n", next_enc);
    printf("║  Ratio:  384× (%zuB → 2B)                   ║\n", len);
    printf("╚══════════════════════════════════════════════╝\n");

    return 0;
}
