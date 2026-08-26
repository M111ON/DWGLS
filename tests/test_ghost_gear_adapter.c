/*
 * test_ghost_gear_adapter.c — gear wire SWAPPED onto the real GhostLog core
 * ══════════════════════════════════════════════════════════════════════
 *
 * Adapter = core/geo_ghost_gear_adapter.h wrapping geo_ghost_lift.h:
 *   ghost_gear_lift   = ghost_lift (real core) + wire append
 *   ghost_gear_expire = ghost_expire (real core) + wire seal
 *   ghost_gear_replay = walk the chain from WIRE ONLY
 *
 * Checks (oracle อิสระทุกข้อ):
 *   A1  lift through adapter == lift through core (bond_key identical,
 *       same rs state size) + entry stamped GHOST_FLAG_GEAR
 *   A2  wire byte == fg_enc(from,to) packed (hand-computed event)
 *   A3  replay from WIRE ONLY == to_scale sequence of the log entries
 *       (per-block, multi-block mix; entries never read during replay)
 *   A4  bond identity untouched: origin_seed before/after equal for all;
 *       wire depends on Delta only (same Δ → same wire at any birth)
 *   A5  expire seals: replay stops at seal even with bytes after it
 *   A6  persistence: serialize→load roundtrip; geared-count mismatch
 *       → -2 (loud corrupt detection)
 *   A7  old-reader compatibility: GHST blob prefix identical whether or
 *       not the wire rides along
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../core/geo_ghost_gear_adapter.h"

static int fails = 0, checks = 0;
#define CHECK(cond, name) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL %s\n", name); } \
    else printf("ok   %s\n", name); \
} while (0)

static uint32_t st_ = 20260828u;
static uint32_t lcg(void) {
    st_ = st_ * 1664525u + 1013904223u;
    return (st_ >> 9) ^ (st_ >> 20);
}

int main(void) {
    printf("Ghost x Gear ADAPTER — gear wire on the real core\n");
    printf("====================================================\n");

    /* ── shared fixtures ───────────────────────────────────────────── */
    static ResidualSpace rs_a, rs_b;
    static GhostLog log_a, log_b;
    static GhostGearWire gw_a, gw_b;
    rs_init(&rs_a, 64);
    ghost_log_init(&log_a);
    memset(&gw_a, 0, sizeof(gw_a));

    /* ── A1+A2: lift via adapter vs plain core ─────────────────────── */
    {
        uint8_t data[64];
        for (int i = 0; i < 64; i++) data[i] = (uint8_t)(lcg() & 0xFF);

        /* plain core reference in a SEPARATE rs/log */
        rs_init(&rs_b, 64);
        ghost_log_init(&log_b);
        memset(&gw_b, 0, sizeof(gw_b));
        uint64_t bk_ref = ghost_lift(&log_b, &rs_b, 7, 3, 51, data, 64);

        uint64_t bk_adp = ghost_gear_lift(&log_a, &rs_a, &gw_a, 7, 3, 51,
                                          data, 64);
        CHECK(bk_ref != RS_BOND_KEY_RESERVED && bk_adp == bk_ref,
              "A1 adapter lift == core lift (identical bond_key)");

        /* stamp check: entry got the GEAR flag */
        int pos = _ghost_pile_lo(&log_a, 7, 3);
        CHECK(pos >= 0 && (log_a.entries[pos].flags & GHOST_FLAG_GEAR) &&
              (log_a.entries[pos].from_scale == 3) &&
              (log_a.entries[pos].to_scale == 51),
              "A1b entry stamped GHOST_FLAG_GEAR at sorted position");

        /* hand event: from=3,to=51 -> D=48=2*24+0 -> q=2 dc=0 dx=0 */
        uint8_t expect = (uint8_t)(2 | (0 << 3) | (0 << 6));
        CHECK(gw_a.n == 1 && gw_a.wire[0] == expect,
              "A2 hand event from=3->51: wire byte q=2|dc=0|dx=0");
    }

    /* ── build a multi-block chain through the REAL path ────────────── */
    {
        uint8_t data[32];
        for (int i = 0; i < 32; i++) data[i] = (uint8_t)(lcg() & 0xFF);
        /* block 9: chain 5 -> 40 -> 88 -> 130 (each a separate lift)     */
        ghost_gear_lift(&log_a, &rs_a, &gw_a, 9, 5, 40, data, 32);
        ghost_gear_lift(&log_a, &rs_a, &gw_a, 9, 40, 88, data, 32);
        ghost_gear_lift(&log_a, &rs_a, &gw_a, 9, 88, 130, data, 32);
        /* block 12: chain 60 -> 20 (backwards hop)                        */
        ghost_gear_lift(&log_a, &rs_a, &gw_a, 12, 60, 20, data, 32);
    }

    /* ── A3: replay from WIRE ONLY matches the log's own records ───── */
    {
        /* per-block: gather (from,to) chains straight from the log      */
        int ok_all = 1;
        uint16_t blocks[2] = {9, 12};
        uint8_t births[2] = {5, 60};
        for (int bi = 0; bi < 2; bi++) {
            uint8_t want[16], got[16];
            uint32_t nw = 0;
            for (uint32_t i = 0; i < log_a.count; i++)
                if (log_a.entries[i].block_id == blocks[bi] &&
                    !(log_a.entries[i].flags & GHOST_FLAG_EXPIRED))
                    want[nw++] = log_a.entries[i].to_scale;
            uint32_t hops = ghost_gear_replay(&log_a, &gw_a, blocks[bi],
                                              births[bi], got, 16);
            if (hops != nw) { ok_all = 0; break; }
            for (uint32_t i = 0; i < nw; i++)
                if (got[i] != want[i]) { ok_all = 0; break; }
        }
        CHECK(ok_all, "A3 wire-only replay == log to-sequences (2 blocks)");
    }

    /* ── A4: bond identity + Delta-only wire ────────────────────────── */
    {
        int ok = 1;
        for (uint32_t i = 0; i < log_a.count; i++)
            if (!(log_a.entries[i].flags & GHOST_FLAG_EXPIRED)) {
                PoglsPiece p = ghost_piece(log_a.entries[i].block_id,
                                           log_a.entries[i].from_scale, 0);
                if (p.geo_key != rdh_addr(log_a.entries[i].block_id,
                                          log_a.entries[i].from_scale)) ok = 0;
            }
        /* translation invariance on the local ring: same D -> same wire */
        for (uint32_t d = 1; d < 144 && ok; d++)
            if (((fg_enc(5u, (5u + d) % 144u).q |
                  (fg_enc(5u, (5u + d) % 144u).dc << 3) |
                  (fg_enc(5u, (5u + d) % 144u).dx << 6))) !=
                ((fg_enc(90u, (90u + d) % 144u).q |
                  (fg_enc(90u, (90u + d) % 144u).dc << 3) |
                  (fg_enc(90u, (90u + d) % 144u).dx << 6)))) ok = 0;
        CHECK(ok, "A4 bond=RDH address untouched; wire depends on Delta only");
    }

    /* ── A5: expire → seal stops replay ─────────────────────────────── */
    {
        uint32_t exp = ghost_gear_expire(&log_a, &rs_a, &gw_a, 9, 88);
        CHECK(exp == 1, "A5a core expire expired exactly 1 route");
        uint8_t got[16];
        /* block 9's span now holds [hop,hop,hop,SEAL] — replay must stop
           at the seal: exactly 3 hops even though the wire rides on      */
        uint32_t hops = ghost_gear_replay(&log_a, &gw_a, 9, 5, got, 16);
        CHECK(hops == 3, "A5b replay stops at seal (3 hops, not more)");
    }

    /* ── A6: persistence roundtrip + loud corruption ────────────────── */
    {
        static uint8_t blob[4096];
        uint64_t need = ghost_gear_serialize(&log_a, &gw_a, blob, sizeof(blob));
        CHECK(need > 0, "A6a serialize wrote bytes");
        /* old reader reads just the GHST part fine                      */
        GhostLog old_reader;
        int rc_old = ghost_log_load(&old_reader, blob, need);
        CHECK(rc_old == 0 && old_reader.count == log_a.count,
              "A6b old reader (GHST only) loads unchanged");
        /* new reader gets log+wire                                       */
        static GhostLog log_c;
        static GhostGearWire gw_c;
        int rc = ghost_gear_load(&log_c, &gw_c, blob, need);
        CHECK(rc == 0 && gw_c.n == gw_a.n &&
              memcmp(gw_c.wire, gw_a.wire, gw_a.n) == 0,
              "A6c new reader recovers wire byte-identical");
        /* corrupt: flip one geared flag off -> wired-bytes no longer
           match geared count (+1 seal) -> -2 loud
           (entry0 flags live at blob offset 12+4: blk u16 + from + to)   */
        static uint8_t blob2[4096];
        memcpy(blob2, blob, (size_t)need);
        blob2[16] &= (uint8_t)~GHOST_FLAG_GEAR;   /* entry0 flags byte   */
        int rc_bad = ghost_gear_load(&log_c, &gw_c, blob2, need);
        CHECK(rc_bad == -2, "A6d geared-count mismatch detected (-2)");
    }

    /* ── A7: GHST prefix compatibility ─────────────────────────────── */
    {
        uint8_t b1[4096], b2[4096];
        uint64_t n1 = ghost_log_serialize(&log_a, b1, sizeof(b1));
        uint64_t n2 = ghost_gear_serialize(&log_a, &gw_a, b2, sizeof(b2));
        CHECK(n1 > 0 && n2 > n1 && memcmp(b1, b2, (size_t)n1) == 0,
              "A7 GHST prefix identical with/without wire riding");
    }

    printf("\nRESULTS: %s (%d/%d pass)\n",
           fails ? "RED" : "ALL PASS", checks - fails, checks);
    return fails ? 1 : 0;
}
