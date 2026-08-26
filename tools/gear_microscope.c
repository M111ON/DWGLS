/*
 * gear_microscope.c — Microscope end-state B: observation tool on the GEAR
 * skeleton (same skeleton as the wire: fan24_gear.h x GhostLog route log).
 *
 * OBSERVATION, not compression. MAP not COMPRESS.
 *   A = inference hook (rid-graft: llama reads weight storage via RID) done
 *   B = microscope on the SAME skeleton — this tool.
 *
 * Field stays still, data moves through it; the microscope only LOOKS.
 *
 * WHAT IT OBSERVES — route logs through the gear language {q,dc,dx}:
 *   D = (to - from + 144) % 144 ; D = 24q + r ; r == (dc mod 8, dx mod 3)
 *
 * ZOOM OUT ladder (microscope = zoom OUT, not in):
 *   level 0  EVENT : one hop = one gear event {q,dc,dx}
 *   level 1  TOOTH : KIS cube-wheel census (s%8) + HYP axis census (s%3)
 *                    + joint tooth s=fg_crt(dc,dx) in [0,24)
 *   level 2  RIM   : q-turn census over [0,144) window, RIM share
 *   level 3  FIELD : per-block chain shape — chain length, drift balance,
 *                    return-to-birth count
 *
 * ORACLE (independent, hand math):
 *   M1  fg_crt == brute force (tripwire — same as ghost_gear_probe P0)
 *   M2  synthetic UNIFORM random routes -> wheel censuses ~uniform:
 *       KisTooth ~= n/8 +-5%, HypTooth ~= n/3 +-5%, JointTooth ~= n/24 +-8%
 *       (explicit hand bounds, no stats lib)
 *   M3  synthetic RIM-pure pattern (all D==0 mod 24) -> dc==dx==0 for ALL
 *       events, RIM share == 100%, q census only q>=1 nonzero
 *   M4  STRUCTURED vs RANDOM discrimination: structured pattern (rim drift)
 *       has LOWER tooth entropy than uniform; both via the SAME code path
 *   M5  zoom-out conservation: sum(kis)==sum(hyp)==sum(joint)==sum(q)==n,
 *       block count sane
 *   M6  mutation tripwire: corrupt ONE wire byte -> replay diverges at that
 *       event index EXACTLY (fail-loud localization)
 *
 * USAGE:
 *   ./build/gear_microscope                 — self-test (M1-M6) on synthetics
 *   ./build/gear_microscope --demo          — + pretty demo of all levels
 *   ./build/gear_microscope --file <path>   — observe a raw entry file
 *                          (16 B/rec: u16 block_id, pad2, u8 from, u8 to, pad10)
 *
 * BUILD:
 *   gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format
 *       -I. -Icore -o build/gear_microscope tools/gear_microscope.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "../core/fan24_gear.h"

/* ════════════════════════════════════════════════════════════════════
 * OBSERVATION RESULT — one struct filled by observe(), printed per level
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t n;                 /* events observed                        */
    /* level 1: tooth censuses */
    uint32_t kis[8];            /* s%8 cube wheel                         */
    uint32_t hyp[3];            /* s%3 axis wheel                         */
    uint32_t joint[24];         /* fg_crt(dc,dx): full tooth id           */
    /* level 2: rim census */
    uint32_t q[6];              /* q in [0..5] (D<=143 -> q<=5)           */
    uint32_t rim_pure;          /* events with dc==0 && dx==0             */
    /* level 3: field shape */
    uint32_t blocks;            /* distinct block chains                  */
    uint32_t max_chain;         /* longest chain                          */
    uint32_t back_home;         /* events landing on chain birth scale    */
    uint32_t fwd, bwd;          /* drift direction balance                */
    double   tooth_entropy;     /* Shannon bits of joint census           */
} GearMicroResult;

/* Shannon entropy of a census (bits). Max log2(k). Independent math. */
static double micro_entropy(const uint32_t *c, uint32_t k, uint32_t total) {
    if (!total) return 0.0;
    double H = 0.0;
    for (uint32_t i = 0; i < k; i++) {
        if (c[i]) {
            double p = (double)c[i] / (double)total;
            H -= p * log(p) / log(2.0);
        }
    }
    return H;
}

/* Observe a route chain through the gear language. entries=(from,to),
 * optional block ids (NULL = single block). Pure READ path: encode-only. */
static void gear_micro_observe(const uint8_t *from, const uint8_t *to,
                               const uint16_t *block, uint32_t n,
                               GearMicroResult *r) {
    memset(r, 0, sizeof(*r));
    r->n = n;
    if (!n) return;
    for (uint32_t i = 0; i < n; i++) {
        FGGearEv e = fg_enc(from[i], to[i]);
        uint8_t  s = fg_crt(e.dc, e.dx);          /* joint tooth [0,24)    */
        r->kis[e.dc]++;
        r->hyp[e.dx]++;
        r->joint[s]++;
        r->q[e.q]++;
        if (e.dc == 0 && e.dx == 0) r->rim_pure++;
        if ((uint32_t)((FG_LOCAL + to[i] - from[i]) % FG_LOCAL) >= FG_RING)
            r->fwd++; else r->bwd++;
        if (to[i] == from[0]) r->back_home++;
    }
    /* field shape: chains per block (contiguous runs of equal block id) */
    if (block) {
        uint16_t cur = block[0];
        uint32_t len = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (block[i] != cur) {
                if (len > r->max_chain) r->max_chain = len;
                cur = block[i];
                len = 1;
            } else {
                len++;
            }
        }
        if (len > r->max_chain) r->max_chain = len;
        uint16_t last = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (i == 0 || block[i] != last) { r->blocks++; last = block[i]; }
        }
    } else {
        r->blocks = 1;
        r->max_chain = n;
    }
    r->tooth_entropy = micro_entropy(r->joint, 24, n);
}

static void micro_print(const GearMicroResult *r, const char *tag) {
    printf("\n-- microscope @ %s (n=%u events, %u blocks) --\n",
           tag, r->n, r->blocks);
    printf("   KIS wheel  (s%%8):");
    for (int i = 0; i < 8; i++) printf(" %u", r->kis[i]);
    printf("\n   HYP wheel  (s%%3):");
    for (int i = 0; i < 3; i++) printf(" %u", r->hyp[i]);
    printf("\n   RIM share  : %u/%u (%u%%)", r->rim_pure, r->n,
           r->n ? (unsigned)(r->rim_pure * 100u / r->n) : 0u);
    printf("\n   q census   :");
    for (int i = 0; i < 6; i++) printf(" q%d=%u", i, r->q[i]);
    printf("\n   drift      : fwd=%u bwd=%u\n", r->fwd, r->bwd);
    printf("   max chain  : %u   tooth entropy: %.3f bits (uniform max %.3f)\n",
           r->max_chain, r->tooth_entropy, log(24.0) / log(2.0));
}

/* ════════════════════════════════════════════════════════════════════
 * SELF-TEST HARNESS (M1-M6)
 * ════════════════════════════════════════════════════════════════════ */
static int fails = 0, checks = 0;
#define CHECK(cond, name) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL %s\n", name); } \
    else printf("ok   %s\n", name); \
} while (0)

#define N_UNI 19200u

/* NOTE: plain LCG has short cycles in LOW bits (bit k period 2^(k+1)) — the
 * microscope CAUGHT this on the first run (all events landed on one KIS
 * tooth). Mix high bits down so synthetic "uniform" is actually uniform. */
static uint32_t st_ = 20260826u;
static uint32_t lcg(void) {
    st_ = st_ * 1664525u + 1013904223u;
    return (st_ >> 9) ^ (st_ >> 20);
}

int main(int argc, char **argv) {
    int demo = 0;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--demo")) demo = 1;
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
    }

    printf("gear_microscope — observation on the gear skeleton (end-state B)\n");
    printf("====================================================\n");

    /* ── M1: CRT tripwire ──────────────────────────────────────────── */
    {
        int ok = 1;
        for (uint8_t dc = 0; dc < 8 && ok; dc++)
            for (uint8_t dx = 0; dx < 3 && ok; dx++) {
                uint8_t s = fg_crt(dc, dx);
                if (s >= 24 || s % 8u != dc || s % 3u != dx) ok = 0;
                for (uint8_t t = (uint8_t)(s + 1u); t < 24; t++)
                    if (t % 8u == dc && t % 3u == dx) ok = 0;
            }
        CHECK(ok, "M1 fg_crt == brute-force oracle (tripwire)");
    }

    /* ── synthetic UNIFORM routes ────────────────────────────────────
     * NOTE: ut != uf excludes D=0 -> D is uniform over 1..143, so bucket
     * expectations are NOT flat: kis[0] holds only the 17 nonzero
     * multiples of 8 (vs 18 values per other tooth); hyp[0] holds 47
     * multiples of 3 (vs 48); joint[0] holds 5 (vs 6). Hand-derived
     * exact shares below — the microscope SEES this skew, so the oracle
     * must encode it, not assume flatness. */
    static uint8_t  uf[N_UNI], ut[N_UNI];
    static uint16_t ub[N_UNI];
    static GearMicroResult ru;
    for (uint32_t i = 0; i < N_UNI; i++) {
        uf[i] = (uint8_t)(lcg() % 144u);
        uint32_t d = 1u + lcg() % 143u;            /* D uniform in 1..143 */
        ut[i] = (uint8_t)((uf[i] + d) % 144u);
        ub[i] = (uint16_t)(i / 300u);              /* 64 blocks x 300     */
    }
    gear_micro_observe(uf, ut, ub, N_UNI, &ru);

    /* ── M2: uniformity against EXACT residue-arithmetic expectations ─ */
    {
        /* expected counts from D ~ Uniform{1..143} (pure hand math)   */
        double e_kis[8], e_hyp[3], e_joint[24];
        for (int j = 0; j < 8; j++) {
            int cnt = 0;
            for (int d = 1; d <= 143; d++) if ((d % 24) % 8 == j) cnt++;
            e_kis[j] = (double)cnt / 143.0;
        }
        for (int j = 0; j < 3; j++) {
            int cnt = 0;
            for (int d = 1; d <= 143; d++) if ((d % 24) % 3 == j) cnt++;
            e_hyp[j] = (double)cnt / 143.0;
        }
        for (int j = 0; j < 24; j++) {
            int cnt = 0;
            for (int d = 1; d <= 143; d++) if ((d % 24) == j) cnt++;
            e_joint[j] = (double)cnt / 143.0;
        }
        int ok = 1;
        /* binomial 4-sigma band: |obs - n*p| <= 4*sqrt(n p (1-p))      */
        #define MICRO_BAND(obs, p) ( \
            fabs((double)(obs) - N_UNI * (p)) <= \
            4.0 * sqrt((double)N_UNI * (p) * (1.0 - (p))))
        for (int j = 0; j < 8 && ok; j++)
            if (!MICRO_BAND(ru.kis[j], e_kis[j])) ok = 0;
        for (int j = 0; j < 3 && ok; j++)
            if (!MICRO_BAND(ru.hyp[j], e_hyp[j])) ok = 0;
        for (int j = 0; j < 24 && ok; j++)
            if (!MICRO_BAND(ru.joint[j], e_joint[j])) ok = 0;
        CHECK(ok, "M2 uniform routes -> wheels match exact residue "
                  "expectations (4-sigma)");
    }

    /* ── synthetic RIM-pure drift ──────────────────────────────────── */
    static uint8_t rf[N_UNI], rt[N_UNI];
    static GearMicroResult rr;
    for (uint32_t i = 0; i < N_UNI; i++) {
        rf[i] = (uint8_t)(lcg() % 144u);
        rt[i] = (uint8_t)((rf[i] + 24u * (1u + lcg() % 5u)) % 144u);
    }
    gear_micro_observe(rf, rt, NULL, N_UNI, &rr);

    /* ── M3: rim-pure detection ────────────────────────────────────── */
    CHECK(rr.rim_pure == N_UNI && rr.q[0] == 0,
          "M3 rim-pure pattern -> RIM share 100%, no off-rim tooth");

    /* ── M4: structure vs noise discrimination (same code path) ────── */
    CHECK(rr.tooth_entropy < ru.tooth_entropy,
          "M4 structured (rim drift) entropy < uniform entropy");

    /* ── M5: conservation across levels ────────────────────────────── */
    {
        uint32_t s8 = 0, s3 = 0, s24 = 0, sq = 0;
        for (int i = 0; i < 8; i++)  s8  += ru.kis[i];
        for (int i = 0; i < 3; i++)  s3  += ru.hyp[i];
        for (int i = 0; i < 24; i++) s24 += ru.joint[i];
        for (int i = 0; i < 6; i++)  sq  += ru.q[i];
        CHECK(s8 == N_UNI && s3 == N_UNI && s24 == N_UNI && sq == N_UNI &&
              ru.blocks == 64 && ru.max_chain == 300,
              "M5 conservation: every level sums to n; field shape exact");
    }

    /* ── M6: corruption localized exactly (fail-loud tripwire) ─────── */
    {
        FGLog g;
        fg_log_init(&g);
        uint32_t w = 10, seq[FG_LOG_CAP];
        for (uint32_t i = 0; i < 32; i++) {
            uint32_t nxt = (w + 1u + lcg() % 140u) % 144u;
            fg_log_push(&g, w, nxt);
            seq[i] = nxt;
            w = nxt;
        }
        g.ev[7].dc = (uint8_t)((g.ev[7].dc + 1u) % 8u);   /* flip one tooth */
        int pos = -1;
        uint32_t cur = 10;
        for (uint32_t i = 0; i < g.hdr.n; i++) {
            uint32_t got = fg_dec(cur, g.ev[i]);
            if (got != seq[i]) { pos = (int)i; break; }
            cur = got;
        }
        CHECK(pos == 7, "M6 single flipped tooth -> replay diverges EXACTLY there");
    }

    if (demo) {
        micro_print(&ru, "UNIFORM random routes (noise floor)");
        micro_print(&rr, "RIM-pure drift (telescope motion)");
    }

    /* ── optional file mode: observe real entry dumps ──────────────── */
    if (file) {
        FILE *fp = fopen(file, "rb");
        if (!fp) { printf("FAIL cannot open %s\n", file); return 1; }
        static uint8_t   ff[65536], tt[65536];
        static uint16_t  bb[65536];
        uint32_t n = 0;
        unsigned char rec[16];
        while (n < 65536 && fread(rec, 1, 16, fp) == 16) {
            memcpy(&bb[n], rec, 2);
            ff[n] = rec[4];
            tt[n] = rec[5];
            n++;
        }
        fclose(fp);
        if (n) {
            static GearMicroResult rm;
            gear_micro_observe(ff, tt, bb, n, &rm);
            micro_print(&rm, file);
            printf("   (observation only — nothing written)\n");
        } else printf("(file empty)\n");
    }

    printf("\n%d/%d PASS%s\n", checks - fails, checks,
           fails ? " — RED" : " — ALL GREEN");
    return fails ? 1 : 0;
}
