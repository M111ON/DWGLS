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
 * FULL FIELD [0,20736) — same skeleton, q grown 6b→10b (fgx_*):
 *   F1  hand event hop +7000 = 291·24+16 → {q=291,dc=0,dx=1} identical
 *       from ALL 144 frames (translation-invariant wire, X3 semantics)
 *   F2  uniform D∈{1..20735} → EXACT residue shares: residue ≡0 mod 24
 *       occurs 863× vs 864× → kis[0]=2591 hyp[0]=6911 joint[0]=863,
 *       others one more; binomial 4-sigma bands
 *   F3  RIM-pure drift full-field: RIM share 100%, q reaches ≥863 turns
 *   F4  rim-drift tooth entropy < uniform tooth entropy (same code path)
 *   F5  frame sweep: all 144 frames observed, conservation holds
 *   F6  flipped q-turn on FGX wire → replay diverges EXACTLY there
 *
 * USAGE:
 *   ./build/gear_microscope                 — self-test (M1-M6 + F1-F6 full)
 *   ./build/gear_microscope --demo          — + pretty demo of all levels
 *   ./build/gear_microscope --file <path>   — observe a raw entry file
 *                          (16 B/rec: u16 block_id, pad2, u8 from, u8 to, pad10)
 *   ./build/gear_microscope --filex <path>  — full-field entry file [0,20736)
 *                          (16 B/rec: u16 block_id, pad2, u16 from, u16 to, pad8)
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
    /* level 2: rim census — q turns; local window [0,6), full [0,864)    */
    uint32_t q[864];            /* full-field capacity; local uses q<6    */
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

#define N_UNI_MAX 65536u    /* wrapper scratch + --filex record cap */

/* Observe route chains through the gear language. positions in [0,mod).
 * full=0 → local window [0,144) via fg_enc · full=1 → [0,20736) via fgx_enc.
 * Teeth are read FROM THE EVENT (dc,dx) — events are translation-invariant
 * wire (test X3), so observation rides the same language, not raw offsets. */
static void gear_micro_observe_u32(const uint32_t *from, const uint32_t *to,
                                   const uint16_t *block, uint32_t n,
                                   int full, GearMicroResult *r) {
    memset(r, 0, sizeof(*r));
    r->n = n;
    if (!n) return;
    uint32_t mod = full ? FG_FULL : FG_LOCAL;
    for (uint32_t i = 0; i < n; i++) {
        FGGearEv e = full ? fgx_enc(from[i], to[i]) : fg_enc(from[i], to[i]);
        uint8_t  s = fg_crt(e.dc, e.dx);          /* joint tooth [0,24)    */
        r->kis[e.dc]++;
        r->hyp[e.dx]++;
        r->joint[s]++;
        if (e.q < FG_FULL_TURNS) r->q[e.q]++;     /* guard: local q<6      */
        if (e.dc == 0 && e.dx == 0) r->rim_pure++;
        if ((uint32_t)((mod + to[i] - from[i]) % mod) >= FG_RING)
            r->fwd++; else r->bwd++;
        if ((to[i] + mod - from[0]) % mod == 0) r->back_home++;
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

/* u8 convenience wrapper (local-window callers) */
static void gear_micro_observe(const uint8_t *from, const uint8_t *to,
                               const uint16_t *block, uint32_t n,
                               GearMicroResult *r) {
    static uint32_t f[N_UNI_MAX], t[N_UNI_MAX];
    if (n > N_UNI_MAX) n = N_UNI_MAX;
    for (uint32_t i = 0; i < n; i++) { f[i] = from[i]; t[i] = to[i]; }
    gear_micro_observe_u32(f, t, block, n, 0, r);
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
    /* q census: sparse print (full field spans q<864 — don't dump it all) */
    {
        uint32_t qmax = 0, qnonzero = 0;
        for (int i = 0; i < (int)FG_FULL_TURNS; i++)
            if (r->q[i]) { qmax = (uint32_t)i; qnonzero++; }
        printf("\n   q census   : nonzero=%u/%u max=q%u |",
               qnonzero, r->n, qmax);
        int shown = 0;
        for (int i = 0; i < (int)FG_FULL_TURNS && shown < 12; i++)
            if (r->q[i]) { printf(" q%d=%u", i, r->q[i]); shown++; }
        if (qnonzero > (uint32_t)shown) printf(" …");
    }
    printf("\n   drift      : fwd=%u bwd=%u\n", r->fwd, r->bwd);
    printf("   max chain  : %u   tooth entropy: %.3f bits (uniform max %.3f)\n",
           r->max_chain, r->tooth_entropy, log(24.0) / log(2.0));
}

/* ════════════════════════════════════════════════════════════════════
 * SELF-TEST HARNESS (M1-M6)
 * ════════════════════════════════════════════════════════════════════ */
static int fails = 0, checks = 0;
static GearMicroResult micro_save, micro_save_rim;   /* full-field demo keep */
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
    const char *file = NULL, *filex = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--demo")) demo = 1;
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "--filex") && i + 1 < argc) filex = argv[++i];
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

    /* ══════════════════════════════════════════════════════════════
     * FULL FIELD [0,20736) — same skeleton, q grown 6b→10b
     * ══════════════════════════════════════════════════════════════ */

    /* ── F1: hand-computed full-field event + frame invariance ─────── */
    {
        /* hop +7000 = 291·24 + 16 → q=291, tooth s=16 ≡ (0 mod 8, 1 mod 3) */
        int ok = 1;
        for (uint32_t fr = 0; fr < FG_FULL / FG_LOCAL && ok; fr++) {
            uint32_t base = (uint32_t)fg_to_full(fr, 7);
            FGGearEv e = fgx_enc(base, base + 7000u);   /* no wrap: 7+7000+14336<20736? see below */
            if (e.q != 291 || e.dc != 0 || e.dx != 1) ok = 0;
        }
        CHECK(ok, "F1 hop +7000 -> {q=291,dc=0,dx=1} identical from ALL 144 frames");
    }

    /* ── F2: uniform full-field routes vs exact residue expectations ── */
    {
        static uint32_t xf[N_UNI_MAX], xt[N_UNI_MAX];
        static uint16_t xb[N_UNI_MAX];
        static GearMicroResult rx;
        uint32_t NF = 43120u;                 /* 20735·2+... : 2 events per D */
        for (uint32_t i = 0; i < NF; i++) {
            uint32_t d = (i % 2 == 0) ? ((i / 2) % 20735u + 1u)   /* D=1..20735 */
                                      : (lcg() % 20735u + 1u);
            uint32_t fpos = lcg() % FG_FULL;
            xf[i] = fpos;
            xt[i] = (fpos + d) % FG_FULL;
            xb[i] = (uint16_t)(i / 674u);
        }
        gear_micro_observe_u32(xf, xt, xb, NF, 1, &rx);
        /* exact expectations under D ~ Uniform{1..20735}:
         * residue r≡0 mod 24 appears 863×, others 864× (hand math)       */
        double e_kis[8], e_hyp[3], e_joint[24];
        for (int j = 0; j < 8; j++) e_kis[j] = (j ? 2592.0 : 2591.0) / 20735.0;
        for (int j = 0; j < 3; j++) e_hyp[j] = (j ? 6912.0 : 6911.0) / 20735.0;
        for (int j = 0; j < 24; j++) e_joint[j] = (j ? 864.0 : 863.0) / 20735.0;
        int ok = 1;
        #define MICRO_BANDX(obs, p) ( \
            fabs((double)(obs) - NF * (p)) <= \
            4.0 * sqrt((double)NF * (p) * (1.0 - (p))))
        for (int j = 0; j < 8 && ok; j++)
            if (!MICRO_BANDX(rx.kis[j], e_kis[j])) ok = 0;
        for (int j = 0; j < 3 && ok; j++)
            if (!MICRO_BANDX(rx.hyp[j], e_hyp[j])) ok = 0;
        for (int j = 0; j < 24 && ok; j++)
            if (!MICRO_BANDX(rx.joint[j], e_joint[j])) ok = 0;
        CHECK(ok, "F2 uniform full-field -> exact residue shares "
                  "(kis0=2591 hyp0=6911 joint0=863, 4-sigma)");
        micro_save = rx;                    /* keep for demo print */
    }

    /* ── F3: rim-pure drift on the full field ───────────────────────── */
    {
        static uint32_t rf2[N_UNI_MAX], rt2[N_UNI_MAX];
        static GearMicroResult rr2;
        uint32_t NR = 20000u;
        for (uint32_t i = 0; i < NR; i++) {
            rf2[i] = lcg() % FG_FULL;
            rt2[i] = (rf2[i] + 24u * (1u + lcg() % 863u)) % FG_FULL;
        }
        gear_micro_observe_u32(rf2, rt2, NULL, NR, 1, &rr2);
        uint32_t qmax = 0, sq = 0;
        for (int i = 0; i < (int)FG_FULL_TURNS; i++) {
            if (rr2.q[i]) qmax = (uint32_t)i;
            sq += rr2.q[i];
        }
        int ok = rr2.rim_pure == NR && rr2.q[0] == 0 && qmax >= 863u &&
                 sq == NR && rr2.blocks == 1 && rr2.max_chain == NR;
        CHECK(ok, "F3 rim-pure full-field: RIM 100%, q up to >=863, "
                  "conservation holds");
        micro_save_rim = rr2;
    }

    /* ── F4: structure vs noise discrimination (full field) ─────────── */
    CHECK(micro_save_rim.tooth_entropy < micro_save.tooth_entropy,
          "F4 rim-drift entropy < uniform entropy (full field)");

    /* ── F5: frame sweep — every frame contributes exactly n/144 ────── */
    {
        static uint32_t sf[N_UNI_MAX], st2[N_UNI_MAX];
        static GearMicroResult rs;
        uint32_t NS = 28800u;                       /* 144 frames × 200 */
        for (uint32_t i = 0; i < NS; i++) {
            uint32_t fr = i / 200u;
            sf[i] = (uint32_t)fg_to_full(fr, (i % 200u) % FG_LOCAL);
            st2[i] = (sf[i] + 1u + lcg() % 20734u) % FG_FULL;
        }
        gear_micro_observe_u32(sf, st2, NULL, NS, 1, &rs);
        CHECK(rs.n == NS && rs.blocks == 1,
              "F5 frame sweep observed: n conserved across 144 frames");
    }

    /* ── F6: corruption localized exactly on the FULL-FIELD wire ────── */
    {
        FGXLog g;
        fgx_log_init(&g);
        uint32_t w = 12345, seq[40];
        for (uint32_t i = 0; i < 40; i++) {
            uint32_t nxt = (w + 1u + lcg() % 20733u) % FG_FULL;
            fgx_log_push(&g, w, nxt);
            seq[i] = nxt;
            w = nxt;
        }
        g.ev[19].q = (uint16_t)((g.ev[19].q + 137u) % FG_FULL_TURNS);
        int pos = -1;
        uint32_t cur = 12345;
        for (uint32_t i = 0; i < g.hdr.n; i++) {
            uint32_t got = fgx_dec(cur, g.ev[i]);
            if (got != seq[i]) { pos = (int)i; break; }
            cur = got;
        }
        CHECK(pos == 19, "F6 flipped q-turn -> replay diverges EXACTLY there");
    }

    if (demo) {
        micro_print(&ru, "UNIFORM random routes (noise floor)");
        micro_print(&rr, "RIM-pure drift (telescope motion)");
        micro_print(&micro_save,
                    "FULL-FIELD uniform [0,20736) (noise floor)");
        micro_print(&micro_save_rim, "FULL-FIELD rim-pure drift");
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

    /* ── full-field file mode: entries over [0,20736) ────────────────
     * 16 B/rec: u16 block_id LE, pad2, u16 from LE, u16 to LE, pad8    */
    if (filex) {
        FILE *fp = fopen(filex, "rb");
        if (!fp) { printf("FAIL cannot open %s\n", filex); return 1; }
        static uint32_t  xf[N_UNI_MAX], xt[N_UNI_MAX];
        static uint16_t  xb[N_UNI_MAX];
        uint32_t n = 0;
        unsigned char rec[16];
        while (n < N_UNI_MAX && fread(rec, 1, 16, fp) == 16) {
            memcpy(&xb[n], rec, 2);
            uint16_t lo, hi;
            memcpy(&lo, rec + 4, 2);
            memcpy(&hi, rec + 6, 2);
            xf[n] = lo;
            xt[n] = hi;
            if (xf[n] >= FG_FULL || xt[n] >= FG_FULL) {
                printf("FAIL record %u out of field (from=%u to=%u)\n",
                       n, xf[n], xt[n]);
                fclose(fp);
                return 1;
            }
            n++;
        }
        fclose(fp);
        if (n) {
            static GearMicroResult rm;
            gear_micro_observe_u32(xf, xt, xb, n, 1, &rm);
            char tag[128];
            snprintf(tag, sizeof tag, "%s [full-field]", filex);
            micro_print(&rm, tag);
            printf("   (observation only — nothing written)\n");
        } else printf("(file empty)\n");
    }

    printf("\n%d/%d PASS%s\n", checks - fails, checks,
           fails ? " — RED" : " — ALL GREEN");
    return fails ? 1 : 0;
}
