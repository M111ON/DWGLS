/*
 * tw_rdh_bench.c — head-to-head: tw_capture vs rdh_capture, cycles/op.
 *
 * Both functions are inlined verbatim below (self-contained, no external
 * headers needed) so the compile is one translation unit and the compiler
 * sees exactly the code under test. Constants taken from the real sources:
 *   collection/tw/tw_capture_int.h  (TW_* geometry)
 *   core/rdh_capture.h + core/rdh_addr.h (rdh_capture / rdh_key)
 *
 * Method: same as tools/rdh_bench.c — rdtsc + lfence, min-of-9 trials
 * over 400k iterations, so each reported number is the *minimum* observed
 * cycle cost (best case, excludes scheduler noise).
 *
 * Three inputs are benched:
 *   tw_capture_int          (hex grid only)
 *   tw_capture_int_combined (hex + tri = 12 centroids/sector — the full path)
 *   rdh_capture             (geometric walk, RDH_CAPTURE_144)
 *   fnv1a                   (reference: the name-hash it replaces)
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o build/tw_rdh_bench tools/tw_rdh_bench.c
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════
   TW_ constants — from collection/tw/tw_capture_int.h + docs
   ══════════════════════════════════════════════════════════════════ */
#define TW_SCALE       207360   /* 12^4 * 10 */
#define TW_N_SECTORS   10       /* decagram: pentagon-pair, 36 deg each */
#define TW_SLOTS_PER   6        /* hex cast, 60 deg */
#define TW_COMBINED_SLOTS 12    /* hex(0..5) + tri(6..11) */
#define TW_MARGIN_NUM  1
#define TW_MARGIN_DEN  8

static const int32_t TW_BOUNDARY_DIR[TW_N_SECTORS][2] = {
    {      0, 207360},
    { 121883, 167758},
    { 197211,  64078},
    { 197211, -64078},
    { 121883,-167758},
    {      0,-207360},
    {-121883,-167758},
    {-197211, -64078},
    {-197211,  64078},
    {-121883, 167758},
};

static const int32_t TW_TRI_SLOT_LOCAL_I[TW_N_SECTORS][TW_SLOTS_PER][2] = {
  {{ 119232, 206517},{  88127, 206517},{  72575, 179580},{  88128, 152643},{ 119232, 152643},{ 134784, 179580}},
  {{ 217848,  96993},{ 192684, 115274},{ 164269, 102624},{ 161018,  71690},{ 186181,  53407},{ 214597,  66059}},
  {{ 233253, -49580},{ 223642, -19998},{ 193218, -13532},{ 172404, -36646},{ 182016, -66228},{ 212441, -72695}},
  {{ 159564,-177213},{ 169176,-147632},{ 148363,-124517},{ 117938,-130984},{ 108328,-160566},{ 129140,-183681}},
  {{  24926,-237160},{  50090,-218877},{  46839,-187942},{  18424,-175292},{  -6740,-193574},{  -3488,-224507}},
  {{-119232,-206517},{ -88128,-206517},{ -72576,-179580},{ -88128,-152643},{-119233,-152643},{-134785,-179580}},
  {{-217849, -96994},{-192685,-115275},{-164270,-102625},{-161019, -71691},{-186182, -53408},{-214598, -66060}},
  {{-233254,  49579},{-223643,  19997},{-193219,  13531},{-172405,  36645},{-182017,  66227},{-212442,  72694}},
  {{-159565, 177212},{-169177, 147631},{-148364, 124516},{-117939, 130983},{-108329, 160565},{-129141, 183680}},
  {{ -24927, 237159},{ -50091, 218876},{ -46840, 187941},{ -18425, 175291},{   6739, 193573},{   3487, 224506}}
};

static const int32_t TW_SLOT_LOCAL_I[TW_N_SECTORS][TW_SLOTS_PER][2] = {
  {{      0, 238464},{ -26937, 222912},{ -26937, 191808},{      0, 176256},{  26937, 191808},{  26937, 222912}},
  {{ 140166, 192922},{ 109232, 196172},{  90949, 171009},{ 103601, 142594},{ 134534, 139342},{ 152817, 164507}},
  {{ 226792,  73689},{ 203678,  94502},{ 174097,  84890},{ 167629,  54466},{ 190744,  33653},{ 220326,  43265}},
  {{ 226792, -73689},{ 220326, -43265},{ 190744, -33653},{ 167629, -54466},{ 174097, -84890},{ 203678, -94502}},
  {{ 140166,-192922},{ 152817,-164507},{ 134534,-139342},{ 103601,-142594},{  90949,-171009},{ 109232,-196172}},
  {{      0,-238464},{  26937,-222912},{  26937,-191808},{      0,-176256},{ -26937,-191808},{ -26937,-222912}},
  {{-140166,-192922},{-109232,-196172},{ -90949,-171009},{-103601,-142594},{-134534,-139342},{-152817,-164507}},
  {{-226792, -73689},{-203678, -94502},{-174097, -84890},{-167629, -54466},{-190744, -33653},{-220326, -43265}},
  {{-226792,  73689},{-220326,  43265},{-190744,  33653},{-167629,  54466},{-174097,  84890},{-203678,  94502}},
  {{-140166, 192922},{-152817, 164507},{-134534, 139342},{-103601, 142594},{ -90949, 171009},{-109232, 196172}}
};

static const int32_t TW_COMBINED_GRID[TW_N_SECTORS][TW_COMBINED_SLOTS][2] = {
  {{      0, 238464},{ -26937, 222912},{ -26937, 191808},{      0, 176256},{  26937, 191808},{  26937, 222912},{ 119232, 206517},{  88127, 206517},{  72575, 179580},{  88128, 152643},{ 119232, 152643},{ 134784, 179580}},
  {{ 140166, 192922},{ 109232, 196172},{  90949, 171009},{ 103601, 142594},{ 134534, 139342},{ 152817, 164507},{ 217848,  96993},{ 192684, 115274},{ 164269, 102624},{ 161018,  71690},{ 186181,  53407},{ 214597,  66059}},
  {{ 226792,  73689},{ 203678,  94502},{ 174097,  84890},{ 167629,  54466},{ 190744,  33653},{ 220326,  43265},{ 233253, -49580},{ 223642, -19998},{ 193218, -13532},{ 172404, -36646},{ 182016, -66228},{ 212441, -72695}},
  {{ 226792, -73689},{ 220326, -43265},{ 190744, -33653},{ 167629, -54466},{ 174097, -84890},{ 203678, -94502},{ 159564,-177213},{ 169176,-147632},{ 148363,-124517},{ 117938,-130984},{ 108328,-160566},{ 129140,-183681}},
  {{ 140166,-192922},{ 152817,-164507},{ 134534,-139342},{ 103601,-142594},{  90949,-171009},{ 109232,-196172},{  24926,-237160},{  50090,-218877},{  46839,-187942},{  18424,-175292},{  -6740,-193574},{  -3488,-224507}},
  {{      0,-238464},{  26937,-222912},{  26937,-191808},{      0,-176256},{ -26937,-191808},{ -26937,-222912},{-119232,-206517},{ -88128,-206517},{ -72576,-179580},{ -88128,-152643},{-119233,-152643},{-134785,-179580}},
  {{-140166,-192922},{-109232,-196172},{ -90949,-171009},{-103601,-142594},{-134534,-139342},{-152817,-164507},{-217849, -96994},{-192685,-115275},{-164270,-102625},{-161019, -71691},{-186182, -53408},{-214598, -66060}},
  {{-226792, -73689},{-203678, -94502},{-174097, -84890},{-167629, -54466},{-190744, -33653},{-220326, -43265},{-233254,  49579},{-223643,  19997},{-193219,  13531},{-172405,  36645},{-182017,  66227},{-212442,  72694}},
  {{-226792,  73689},{-220326,  43265},{-190744,  33653},{-167629,  54466},{-174097,  84890},{-203678,  94502},{-159565, 177212},{-169177, 147631},{-148364, 124516},{-117939, 130983},{-108329, 160565},{-129141, 183680}},
  {{-140166, 192922},{-152817, 164507},{-134534, 139342},{-103601, 142594},{ -90949, 171009},{-109232, 196172},{ -24927, 237159},{ -50091, 218876},{ -46840, 187941},{ -18425, 175291},{   6739, 193573},{   3487, 224506}}
};

typedef struct {
    uint8_t zone, slot;
    int64_t resid_x, resid_y;
    uint8_t drain, drain_zone, drain_slot;
    int64_t drain_resid_x, drain_resid_y;
} TWCaptureInt;

static inline int64_t _tw_cross(int32_t ax, int32_t ay, int64_t bx, int64_t by)
{ return (int64_t)ax*by - (int64_t)ay*bx; }
static inline int64_t _tw_mag2(int64_t x, int64_t y) { return x*x + y*y; }
static inline int64_t _tw_abs64(int64_t v) { return v < 0 ? -v : v; }

static inline void _tw_pick_slot_on(int64_t vx, int64_t vy, int sector,
                                     const int32_t centroids[TW_N_SECTORS][TW_SLOTS_PER][2],
                                     uint8_t *slot_out, int64_t *rx, int64_t *ry)
{
    int best = 0; int64_t bd = -1;
    for (int j = 0; j < TW_SLOTS_PER; j++) {
        int64_t dx = vx - centroids[sector][j][0];
        int64_t dy = vy - centroids[sector][j][1];
        int64_t d = dx*dx + dy*dy;
        if (bd < 0 || d < bd) { bd = d; best = j; }
    }
    *slot_out = (uint8_t)(sector*TW_SLOTS_PER + best);
    *rx = vx - centroids[sector][best][0];
    *ry = vy - centroids[sector][best][1];
}

static inline int _tw_find_sector(int64_t vx, int64_t vy, int64_t *out_mincross_abs)
{
    int64_t cross[TW_N_SECTORS];
    for (int k = 0; k < TW_N_SECTORS; k++)
        cross[k] = _tw_cross(TW_BOUNDARY_DIR[k][0], TW_BOUNDARY_DIR[k][1], vx, vy);
    int sector = 0; int64_t mincross = -1;
    for (int k = 0; k < TW_N_SECTORS; k++) {
        int kn = (k+1) % TW_N_SECTORS;
        if (cross[k] <= 0 && cross[kn] >= 0) sector = k;
        int64_t a = _tw_abs64(cross[k]);
        if (mincross < 0 || a < mincross) mincross = a;
    }
    *out_mincross_abs = mincross;
    return sector;
}

/* full hex-only path (sector + 6-slot pick + drain test) */
static inline void tw_capture_int(int64_t vx, int64_t vy, TWCaptureInt *out)
{
    int64_t mincross;
    int primary = _tw_find_sector(vx, vy, &mincross);
    out->zone = (uint8_t)primary;
    _tw_pick_slot_on(vx, vy, primary, TW_SLOT_LOCAL_I, &out->slot, &out->resid_x, &out->resid_y);
    int64_t vmag2 = _tw_mag2(vx, vy);
    __int128 lhs = (__int128)mincross * mincross * TW_MARGIN_DEN * TW_MARGIN_DEN;
    __int128 rhs = (__int128)vmag2 * TW_SCALE * TW_SCALE * (int64_t)TW_MARGIN_NUM * TW_MARGIN_NUM;
    if (lhs < rhs) {
        int64_t cp = _tw_cross(TW_BOUNDARY_DIR[primary][0], TW_BOUNDARY_DIR[primary][1], vx, vy);
        int64_t cn = _tw_cross(TW_BOUNDARY_DIR[(primary+1)%TW_N_SECTORS][0], TW_BOUNDARY_DIR[(primary+1)%TW_N_SECTORS][1], vx, vy);
        int secondary = (_tw_abs64(cp) < _tw_abs64(cn)) ? (primary-1+TW_N_SECTORS)%TW_N_SECTORS : (primary+1)%TW_N_SECTORS;
        out->drain = 1; out->drain_zone = (uint8_t)secondary;
        _tw_pick_slot_on(vx, vy, secondary, TW_SLOT_LOCAL_I, &out->drain_slot, &out->drain_resid_x, &out->drain_resid_y);
    } else {
        out->drain = 0; out->drain_zone = 0; out->drain_slot = 0;
        out->drain_resid_x = 0; out->drain_resid_y = 0;
    }
}

/* full combined path (hex + tri, 12 centroids/sector) */
static inline void tw_capture_int_combined(int64_t vx, int64_t vy,
                                            TWCaptureInt *out, uint8_t *is_tri_out)
{
    int64_t mincross;
    int primary = _tw_find_sector(vx, vy, &mincross);
    out->zone = (uint8_t)primary;
    int best = 0, best_is_tri = 0; int64_t bd = -1;
    for (int j = 0; j < (int)TW_COMBINED_SLOTS; j++) {
        int64_t dx = vx - TW_COMBINED_GRID[primary][j][0];
        int64_t dy = vy - TW_COMBINED_GRID[primary][j][1];
        int64_t d = dx*dx + dy*dy;
        if (bd < 0 || d < bd) { bd = d; best = j; best_is_tri = (j >= TW_SLOTS_PER); }
    }
    int local = best % TW_SLOTS_PER;
    out->slot = (uint8_t)(primary * TW_SLOTS_PER + local);
    out->resid_x = vx - TW_COMBINED_GRID[primary][best][0];
    out->resid_y = vy - TW_COMBINED_GRID[primary][best][1];
    *is_tri_out = (uint8_t)best_is_tri;
    int64_t vmag2 = _tw_mag2(vx, vy);
    __int128 lhs = (__int128)mincross * mincross * TW_MARGIN_DEN * TW_MARGIN_DEN;
    __int128 rhs = (__int128)vmag2 * TW_SCALE * TW_SCALE * (int64_t)TW_MARGIN_NUM * TW_MARGIN_NUM;
    if (lhs < rhs) {
        int64_t cp = _tw_cross(TW_BOUNDARY_DIR[primary][0], TW_BOUNDARY_DIR[primary][1], vx, vy);
        int64_t cn = _tw_cross(TW_BOUNDARY_DIR[(primary+1)%TW_N_SECTORS][0], TW_BOUNDARY_DIR[(primary+1)%TW_N_SECTORS][1], vx, vy);
        int secondary = (_tw_abs64(cp) < _tw_abs64(cn)) ? (primary-1+TW_N_SECTORS)%TW_N_SECTORS : (primary+1)%TW_N_SECTORS;
        out->drain = 1; out->drain_zone = (uint8_t)secondary;
        int d_best = 0; int64_t d_bd = -1;
        for (int j = 0; j < (int)TW_COMBINED_SLOTS; j++) {
            int64_t dx = vx - TW_COMBINED_GRID[secondary][j][0];
            int64_t dy = vy - TW_COMBINED_GRID[secondary][j][1];
            int64_t d = dx*dx + dy*dy;
            if (d_bd < 0 || d < d_bd) { d_bd = d; d_best = j; }
        }
        int d_local = d_best % TW_SLOTS_PER;
        out->drain_slot = (uint8_t)(secondary * TW_SLOTS_PER + d_local);
        out->drain_resid_x = vx - TW_COMBINED_GRID[secondary][d_best][0];
        out->drain_resid_y = vy - TW_COMBINED_GRID[secondary][d_best][1];
    } else {
        out->drain = 0; out->drain_zone = 0; out->drain_slot = 0;
        out->drain_resid_x = 0; out->drain_resid_y = 0;
    }
}

/* ══════════════════════════════════════════════════════════════════
   RDH — from core/rdh_addr.h + core/rdh_capture.h
   ══════════════════════════════════════════════════════════════════ */
typedef struct { int64_t n_rings, n_wedges, n_mirror, max_u, n_v; } RDHConfig;

static inline int64_t rdh_key(const RDHConfig *cfg, int64_t ring, int64_t wedge,
                              int64_t mirror, int64_t u, int64_t v)
{
    (void)v;
    return ((ring * cfg->n_wedges + wedge) * cfg->n_mirror + mirror) * cfg->max_u + u;
}

static inline int64_t rdh_capture(const uint8_t *data, size_t len, const RDHConfig *cfg)
{
    int32_t field_w = (int32_t)cfg->n_wedges;
    int32_t field_h = (int32_t)cfg->n_rings;
    int64_t acc_x = 0, acc_y = 0;
    size_t steps = (len < 48) ? 48 : len;
    for (size_t i = 0; i < steps; i++) {
        uint32_t b = data[i % len];
        uint32_t dir = b & 0x0F;
        switch (dir) {
            case 0:  acc_x++;               break;
            case 1:  acc_x++; acc_y++;      break;
            case 2:  acc_y++;               break;
            case 3:  acc_x++; acc_y--;      break;
            case 4:  acc_x--;               break;
            case 5:  acc_x--; acc_y--;      break;
            case 6:  acc_y--;               break;
            case 7:  acc_x--; acc_y++;      break;
            case 8:  acc_x += 2;            break;
            case 9:  acc_x++;   acc_y += 2; break;
            case 10: acc_x--;   acc_y += 2; break;
            case 11: acc_x -= 2;            break;
            default: break;
        }
        if ((i & 0xFFF) == 0xFFF) { acc_x %= field_w; acc_y %= field_h; }
    }
    int64_t wedge = (acc_x % field_w + field_w) % field_w;
    int64_t ring  = (acc_y % field_h + field_h) % field_h;
    return rdh_key(cfg, ring, wedge, 0, 0, 0);
}

static inline uint32_t fnv1a(const uint8_t *d, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
    return h;
}

/* ══════════════════════════════════════════════════════════════════
   BENCH
   ══════════════════════════════════════════════════════════════════ */
static inline uint64_t rdtsc_begin(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__ ("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t rdtsc_end(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtscp" : "=a"(lo), "=d"(hi) :: "ecx", "memory");
    __asm__ __volatile__ ("lfence" ::: "memory");
    return ((uint64_t)hi << 32) | lo;
}

#define ITERS 400000
#define TRIALS 9

/* volatile sinks so the optimizer cannot delete the work */
static volatile int64_t g_sink_i;
static volatile uint32_t g_sink_u;

static double bench_tw_hex(const uint8_t *buf, size_t n)
{
    TWCaptureInt c;
    double best = 1e30;
    for (int t = 0; t < TRIALS; t++) {
        uint64_t t0 = rdtsc_begin();
        int64_t acc = 0;
        for (int it = 0; it < ITERS; it++) {
            int64_t vx = (int64_t)(int8_t)buf[(it*7) % n] * (TW_SCALE/256);
            int64_t vy = (int64_t)(int8_t)buf[(it*7+3) % n] * (TW_SCALE/256);
            tw_capture_int(vx, vy, &c);
            acc += c.zone + c.slot;
        }
        uint64_t t1 = rdtsc_end();
        g_sink_i = acc;
        double cpo = (double)(t1 - t0) / ITERS;
        if (cpo < best) best = cpo;
    }
    return best;
}

static double bench_tw_combined(const uint8_t *buf, size_t n)
{
    TWCaptureInt c; uint8_t is_tri;
    double best = 1e30;
    for (int t = 0; t < TRIALS; t++) {
        uint64_t t0 = rdtsc_begin();
        int64_t acc = 0;
        for (int it = 0; it < ITERS; it++) {
            int64_t vx = (int64_t)(int8_t)buf[(it*7) % n] * (TW_SCALE/256);
            int64_t vy = (int64_t)(int8_t)buf[(it*7+3) % n] * (TW_SCALE/256);
            tw_capture_int_combined(vx, vy, &c, &is_tri);
            acc += c.zone + c.slot + is_tri;
        }
        uint64_t t1 = rdtsc_end();
        g_sink_i = acc;
        double cpo = (double)(t1 - t0) / ITERS;
        if (cpo < best) best = cpo;
    }
    return best;
}

static double bench_rdh(const uint8_t *buf, size_t n)
{
    RDHConfig cfg = { 144, 144, 1, 1, 1 };  /* RDH_CAPTURE_144 */
    double best = 1e30;
    for (int t = 0; t < TRIALS; t++) {
        uint64_t t0 = rdtsc_begin();
        int64_t acc = 0;
        for (int it = 0; it < ITERS; it++) {
            acc += rdh_capture(buf, n, &cfg);
        }
        uint64_t t1 = rdtsc_end();
        g_sink_i = acc;
        double cpo = (double)(t1 - t0) / ITERS;
        if (cpo < best) best = cpo;
    }
    return best;
}

static double bench_fnv(const uint8_t *buf, size_t n)
{
    double best = 1e30;
    for (int t = 0; t < TRIALS; t++) {
        uint64_t t0 = rdtsc_begin();
        uint32_t acc = 0;
        for (int it = 0; it < ITERS; it++) {
            acc += fnv1a(buf, n);
        }
        uint64_t t1 = rdtsc_end();
        g_sink_u = acc;
        double cpo = (double)(t1 - t0) / ITERS;
        if (cpo < best) best = cpo;
    }
    return best;
}

int main(void)
{
    printf("tw_rdh_bench — rdtsc+lfence, min-of-%d trials x %d iters\n", TRIALS, ITERS);
    printf("(cycles/op, LOWER is better; self-contained inline copies of the real fns)\n\n");

    /* name/len buckets: RDH is O(1) in len, FNV is O(len) */
    const size_t lens[] = { 8, 16, 24, 32, 48, 64, 128 };
    int nlens = (int)(sizeof(lens)/sizeof(lens[0]));

    printf("%-8s %12s %14s %13s %12s\n",
           "len", "tw_hex", "tw_combined", "rdh_capture", "fnv1a");
    printf("%-8s %12s %14s %13s %12s\n",
           "---", "------", "-----------", "-----------", "-----");

    double tw_h = 0, tw_c = 0, rdh = 0, fnv = 0;
    for (int i = 0; i < nlens; i++) {
        uint8_t buf[128];
        for (int k = 0; k < (int)lens[i]; k++) buf[k] = (uint8_t)(k*31 + 7);
        double a = bench_tw_hex(buf, lens[i]);
        double b = bench_tw_combined(buf, lens[i]);
        double c = bench_rdh(buf, lens[i]);
        double d = bench_fnv(buf, lens[i]);
        printf("%-8d %12.2f %14.2f %13.2f %12.2f\n", (int)lens[i], a, b, c, d);
        if (lens[i] == 32) { tw_h = a; tw_c = b; rdh = c; fnv = d; }
    }

    printf("\n=== headline (len=32, the name-hash regime) ===\n");
    printf("  tw_capture (hex)      = %6.2f cyc\n", tw_h);
    printf("  tw_capture (hex+tri)  = %6.2f cyc\n", tw_c);
    printf("  rdh_capture           = %6.2f cyc\n", rdh);
    printf("  fnv1a                 = %6.2f cyc\n", fnv);
    printf("\n  tw_hex / rdh          = %.2fx\n", tw_h / rdh);
    printf("  tw_combined / rdh     = %.2fx\n", tw_c / rdh);
    printf("  fnv / rdh             = %.2fx\n", fnv / rdh);
    printf("  fnv / tw_combined     = %.2fx\n", fnv / tw_c);
    return 0;
}
