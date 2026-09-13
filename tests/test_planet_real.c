/*
 * test_planet_real.c — Planet on REAL data + REAL engine output
 * ═══════════════════════════════════════════════════════════════════════════
 * Planet watches a real GGUF slice (default Qwen2.5-0.5B-Q8_0, +1MB offset,
 * 4MB) while a REAL BreathingFS breathes beside it; replay walks the REAL
 * FGXLog the ticks produced (frame-invariance: 20736 = 0 mod 144, so the
 * local-W walk tracks W_full % 144 by induction — proven HERE, not cited).
 *
 * Missing model file -> SKIP (return 0, named). Never writes the model.
 * RUN: ./build/test_planet_real [model.gguf]
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/test_planet_real tests/test_planet_real.c
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/geo_planet.h"
#include "../core/bfs_breath.h"

#define REAL_SLICE_OFF (1024u * 1024u)
#define REAL_SLICE_LEN (4u * 1024u * 1024u)
#define REAL_TICKS 2000u

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static uint64_t ref_digest(const int8_t *d, uint32_t n) {
    uint64_t h = 5381u;
    const int8_t *q = d, *end = d + n;
    for (; q < end; q++) h = h * 33u + (uint8_t)*q;
    return h;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1]
        : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    printf("═ PLANET REAL — %s ═\n", path);

    FILE *f = fopen(path, "rb");
    if (!f) { printf("  SKIP — model file not present (named, not hidden)\n"); return 0; }
    int8_t *slice = (int8_t *)malloc(REAL_SLICE_LEN);
    if (!slice) { fclose(f); printf("  SKIP — malloc failed\n"); return 0; }
    fseek(f, REAL_SLICE_OFF, SEEK_SET);
    size_t got = fread(slice, 1, REAL_SLICE_LEN, f);
    fclose(f);
    if (got != REAL_SLICE_LEN) { free(slice); printf("  SKIP — short read\n"); return 0; }
    {
        uint64_t dg = ref_digest(slice, REAL_SLICE_LEN);
        printf("  slice: 4.0 MB @+1MB, digest=%08x%08x\n",
               (uint32_t)(dg >> 32), (uint32_t)(dg & 0xFFFFFFFFu));
    }

    /* ── real churn beside the planet ── */
    BreathingFS fs;
    bfs_init(&fs);
    int8_t d0[144], d1[432];
    for (uint32_t i = 0; i < 144u; i++) d0[i] = (int8_t)((i * 7u) & 0xFFu);
    for (uint32_t i = 0; i < 432u; i++) d1[i] = (int8_t)((i * 13u + 1u) & 0xFFu);
    fs.seeker.current_pos = 500; fs.seeker.home_pos = 500;
    bfs_write(&fs, "a.bin", d0, 144);
    fs.seeker.current_pos = 1200; fs.seeker.home_pos = 1200;
    bfs_write(&fs, "b.bin", d1, 432);

    Planet p;
    planet_birth(&p, 21u, 10u, 777u, slice, REAL_SLICE_LEN);
    CHECK("R1: birth digest == ref over 4MB real bytes",
          p.digest == ref_digest(slice, REAL_SLICE_LEN));

    Planet snap = p;
    uint32_t n0 = fs.fg_log.hdr.n;
    BFSBreath b;
    bfs_breath_init(&b, &fs, 0.05);
    for (uint32_t i = 0; i < REAL_TICKS; i++) bfs_breath_tick(&b);
    uint32_t n1 = fs.fg_log.hdr.n;
    CHECK("R2: detach — planet bit-identical after 2000 real ticks",
          b.reanchors > 0 && memcmp(&snap, &p, sizeof(Planet)) == 0 &&
          planet_verify(&p, slice, REAL_SLICE_LEN) == 0);
    printf("  main: reanchors=%u, real FGXLog events=%u\n", b.reanchors, n1 - n0);

    /* ── real-log integrity: backward reconstruct + enc-consistency ── */
    uint32_t cur_last = (uint32_t)(BFS_TOTAL_SLOTS * b.cur_scale) % BFS_TOTAL_SLOTS;
    uint32_t chain[FG_LOG_CAP + 1];
    uint32_t got_n = fgx_reconstruct(&fs.fg_log, cur_last, chain, FG_LOG_CAP + 1u);
    int consistent = (got_n == n1 + 1u);
    for (uint32_t i = 0; consistent && i < n1; i++) {
        FGGearEv e = fgx_enc(chain[i], chain[i + 1u]);
        FGGearEv s = fs.fg_log.ev[i];
        if (e.q != s.q || e.dc != s.dc || e.dx != s.dx) consistent = 0;
    }
    CHECK("R3: real FGXLog self-consistent (reconstruct+enc, all events)", consistent);

    /* ── real replay: late-join planet at chain head (local projection) ──
     * Sight rule (mask=8): a planet sees <=8 events from its birth frame.
     * Short tail (born 8 back): agrees. Long tail (born 256 back):
     * diverges HONESTLY — rule is re-birth closer, not blind trust. */
    {
        Planet late;
        uint32_t head_local = chain[n0] % FG_LOCAL;
        planet_birth(&late, 22u, head_local, 1u, slice, REAL_SLICE_LEN);
        /* trouble round in place: gate opens, bytes restored after */
        slice[999] ^= 0x08;
        int opened = (planet_verify(&late, slice, REAL_SLICE_LEN) == 1 &&
                      late.link_open == 1u);
        slice[999] ^= 0x08;
        /* walk ONLY the new events with the real ev structs */
        int agree = planet_replay(&late, &fs.fg_log.ev[n0], n1 - n0, cur_last % FG_LOCAL);
        FGGearEv tmp[FG_LOG_CAP];
        memcpy(tmp, &fs.fg_log.ev[n0], (n1 - n0) * sizeof(FGGearEv));
        if (n1 - n0 > 0) tmp[0].dc ^= 1u;
        int div = planet_replay(&late, tmp, n1 - n0, cur_last % FG_LOCAL);
        uint32_t tail_len = n1 - n0;
        /* short-tail proof: born within sight, walks the last <=8 events */
        Planet near;
        uint32_t nk = tail_len < 8u ? tail_len : 8u;
        uint32_t nnear = n1 - nk;
        planet_birth(&near, 23u, chain[nnear] % FG_LOCAL, 2u, slice, REAL_SLICE_LEN);
        slice[999] ^= 0x08;
        int nopened = (planet_verify(&near, slice, REAL_SLICE_LEN) == 1);
        slice[999] ^= 0x08;
        int nagree = planet_replay(&near, &fs.fg_log.ev[nnear], nk, cur_last % FG_LOCAL);
        CHECK("R4: gate opens; short tail agrees; long tail diverges honestly (mask)",
              opened && nopened && nagree == 0 &&
              (tail_len <= 8u ? (agree == 0 && (tail_len == 0 || div == 1))
                              : (agree == 1 && div == 1)));
    }

    /* ── R7: fan12-as-view over the REAL log (no new storage) ── */
    {
        /* r12 = r24 % 12; quad = dc % 4; axis = dx.
         * Oracle: projected teeth must recover D%12 for every real event. */
        int ok = (n1 > n0);
        for (uint32_t i = n0; ok && i < n1; i++) {
            FGGearEv s = fs.fg_log.ev[i];
            uint32_t r24 = (uint32_t)s.q * FG_RING + fg_crt(s.dc, s.dx);
            uint32_t d_full = r24 % FG_FULL;
            uint32_t r12 = r24 % 12u;
            uint32_t quad = (uint32_t)s.dc % 4u;
            /* cross-check: dc/dx decode back to r24 (self-consistency),
             * and r12 matches d_full % 12 (projection correctness) */
            if (fg_crt(s.dc, s.dx) != (r24 % FG_RING)) ok = 0;
            if (r12 != (d_full % 12u)) ok = 0;
            if (quad != ((r24 % FG_RING) % 4u)) ok = 0;
        }
        CHECK("R7: fan12 view derivable from every real fan24 event", ok);
    }

    /* ── real corruption collect + tombstone ── */
    {
        int8_t save = slice[123456];
        slice[123456] ^= 0x01;
        int rc = planet_verify(&p, slice, REAL_SLICE_LEN);
        slice[123456] = save;
        int clean = planet_verify(&p, slice, REAL_SLICE_LEN);
        CHECK("R5: 1-bit real corruption collected, restore clean",
              rc == 1 && p.tail_n == 1u && clean == 0 &&
              p.tail[0].observed != p.tail[0].expected);
        planet_retire(&p, 12u);
        CHECK("R6: tombstone carries real digest, severed after",
              p.tomb.magic == PLANET_TOMB_MAGIC && p.tomb.digest == p.digest &&
              planet_verify(&p, slice, REAL_SLICE_LEN) == -2);
    }

    printf("═ RESULT: %d pass, %d fail ═\n", pass_count, fail_count);
    free(slice);
    return fail_count ? 1 : 0;
}
