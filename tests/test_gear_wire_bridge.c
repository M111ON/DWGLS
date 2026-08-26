/*
 * test_gear_wire_bridge.c — interop bridge for the GHST+gear-wire container
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Tests gear_wire_bridge.h against the real core (geo_ghost_gear_adapter.h)
 * and independent oracles (brute-force CRT, hand-computed events, etc.).
 *
 * B1  bridge parses real-code-produced container; entry fields match
 * B2  hand-computed oracle: from=3→51 = q=2|dc=0|dx=0 → byte 0x02
 * B3  bridge block_chain == core ghost_gear_replay (multi-block, out-of-order)
 * B4  enter-anywhere backward reconstruct matches forward (bridge-side)
 * B5  seal semantics: replay stops at seal (bridge + core agree)
 * B6  no-expire container roundtrip (regression: old loader rejected)
 * B7  corruption drills: bad magic / truncated / flag flip → loud errors
 * B8  JSON output: key fields present + valid=true
 * B9  bridge writer → core loader roundtrip (interop BOTH directions)
 * B10 CRT brute-force oracle: all 24 cells unique
 * B11 exhaustive encode→decode [0,144)×[0,144) via bridge side
 * B12 bridge gwb_write → gwb_parse roundtrip
 *
 * BUILD: gcc -O2 -Wall -Wextra -I. -Icore -o build/test_gear_wire_bridge \
 *          tests/test_gear_wire_bridge.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ── Bridge (self-contained — no DWGLS deps) ────────────────────────────── */
#include "../core/gear_wire_bridge.h"

/* ── Core stack (to produce real containers) ────────────────────────────── */
#include "../core/geo_ghost_gear_adapter.h"

static int fails = 0, checks = 0;
#define CHECK(cond, name) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL %s\n", name); } \
    else printf("ok   %s\n", name); \
} while (0)

static uint32_t lcg_state = 20260826u;
static uint32_t lcg(void) {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return (lcg_state >> 9) ^ (lcg_state >> 20);
}

int main(void) {
    printf("Gear Wire Bridge — interop test\n");
    printf("================================\n");

    /* ── shared fixtures ───────────────────────────────────────────── */
    static ResidualSpace rs;
    static GhostLog log;
    static GhostGearWire gw;
    rs_init(&rs, 64);
    ghost_log_init(&log);
    memset(&gw, 0, sizeof(gw));

    /* build a multi-block, multi-route container */
    uint8_t data[64];
    for (int i = 0; i < 64; i++) data[i] = (uint8_t)(lcg() & 0xFF);
    /* out-of-order: lift block 9 first, then block 3, then block 9 again */
    ghost_gear_lift(&log, &rs, &gw, 9, 10, 60, data, 32);
    ghost_gear_lift(&log, &rs, &gw, 3, 5, 70, data, 32);
    ghost_gear_lift(&log, &rs, &gw, 9, 60, 110, data, 32);
    /* expire block 9's first pile → seal */
    ghost_gear_expire(&log, &rs, &gw, 9, 10);

    static uint8_t blob[4096];
    uint64_t need = ghost_gear_serialize(&log, &gw, blob, sizeof(blob));

    /* ── B1: bridge parses real container ──────────────────────────── */
    {
        gwb_view v;
        int rc = gwb_parse(blob, need, &v);
        CHECK(rc == GWB_OK, "B1a bridge parse ok");
        CHECK(v.count == log.count, "B1b entry count matches");
        /* check entry fields match log */
        int ent_ok = 1;
        for (uint32_t i = 0; i < v.count && ent_ok; i++) {
            gwb_entry be;
            gwb_entry_get(&v, i, &be);
            if (be.block_id != log.entries[i].block_id ||
                be.from_scale != log.entries[i].from_scale ||
                be.to_scale != log.entries[i].to_scale ||
                be.flags != log.entries[i].flags) ent_ok = 0;
        }
        CHECK(ent_ok, "B1c entry fields match core log byte-for-byte");
        /* validate */
        CHECK(gwb_validate(&v) == GWB_OK, "B1d gwb_validate ok");
    }

    /* ── B2: hand-computed oracle ──────────────────────────────────── */
    {
        /* from=3, to=51 → D = (51+144-3)%144 = 48 = 24*2 + 0 → q=2, r=0
         * r=0 → dc=0, dx=0 → byte = 2 | 0<<3 | 0<<6 = 0x02.
         * Verify via brute force and bridge decode. */
        uint8_t byte = gwb_encode_byte(2, 0, 0);
        CHECK(byte == 0x02, "B2a hand-computed: from=3→51 = 0x02");

        uint8_t q, dc, dx;
        gwb_decode(0x02, &q, &dc, &dx);
        CHECK(q == 2 && dc == 0 && dx == 0, "B2b decode 0x02 = q=2 dc=0 dx=0");

        /* from=3 → step(3, q=2, dc=0, dx=0) should = 51 */
        CHECK(gwb_step(3, 2, 0, 0) == 51, "B2c gwb_step(3,2,0,0) = 51");
    }

    /* ── B3: bridge block_chain == core replay (multi-block) ───────── */
    {
        gwb_view v;
        gwb_parse(blob, need, &v);
        int ok = 1;
        struct { uint16_t blk; uint8_t birth; } cases[] = { {9,10}, {3,5} };
        for (int c = 0; c < 2 && ok; c++) {
            uint8_t br[16], bc[16];
            uint32_t hr = ghost_gear_replay(&log, &gw, cases[c].blk,
                                            cases[c].birth, br, 16);
            uint32_t hb = gwb_block_chain(&v, cases[c].blk, cases[c].birth,
                                          bc, 16);
            if (hr != hb) { ok = 0; break; }
            for (uint32_t i = 0; i < hr; i++)
                if (br[i] != bc[i]) { ok = 0; break; }
        }
        CHECK(ok, "B3 bridge chain == core replay (2 blocks, out-of-order)");
    }

    /* ── B4: enter-anywhere backward reconstruct ───────────────────── */
    {
        /* block 3: chain 5→70. One event: D=(70+144-5)%144=105=24*4+9.
         * q=4, r=9: dc=9%8=1, dx=9%3=0. byte = 4 | 1<<3 | 0<<6 = 0x0C. */
        uint8_t ev_bytes[4];
        uint8_t q_val, dc_val, dx_val;
        uint32_t D = (70u + 144u - 5u) % 144u;
        q_val = (uint8_t)(D / 24);
        dc_val = (uint8_t)((D % 24) % 8);
        dx_val = (uint8_t)((D % 24) % 3);
        ev_bytes[0] = gwb_encode_byte(q_val, dc_val, dx_val);
        uint32_t chain[4];
        uint32_t m = gwb_reconstruct(ev_bytes, 1, 70, chain, 4);
        CHECK(m == 2 && chain[0] == 5 && chain[1] == 70,
              "B4 backward reconstruct recovers append scale (enter-anywhere)");
        /* negative control: wrong cur_w → different append scale */
        uint32_t bad_chain[4];
        gwb_reconstruct(ev_bytes, 1, 71, bad_chain, 4);
        CHECK(bad_chain[0] != 5, "B4b negative control: wrong cur_w ≠ append");
    }

    /* ── B5: seal stops bridge chain ───────────────────────────────── */
    {
        gwb_view v;
        gwb_parse(blob, need, &v);
        uint8_t got[16];
        uint32_t h = gwb_block_chain(&v, 9, 10, got, 16);
        /* block 9: event 10→60, then seal (expire stopped 10→60 chain).
         * Actually: expire(9,10) expired pile (9,10) which had 1 route.
         * Chain has 2 events: 10→60 (pile 9/10) then 60→110 (pile 9/60).
         * Seal sits after block 5's byte in canonical, so block 9 sees
         * both events, then seal stops. */
        uint8_t got_r[16];
        uint32_t h_r = ghost_gear_replay(&log, &gw, 9, 10, got_r, 16);
        CHECK(h == h_r, "B5a bridge chain length == core for sealed block");
        int match = 1;
        for (uint32_t i = 0; i < h; i++)
            if (got[i] != got_r[i]) { match = 0; break; }
        CHECK(match, "B5b bridge chain values == core for sealed block");
    }

    /* ── B6: no-expire container (regression) ──────────────────────── */
    {
        static ResidualSpace rs2;
        static GhostLog log2;
        static GhostGearWire gw2;
        rs_init(&rs2, 64);
        ghost_log_init(&log2);
        memset(&gw2, 0, sizeof(gw2));
        uint8_t d[32]; memset(d, 0xBB, sizeof(d));
        ghost_gear_lift(&log2, &rs2, &gw2, 2, 0, 36, d, 32);
        ghost_gear_lift(&log2, &rs2, &gw2, 2, 36, 72, d, 32);
        static uint8_t blob2[4096];
        uint64_t n2 = ghost_gear_serialize(&log2, &gw2, blob2, sizeof(blob2));
        gwb_view v;
        int rc = gwb_parse(blob2, n2, &v);
        CHECK(rc == GWB_OK, "B6a no-expire container parsed ok");
        CHECK(gwb_validate(&v) == GWB_OK, "B6b no-expire validates ok");
        uint8_t got[16];
        uint32_t h = gwb_block_chain(&v, 2, 0, got, 16);
        CHECK(h == 2 && got[0] == 36 && got[1] == 72,
              "B6c no-expire chain = 2 hops lossless");
    }

    /* ── B7: corruption drills ─────────────────────────────────────── */
    {
        uint8_t bad[4096];
        /* bad magic */
        memcpy(bad, blob, (size_t)need);
        bad[0] = 'X';
        gwb_view v;
        CHECK(gwb_parse(bad, need, &v) == GWB_E_MAGIC, "B7a bad magic → GWB_E_MAGIC");
        /* truncated buffer */
        CHECK(gwb_parse(blob, 8, &v) == GWB_E_SMALL, "B7b truncated → GWB_E_SMALL");
        CHECK(gwb_parse(blob, 13, &v) == GWB_E_TRUNC, "B7c partial count → GWB_E_TRUNC");
        /* flag flip: reduce geared count → evs != geared → GWB_E_WIRE */
        memcpy(bad, blob, (size_t)need);
        bad[16] &= (uint8_t)~GHOST_FLAG_GEAR;   /* entry0 flags byte */
        /* after flag flip the old loader returns -2; bridge returns -7 */
        CHECK(gwb_parse(bad, need, &v) == GWB_OK,
              "B7d flag flip: parse still ok (flags are data, not structural)");
        CHECK(gwb_validate(&v) == GWB_E_WIRE,
              "B7e flag flip: validate catches seal-accounting mismatch");
    }

    /* ── B8: JSON output sanity ────────────────────────────────────── */
    {
        gwb_view v;
        gwb_parse(blob, need, &v);
        char json[8192];
        uint64_t len = gwb_json(&v, json, sizeof(json));
        CHECK(len > 100 && len < sizeof(json), "B8a JSON emitted non-trivial");
        CHECK(strstr(json, "\"format\":\"ghst-gear-wire\"") != NULL,
              "B8b JSON has format field");
        CHECK(strstr(json, "\"version\":1") != NULL, "B8c JSON has version field");
        CHECK(strstr(json, "\"valid\":true") != NULL, "B8d JSON valid=true");
        CHECK(strstr(json, "\"records\":[") != NULL, "B8e JSON has records array");
        CHECK(strstr(json, "\"wire\":[") != NULL, "B8f JSON has wire array");
        /* hex output is present */
        CHECK(strstr(json, "\"wire_bytes_hex\":\"") != NULL,
              "B8g JSON has wire_bytes_hex");
    }

    /* ── B9: bridge writer → core loader roundtrip ─────────────────── */
    {
        /* build a known container via bridge writer */
        gwb_entry evts[] = {
            { .block_id = 1, .from_scale = 0, .to_scale = 24, .flags = GWB_FLAG_GEAR | 0x01 },
            { .block_id = 1, .from_scale = 24, .to_scale = 48, .flags = GWB_FLAG_GEAR | 0x01 },
            { .block_id = 4, .from_scale = 10, .to_scale = 50, .flags = GWB_FLAG_GEAR | 0x01 },
        };
        /* wire in canonical order: block 1 events, block 4 event */
        uint8_t wire_w[3];
        /* D(0→24)=24 → q=1,r=0 → 0x01; D(24→48)=24 → 0x01 */
        wire_w[0] = gwb_encode_byte(1, 0, 0);
        wire_w[1] = gwb_encode_byte(1, 0, 0);
        /* D(10→50)=40 → q=1,r=16→dc=0,dx=1 → 0x41 */
        wire_w[2] = gwb_encode_byte(1, 0, 1);

        static uint8_t wblob[4096];
        uint64_t wn = gwb_write(evts, 3, wire_w, 3, wblob, sizeof(wblob));
        CHECK(wn > 0, "B9a bridge writer produced bytes");
        /* parse back via bridge */
        gwb_view v;
        CHECK(gwb_parse(wblob, wn, &v) == GWB_OK, "B9b bridge re-parses writer output");
        CHECK(gwb_validate(&v) == GWB_OK, "B9c writer output validates");
        /* also load via core ghost_gear_load */
        static GhostLog log_w;
        static GhostGearWire gw_w;
        int rc = ghost_gear_load(&log_w, &gw_w, wblob, wn);
        CHECK(rc == 0, "B9d core loader accepts bridge-written container");
        CHECK(gw_w.n == 3, "B9e core loaded 3 wire bytes from bridge output");
    }

    /* ── B10: CRT brute-force oracle ───────────────────────────────── */
    {
        int ok = 1;
        for (uint8_t dc = 0; dc < 8 && ok; dc++)
            for (uint8_t dx = 0; dx < 3 && ok; dx++) {
                uint8_t s = gwb_crt(dc, dx);
                if (s >= 24 || s % 8u != dc || s % 3u != dx) ok = 0;
                for (uint8_t t = (uint8_t)(s + 1); t < 24; t++)
                    if (t % 8u == dc && t % 3u == dx) ok = 0;
            }
        CHECK(ok, "B10 gwb_crt: all 24 cells resolve + unique");
    }

    /* ── B11: exhaustive encode→decode [0,144)×[0,144) ────────────── */
    {
        int ok = 1;
        for (uint32_t f = 0; f < 144 && ok; f++)
            for (uint32_t t = 0; t < 144; t++) {
                /* bridge encode */
                uint32_t D = (t + 144 - f) % 144;
                uint8_t bq = (uint8_t)(D / 24);
                uint8_t bdc = (uint8_t)((D % 24) % 8);
                uint8_t bdx = (uint8_t)((D % 24) % 3);
                uint8_t byte = gwb_encode_byte(bq, bdc, bdx);
                /* bridge decode + step */
                uint8_t q2, dc2, dx2;
                gwb_decode(byte, &q2, &dc2, &dx2);
                uint32_t res = gwb_step(f, q2, dc2, dx2);
                if (res != t || q2 != bq || dc2 != bdc || dx2 != bdx) ok = 0;
            }
        CHECK(ok, "B11 exhaustive encode→decode 144×144 = 20736 pairs");
    }

    /* ── B12: bridge write → parse roundtrip ────────────────────────── */
    {
        gwb_entry ev[] = {
            { 5, 0, 24, 0x09 },
            { 5, 24, 48, 0x09 },
        };
        uint8_t wr[2];
        wr[0] = gwb_encode_byte(1, 0, 0);
        wr[1] = gwb_encode_byte(1, 0, 0);
        static uint8_t rt_blob[4096];
        uint64_t rt_n = gwb_write(ev, 2, wr, 2, rt_blob, sizeof(rt_blob));
        gwb_view rv;
        gwb_parse(rt_blob, rt_n, &rv);
        gwb_entry re;
        gwb_entry_get(&rv, 0, &re);
        CHECK(re.block_id == 5 && re.from_scale == 0 && re.to_scale == 24,
              "B12a write→parse entry 0 roundtrip");
        gwb_entry_get(&rv, 1, &re);
        CHECK(re.block_id == 5 && re.from_scale == 24 && re.to_scale == 48,
              "B12b write→parse entry 1 roundtrip");
        uint8_t got[4];
        uint32_t h = gwb_block_chain(&rv, 5, 0, got, 4);
        CHECK(h == 2 && got[0] == 24 && got[1] == 48,
              "B12c write→parse chain lossless");
    }

    /* ═══════════════════════════════════════════════════════════════════
     * FGF2 FULL-FIELD TESTS (B13–B19)
     * ═══════════════════════════════════════════════════════════════════ */

    /* ── B13: FGF2 encode/decode roundtrip (oracle: CRT brute-force) ── */
    {
        int ok = 1;
        /* test a sample of full-field pairs */
        for (uint32_t f = 0; f < 20736 && ok; f += 113) {
            for (uint32_t t = 0; t < 20736 && ok; t += 97) {
                uint32_t D = (t + 20736 - f) % 20736;
                uint16_t q = (uint16_t)(D / 24);
                uint8_t dc = (uint8_t)((D % 24) % 8);
                uint8_t dx = (uint8_t)((D % 24) % 3);
                uint8_t lo, hi;
                gwb_fg_encode(q, dc, dx, &lo, &hi);
                uint16_t q2; uint8_t dc2, dx2;
                gwb_fg_decode(lo, hi, &q2, &dc2, &dx2);
                if (q2 != q || dc2 != dc || dx2 != dx) ok = 0;
                uint32_t res = gwb_fg_step(f, q2, dc2, dx2);
                if (res != t) ok = 0;
            }
        }
        CHECK(ok, "B13 FGF2 encode→decode sample of full-field pairs");
    }

    /* ── B14: FGF2 hand-computed oracle ────────────────────────────── */
    {
        /* from=100, to=500, D=400, q=400/24=16, r=400%24=16,
         * dc=16%8=0, dx=16%3=1. byte = 16|(0<<10)|(1<<13) = 0x2000
         * lo=0x00, hi=0x20 */
        uint8_t lo, hi;
        gwb_fg_encode(16, 0, 1, &lo, &hi);
        CHECK(lo == 0x10 && hi == 0x20,
              "B14a hand-computed FGF2: from=100→500 = 0x10,0x20");
        uint16_t q; uint8_t dc, dx;
        gwb_fg_decode(0x10, 0x20, &q, &dc, &dx);
        CHECK(q == 16 && dc == 0 && dx == 1,
              "B14b decode 0x10,0x20 = q=16 dc=0 dx=1");
        CHECK(gwb_fg_step(100, 16, 0, 1) == 500,
              "B14c gwb_fg_step(100,16,0,1) = 500");
    }

    /* ── B15: FGF2 writer → parse roundtrip ────────────────────────── */
    {
        gwb_fg_event evts[] = {
            { 1, 0, 0 },   /* q=1,dc=0,dx=0 → Δ=24 */
            { 16, 0, 1 },  /* q=16,dc=0,dx=1 → Δ=400 */
            { 0, 7, 2 },   /* q=0,dc=7,dx=2 → Δ=23 */
        };
        static uint8_t fbuf[256];
        uint64_t fn = gwb_fg_write(evts, 3, 0, fbuf, sizeof(fbuf));
        CHECK(fn == 12 + 6, "B15a FGF2 writer produced correct size (12+6=18)");
        gwb_view v;
        int rc = gwb_parse(fbuf, fn, &v);
        CHECK(rc == GWB_OK, "B15b FGF2 parse ok");
        CHECK(v.format == GWB_FMT_FGF2 && v.field_size == 20736,
              "B15c FGF2 detected: format=FMT_FGF2, field=20736");
        CHECK(v.count == 3, "B15d FGF2 event count = 3");
        CHECK(gwb_validate(&v) == GWB_OK, "B15e FGF2 validates ok");
    }

    /* ── B16: FGF2 chain walk lossless ─────────────────────────────── */
    {
        /* events: Δ=24, Δ=400, Δ=23 on field 20736
         * birth=1000 → 1024 → 1424 → 1447 */
        gwb_fg_event evts[] = { {1,0,0}, {16,0,1}, {0,7,2} };
        static uint8_t fbuf[256];
        gwb_fg_write(evts, 3, 0, fbuf, sizeof(fbuf));
        gwb_view v;
        gwb_parse(fbuf, 18, &v);
        uint32_t got[4];
        uint32_t h = gwb_fg_chain(&v, 1000, got, 4);
        CHECK(h == 3, "B16a FGF2 chain walk 3 hops");
        CHECK(got[0] == 1024 && got[1] == 1424 && got[2] == 1447,
              "B16b FGF2 chain values: 1000→1024→1424→1447");
    }

    /* ── B17: FGF2 backward reconstruct (enter-anywhere) ───────────── */
    {
        /* given events Δ=24, Δ=400 and cur_w=1424
         * reconstruct: w[2]=1424, w[1]=1424-400=1024, w[0]=1024-24=1000 */
        uint8_t packed[4];
        gwb_fg_encode(1, 0, 0, &packed[0], &packed[1]);   /* Δ=24 */
        gwb_fg_encode(16, 0, 1, &packed[2], &packed[3]);  /* Δ=400 */
        uint32_t chain[4];
        uint32_t m = gwb_fg_reconstruct(packed, 2, 1424, chain, 4);
        CHECK(m == 3 && chain[0] == 1000 && chain[1] == 1024 && chain[2] == 1424,
              "B17 FGF2 backward reconstruct: 1000←1024←1424 (enter-anywhere)");
    }

    /* ── B18: FGF2 corrupt detection (bit15 set) ──────────────────── */
    {
        gwb_fg_event evts[] = { {1,0,0} };
        static uint8_t fbuf[256];
        gwb_fg_write(evts, 1, 0, fbuf, sizeof(fbuf));
        /* flip bit15 on the event byte (hi |= 0x80) */
        fbuf[13] |= 0x80;  /* offset 12=header, byte 1 of first event */
        gwb_view v;
        gwb_parse(fbuf, 14, &v);
        CHECK(gwb_validate(&v) == GWB_E_WIRE,
              "B18 FGF2 bit15 set → validate catches corrupt");
    }

    /* ── B19: FGF2 seal stops chain ────────────────────────────────── */
    {
        /* 2 events + seal: Δ=24, Δ=400, SEAL */
        gwb_fg_event evts[] = { {1,0,0}, {16,0,1} };
        static uint8_t fbuf[256];
        gwb_fg_write(evts, 2, 0, fbuf, sizeof(fbuf));
        /* append 2-byte seal at offset 16 */
        fbuf[16] = 0xFF; fbuf[17] = 0xFF;
        gwb_view v;
        gwb_parse(fbuf, 18, &v);
        uint32_t got[4];
        uint32_t h = gwb_fg_chain(&v, 1000, got, 4);
        CHECK(h == 2 && got[0] == 1024 && got[1] == 1424,
              "B19 FGF2 seal stops chain (2 hops, not 3)");
    }

    printf("\nRESULTS: %s (%d/%d pass)\n",
           fails ? "RED" : "ALL PASS", checks - fails, checks);
    return fails ? 1 : 0;
}
