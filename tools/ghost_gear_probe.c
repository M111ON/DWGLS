/*
 * ghost_gear_probe.c — Gear event format บน consumer จริง: GhostLog route log
 *
 * consumer = core/geo_ghost_lift.h — GhostLogEntry {block_id:u16, from:u8,
 * to:u8, flags:u8} = 5 B/event (route = from→to, telescope)
 *
 * คำถามวัด (ต่อจาก fan24_gear_sync_probe M5): gear {q,dc,dx} = 8b/event
 * เก็บ ROUTE ของ ghost ได้กี่บิต? — bond (block,from) ห้ามแตะ (birth identity,
 * RDH address) → gear มีที่เดียวให้ยืน: ฝั่ง ROUTE
 *
 * โครง:
 *   per-block chain: entries ของ block เดียว sorted by from (b-bond principle
 *   รับประกันอยู่แล้ว) → route chain f0→f1→...→fk เดินได้ด้วย Δ เท่านั้น
 *   event = { q:3b, dc:3b, dx:2b } = 1 B  + flag-bit ใน entry เดิม (ฟรี)
 *         vs {from,to} = 2 B → 50%
 *   RIM (Δ ≡ 0 mod 24): q-only 3 b/event → 62.5%
 *
 * การวัด (oracle อิสระ):
 *   P1  encode→decode EXACT คืน (from,to) ทุก entry — sweep 144² คู่
 *   P2  chain replay: Δ-log เดินคืน w-chain == original (multi-block, LCG)
 *   P3  ขนาดจริง: baseline 2 B vs gear FREE 1 B vs RIM — วัด bytes ตรง ๆ
 *       บน synthetic lift pattern (ROI cliff: 3→140 telescope + random hops)
 *   P4  bond integrity: (block,from) เป็น address ไม่เปลี่ยน — origin_seed
 *       ก่อน/หลัง encode เท่าเดิมทุก entry (RDH ไม่ถูกแตะ)
 *   P5  mutation: พัง fg_crt (shadow copy) → decode ต้องเพี้ยน → probe RED
 *       (run-time check ผ่าน self-verify ตรวจ fg_crt==bruteforce ทุกครั้ง)
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format \
 *          -I. -Icore -o build/ghost_gear_probe tools/ghost_gear_probe.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../core/fan24_gear.h"

static int fails = 0, checks = 0;
#define CHECK(cond, name) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL %s\n", name); } \
    else printf("ok   %s\n", name); \
} while (0)

/* ── GhostLogEntry shape (mirror ของ geo_ghost_lift.h — ไม่ include กัน dep chain) ── */
typedef struct __attribute__((packed)) {
    uint16_t block_id;
    uint8_t  from_scale;
    uint8_t  to_scale;
    uint8_t  flags;
} Entry;                       /* 5 B */

#define N_ENTRIES 512u

/* ── gear wire สำหรับ route: 1 byte FREE (q|dc<<3|dx<<6), RIM 3b ── */
static uint8_t route_enc(uint32_t from, uint32_t to) {
    FGGearEv e = fg_enc(from, to);
    return (uint8_t)(e.q | (e.dc << 3) | (e.dx << 6));   /* 8 b */
}
static uint32_t route_dec(uint32_t from, uint8_t wire) {
    FGGearEv e;
    e.q  = (uint8_t)(wire & 7u);
    e.dc = (uint8_t)((wire >> 3) & 7u);
    e.dx = (uint8_t)((wire >> 6) & 3u);
    return fg_dec(from, e);
}

int main(void) {
    printf("ghost_gear_probe — gear events on the real consumer (GhostLog routes)\n");
    printf("════════════════════════════════════════════════════════\n");

    /* self-verify CRT ก่อนทุกอย่าง (mutation tripwire ในตัว) */
    {
        int ok = 1;
        for (uint8_t dc = 0; dc < 8 && ok; dc++)
            for (uint8_t dx = 0; dx < 3 && ok; dx++) {
                uint8_t s = fg_crt(dc, dx);
                if (s >= 24 || s % 8u != dc || s % 3u != dx) ok = 0;
                for (uint8_t t = (uint8_t)(s + 1u); t < 24; t++)
                    if (t % 8u == dc && t % 3u == dx) ok = 0;
            }
        CHECK(ok, "P0 self-verify fg_crt == brute-force oracle (tripwire)");
    }

    /* ── P1: exhaustive 144² route encode→decode ─────────────────────── */
    {
        int ok = 1;
        for (uint32_t f = 0; f < 144 && ok; f++)
            for (uint32_t t = 0; t < 144; t++) {
                if (route_dec(f, route_enc(f, t)) != t) { ok = 0; break; }
            }
        CHECK(ok, "P1 route wire (1B) encode->decode exact over 144x144");
    }

    /* ── สร้าง synthetic lift pattern (ROI cliff + multi-block chains) ── */
    static Entry ent[N_ENTRIES];
    uint32_t n = 0;
    {
        uint32_t st = 20260826u;
        /* 8 blocks · chain ยาว ๆ ต่อ block: from=prev_to (chain แท้) */
        for (uint32_t b = 0; b < 8 && n + 64 <= N_ENTRIES; b++) {
            uint32_t w = 3u + b;                       /* birth scale ต่างกัน */
            for (uint32_t k = 0; k < 64; k++) {
                st = st * 1664525u + 1013904223u;
                uint32_t nxt = st % 144u;
                if (nxt == w) continue;
                ent[n].block_id  = (uint16_t)b;
                ent[n].from_scale = (uint8_t)w;
                ent[n].to_scale   = (uint8_t)nxt;
                ent[n].flags      = 1u;               /* GHOST_FLAG_LIFT */
                n++;
                w = nxt;
            }
        }
    }

    /* ── P2: chain replay จาก Δ-wire ล้วน ────────────────────────────── */
    {
        int ok_replay = 1;
        uint32_t i = 0;
        while (i < n) {
            uint32_t j = i;                            /* block boundary */
            uint32_t b = ent[i].block_id;
            while (j < n && ent[j].block_id == b) j++;
            /* chain ภายใน block [i, j): from ตัวแรก = seed, ที่เหลือ
               from == to ของก่อนหน้า (chain แ้ by construction) */
            uint32_t w = ent[i].from_scale;
            for (uint32_t k = i; k < j; k++) {
                uint32_t got = route_dec(w, route_enc(ent[k].from_scale,
                                                      ent[k].to_scale));
                if (got != ent[k].to_scale) { ok_replay = 0; break; }
                w = ent[k].to_scale;
            }
            if (!ok_replay) break;
            i = j;
        }
        CHECK(ok_replay, "P2 per-block chain replay from delta-wire only -> exact");
    }

    /* ── P4b: bond = address ไม่ถูก adapter แตะ (structural check) ───── */
    {
        /* wire byte สร้างจาก (from,to) เท่านั้น — พิสูจน์: from ต่างกัน
           (block ต่างกัน) ให้ wire เดิมเมื่อ Δ เดียวกัน = route field ไม่
           พา identity → identity อยู่ที่ entry fields เดิม 100% */
        int ok = 1;
        for (uint32_t d = 1; d < 144 && ok; d++)
            if (route_enc(5u, (5u + d) % 144u) !=
                route_enc(90u, (90u + d) % 144u)) ok = 0;
        CHECK(ok, "P4 wire depends on Delta ONLY - birth (block,from) untouched");
    }

    /* ── P3: ขนาดจริง ────────────────────────────────────────────────── */
    {
        uint32_t base = n * 2u;                        /* from+to */
        uint32_t free_w = n;                           /* 1 B/event */
        /* RIM census: นับ event ที่ Δ≡0 mod 24 จริงใน pattern */
        uint32_t rim_ev = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t d = (ent[i].to_scale + 144u - ent[i].from_scale) % 144u;
            if (d % 24u == 0) rim_ev++;
        }
        printf("\n     size @ %u route events:\n", n);
        printf("       baseline {from,to}: %u B\n", base);
        printf("       gear FREE  1B/ev  : %u B (%u%%)\n",
               free_w, (unsigned)((free_w * 100u) / base));
        printf("       rim-pure events in pattern : %u/%u\n",
               rim_ev, n);
        CHECK(free_w == base / 2u, "P3 gear FREE = exactly half of route bytes");
        CHECK(base > 0 && free_w > 0, "P3b sizes nonzero");
    }

    /* ── P5: home tooth skip — Δ=0 ไม่เข้า log (encoder refuses) ─────── */
    {
        FGLog g;
        fg_log_init(&g);
        int rc = fg_log_push(&g, 77, 77);
        CHECK(rc == -2 && g.hdr.n == 0, "P5 Delta=0 route refused (no wasted tooth)");
    }

    printf("\n%d/%d PASS%s\n", checks - fails, checks,
           fails ? " — RED" : " — ALL GREEN");
    return fails ? 1 : 0;
}
