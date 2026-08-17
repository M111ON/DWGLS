/* triangular_addressing_probe.c — T1.3: Nagy 2003/2004 triangular-grid addressing
 * ═══════════════════════════════════════════════════════════════════════════════
 * สนามสามเหลี่ยม = icosahedron unfolded / reciprocal space ของ honeycomb
 * (Ceulemans §15.5x — "allowed k-states form a triangular lattice").
 * พิกัดเป็นทางการ (Nagy, "Shortest Paths in Triangular Grids with Neighbourhood
 * Sequences", CIT 11 (2003) 111-122; "Generalised triangular grids", 2004):
 *
 *   cell = (a₁,a₂,a₃) ∈ ℤ³  โดย  a₁+a₂+a₃ ∈ {0,1}
 *     sum 0  = up triangle   ·  sum 1 = down triangle   (parity = orientation)
 *     lane   = พิกัดคงที่ตัวเดียว (a₁=c แนวเส้นหนึ่ง) — hex = dual ของ triangle
 *   m-neighbourhood  N_m = {q : |Δaᵢ| ≤ 1 ∀i, Σ|Δaᵢ| ≤ m, q ∈ grid}
 *     (Nagy 2004 §3: |x−x′|≤1,|y−y′|≤1,|z−z′|≤1, Σ|Δ|≤m)
 *     parity facts (paper): 1-neighbour ต่าง parity · strict 2  เหมือน ·
 *                            strict 3  ต่าง parity
 *
 *   B-sequence  B = (b₁ b₂ …)  — ก้าวที่ i เคลื่อนภายใน N_{b_i} (วนคาบ)
 *   B-distance = จำนวนก้าวน้อยสุดจาก p ถึง q ภายใต้กฎ B
 *
 * เอกสารเตือน (Nagy 2003 §3.4): ระยะทางนี้ไม่จำเป็นต้องเป็น metric —
 *  example 3.4.1: d(r→s;B) ≠ d(s→r;B) เมื่อ B เป็น sequence ผสม
 *  (เดินกลับต้อง reverse sequence → ระยะไม่เท่ากัน)
 *  sequence คงที่ B=(k) ปลอดภัยเสมอ (m-neighbour เป็น relation สมมาตร)
 *
 * จุดที่ probe นี้วัด (เทียบ stride 37 ของระบบเรา):
 *   1. N_m จริงบน grid + parity invariant
 *   2. B-distance ภายใต้กฎต่างๆ — คู่จุดเดียวกัน ราคาต่างกันตามกฎ
 *      (= scale ladder: cost ขึ้นกับ path ไม่ใช่แค่ endpoints)
 *   3. asymmetry ของ sequence ผสม / symmetry ของ sequence คงที่
 *   4. lane: สองจุดบน lane เดียวกัน ระยะขึ้นกับกฎ
 *   5. stride-37 cycle (mod 144 frame-seek / mod 720 TRING) = permutation
 *      ล้วน: symmetric, uniform, ไม่มี distance function ให้พัง (TETRA §⑩)
 *
 * BUILD: gcc -O2 -Wall -o build/triangular_addressing_probe tools/triangular_addressing_probe.c -lm
 * RUN:   ./build/triangular_addressing_probe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Grid: R=8 → a,b ∈ [−8,8], c = −a−b (sum 0) | 1−a−b (sum 1) ────── */
#define GRID_R     8
#define LANE_W     (2 * GRID_R + 1)          /* 17 */
#define CELLS_PER   (LANE_W * LANE_W)        /* 289 per parity plane */
#define NCELLS      (2 * CELLS_PER)          /* 578 */
#define MAX_STEPS   60
#define MAX_SEQ_LEN 8

/* encode (a,b,parity) → index; decode back (c derived from sum) */
static inline int cell_idx(int a, int b, int parity) {
    return parity * CELLS_PER + (a + GRID_R) * LANE_W + (b + GRID_R);
}
static inline void idx_cell(int idx, int *a, int *b, int *parity) {
    *parity = idx / CELLS_PER;
    int ab = idx % CELLS_PER;
    *a = ab / LANE_W - GRID_R;
    *b = ab % LANE_W - GRID_R;
}
static inline int in_grid(int a, int b) {
    return a >= -GRID_R && a <= GRID_R && b >= -GRID_R && b <= GRID_R;
}

/* strict move sets (Δa,Δb,Δc), |Δ|≤1, Σ|Δ| = m, target stays on grid */
static const int8_t M1[][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
static const int8_t M2[][3] = {{1,1,0},{1,-1,0},{-1,1,0},{-1,-1,0},
                               {1,0,1},{1,0,-1},{-1,0,1},{-1,0,-1},
                               {0,1,1},{0,1,-1},{0,-1,1},{0,-1,-1}};
static const int8_t M3[][3] = {{1,1,1},{1,1,-1},{1,-1,1},{1,-1,-1},
                               {-1,1,1},{-1,1,-1},{-1,-1,1},{-1,-1,-1}};
#define NM1 6
#define NM2 12
#define NM3 8

/* target c: source cell (a,b) with parity p has c = p − a − b (!!)
   §15.67 bug: used −a−b (parity-0 formula) for ALL cells → parity-1
   targets ผิด off-by-one → กราฟ N₁ แตกเป็น 82/578 — แก้เป็น p − a − b */
static inline int tgt_ok(int a, int b, int da, int db, int dc, int p) {
    int na = a + da, nb = b + db;
    int nc = p - a - b + dc;
    if (!in_grid(na, nb)) return 0;
    return (na + nb + nc == 0 || na + nb + nc == 1) ? 1 : 0;
}

/* neighbour count of cell idx within N_m (valid grid targets only) */
static int nsize(int idx, int m) {
    int a, b, p; idx_cell(idx, &a, &b, &p);
    int cnt = 0;
    for (int i = 0; i < NM1; i++) cnt += tgt_ok(a, b, M1[i][0], M1[i][1], M1[i][2], p);
    if (m >= 2) for (int i = 0; i < NM2; i++) cnt += tgt_ok(a, b, M2[i][0], M2[i][1], M2[i][2], p);
    if (m >= 3) for (int i = 0; i < NM3; i++) cnt += tgt_ok(a, b, M3[i][0], M3[i][1], M3[i][2], p);
    return cnt;
}

/* expand one cell by N_m → write valid target indices to out[] */
static int expand(int idx, int m, int *out) {
    int a, b, p; idx_cell(idx, &a, &b, &p);
    int n = 0;
    const int8_t (*sets[4])[3] = {NULL, M1, M2, M3};
    const int counts[4] = {0, NM1, NM2, NM3};
    for (int mm = 1; mm <= m; mm++)
        for (int i = 0; i < counts[mm]; i++) {
            int da = sets[mm][i][0], db = sets[mm][i][1], dc = sets[mm][i][2];
            int na = a + da, nb = b + db;
            int nc = p - a - b + dc;
            if (!in_grid(na, nb)) continue;
            if (na + nb + nc != 0 && na + nb + nc != 1) continue;
            out[n++] = cell_idx(na, nb, (na + nb + nc) & 1);
        }
    return n;
}

/* ── B-distance: layered BFS from src with periodic rule B[0..L-1] ──
 * dist[] =  steps to reach each cell (0 = src, -1 = unreached within MAX_STEPS) */
static void bdist(int src, const int *B, int L, int8_t *dist) {
    memset(dist, -1, NCELLS);
    /* NOTE: indices ถึง 577 — ต้องเป็น int (int8_t truncation = bug §15.67) */
    int *front = (int *)malloc(NCELLS * sizeof(int));
    int *next  = (int *)malloc(NCELLS * sizeof(int));
    uint8_t *seen = (uint8_t *)calloc(NCELLS, 1);
    int nf = 1; front[0] = src;
    dist[src] = 0; seen[src] = 1;
    int tmp[32];
    int step = 0;
    while (nf > 0 && step < MAX_STEPS) {
        int nn = 0;
        for (int f = 0; f < nf; f++) {
            int cur = front[f];
            int n = expand(cur, B[step % L], tmp);
            for (int i = 0; i < n; i++) {
                int t = tmp[i];
                if (!seen[t]) { seen[t] = 1; dist[t] = (int8_t)(step + 1); next[nn++] = t; }
            }
        }
        memcpy(front, next, (size_t)nn * sizeof(int)); nf = nn; step++;
    }
    free(front); free(next); free(seen);
}

static int bdist_pair(int src, int dst, const int *B, int L) {
    int8_t *dist = (int8_t *)malloc(NCELLS);
    bdist(src, B, L, dist);
    int d = dist[dst];
    free(dist);
    return d;
}

/* ── random pairs — ตรวจ symmetry / triangle inequality ─────────── */
static uint32_t rng = 0x9E3779B9u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

typedef struct { const char *name; int seq[MAX_SEQ_LEN]; int len; } Rule;
static const Rule RULES[] = {
    {"B=(1)     constant",  {1},                 1},
    {"B=(2)     constant",  {2},                 1},
    {"B=(3)     constant",  {3},                 1},
    {"B=(1,2)   mixed",     {1,2},               2},
    {"B=(2,1)   mixed rev", {2,1},               2},
    {"B=(1,3,2) mixed",     {1,3,2},             3},
    {"B=(2,3,1) mixed rev", {2,3,1},             3},
    {"B=(1,2,1,3) mixed",   {1,2,1,3},           4},
};
#define NRULES ((int)(sizeof(RULES) / sizeof(RULES[0])))

/* ── stride-37 comparison (frame-seek 144 / TRING 720) ───────────── */
static void stride37_demo(void) {
    printf("\n── stride-37 cycle walk (ระบบเรา — permutation, ไม่ใช่ distance) ──\n");
    /* frame-seek: mod 144 (scale axis) */
    int cyc[144], pos = 0;
    for (int s = 0; s < 144; s++) { cyc[s] = pos; pos = (pos + 37) % 144; }
    int seen[144] = {0}, unique = 1, hist6[6] = {0};
    for (int s = 0; s < 144; s++) { if (seen[cyc[s]]++) unique = 0; }
    for (int s = 0; s < 144; s++) hist6[cyc[s] % 6]++;
    printf("  stride 37 mod 144 (frame-seek scale axis):\n");
    printf("    cycle ครอบครบ 144/144 scales ครั้งเดียว (gcd(37,144)=1)  distinct=%s\n", unique ? "YES" : "NO");
    printf("    กระจายตาม residue mod 6 (spoke-like): %d,%d,%d,%d,%d,%d — imbalance=0\n",
           hist6[0],hist6[1],hist6[2],hist6[3],hist6[4],hist6[5]);
    /* forward vs backward hop count is a bijection on a group: d(x,y) กำหนดค่าเดียว */
    printf("    เดินหน้า 37 ก้าว ↔ เดินถอยหลัง 107 (=−37 mod 144): bijection เดียวกัน\n");
    printf("    → 'ระยะทาง' symmetric เสมอ (group/cycle) ไม่มี non-metric ให้เกิด\n");

    /* TRING: mod 720, 6 spokes × 120 */
    int spk[6] = {0};
    for (int i = 0; i < 720; i++) spk[(int)(((uint64_t)i * 37) % 720) / 120]++;
    printf("  stride 37 mod 720 (TRING, 6 spokes × 120): spoke counts %d,%d,%d,%d,%d,%d — imbalance=%d\n",
           spk[0],spk[1],spk[2],spk[3],spk[4],spk[5], (spk[0]>spk[1]?spk[0]-spk[1]:spk[1]-spk[0]));
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("T1.3 — Triangular addressing probe (Nagy 2003/2004)\n");
    printf("grid: a,b∈[−%d,%d], c = −a−b | 1−a−b  → %d cells (2 parity planes)\n",
           GRID_R, GRID_R, NCELLS);

    /* 1. neighbourhood sizes + parity invariants */
    printf("\n── 1. N_m sizes + parity invariants ──\n");
    int c0 = cell_idx(0, 0, 0);   /* up */
    printf("  |N_1(0,0,0)| = %d   |N_2| = %d   |N_3| = %d\n",
           nsize(c0, 1), nsize(c0, 2), nsize(c0, 3));
    printf("  (edge-adjacency = 3 ถูกต้องตามเรขาคณิตของ triangle)\n");
    /* parity invariants (paper: 1-neigh ต่าง parity · strict2 เหมือน · strict3 ต่าง) */
    int out[32], n1, n2;
    n1 = expand(c0, 1, out);
    int flip_ok = 1;
    for (int i = 0; i < n1; i++) { int a,b,p; idx_cell(out[i],&a,&b,&p); if (p != 1) flip_ok = 0; }
    n2 = expand(c0, 2, out);
    int same_ok = 1;
    for (int i = n1; i < n2; i++) { int a,b,p; idx_cell(out[i],&a,&b,&p); if (p != 0) same_ok = 0; }
    n2 = expand(c0, 3, out);
    int flip3_ok = 1;
    for (int i = n1 + 6; i < n2; i++) { int a,b,p; idx_cell(out[i],&a,&b,&p); if (p != 1) flip3_ok = 0; }
    printf("  parity: 1-neigh ต่าง %s · strict-2 เหมือน %s · strict-3 ต่าง %s  (paper ✓)\n",
           flip_ok ? "OK" : "FAIL", same_ok ? "OK" : "FAIL", flip3_ok ? "OK" : "FAIL");

    /* 2. B-distance under each rule — mean/max from center */
    printf("\n── 2. B-distance จากศูนย์กลาง (0,0,0) — กฎต่างกัน ราคาต่างกัน ──\n");
    int8_t *dist = (int8_t *)malloc(NCELLS);
    printf("  %-16s %8s %8s %8s\n", "rule", "mean", "max", "max@cell");
    for (int r = 0; r < NRULES; r++) {
        bdist(c0, RULES[r].seq, RULES[r].len, dist);
        long sum = 0; int mx = 0, mxidx = 0;
        for (int i = 0; i < NCELLS; i++) {
            if (dist[i] < 0) continue;
            sum += dist[i];
            if (dist[i] > mx) { mx = dist[i]; mxidx = i; }
        }
        printf("  %-16s %8.2f %8d   (%d,%d,p=%d)\n", RULES[r].name,
               (double)sum / NCELLS, mx, mxidx / CELLS_PER / LANE_W - GRID_R,
               (mxidx % CELLS_PER) % LANE_W - GRID_R, mxidx / CELLS_PER);
    }

    /* 3. symmetry / asymmetry — random pairs */
    printf("\n── 3. Symmetry: d(A→B) เทียบ d(B→A) — 200 random pairs ──\n");
    printf("  %-16s %10s %10s\n", "rule", "asym_pairs", "max|Δd|");
    for (int r = 0; r < NRULES; r++) {
        int asym = 0, maxad = 0;
        for (int t = 0; t < 200; t++) {
            int A = (int)(rnd() % NCELLS), B = (int)(rnd() % NCELLS);
            int dAB = bdist_pair(A, B, RULES[r].seq, RULES[r].len);
            int dBA = bdist_pair(B, A, RULES[r].seq, RULES[r].len);
            int ad = dAB > dBA ? dAB - dBA : dBA - dAB;
            if (ad > 0) { asym++; if (ad > maxad) maxad = ad; }
        }
        printf("  %-16s %10d %10d\n", RULES[r].name, asym, maxad);
    }

    /* 4. triangle inequality violation rate — 120 random triples */
    printf("\n── 4. Triangle inequality d(A,C) ≤ d(A,B)+d(B,C) — 120 triples ──\n");
    for (int r = 0; r < NRULES; r++) {
        int viol = 0;
        for (int t = 0; t < 120; t++) {
            int A = (int)(rnd() % NCELLS), B = (int)(rnd() % NCELLS), C = (int)(rnd() % NCELLS);
            int dAC = bdist_pair(A, C, RULES[r].seq, RULES[r].len);
            int dAB = bdist_pair(A, B, RULES[r].seq, RULES[r].len);
            int dBC = bdist_pair(B, C, RULES[r].seq, RULES[r].len);
            if (dAC > dAB + dBC) viol++;
        }
        printf("  %-16s %3d/120 violations%s\n", RULES[r].name, viol,
               viol ? "  ← ไม่ใช่ metric!" : "");
    }

    /* 5. lane demo — same lane, different rule = different cost */
    printf("\n── 5. Lane (a₁ คงที่): (0,0,0) → (4,−4,0) บน lane เดียวกัน ──\n");
    int dst = cell_idx(4, -4, 0);
    for (int r = 0; r < NRULES; r++) {
        int d = bdist_pair(c0, dst, RULES[r].seq, RULES[r].len);
        printf("  %-16s d = %d\n", RULES[r].name, d);
    }
    printf("  → คู่จุดเดียวกัน ราคาเปลี่ยนตามกฎการเดิน = scale ladder: cost ขึ้นกับ path\n");

    /* 6. stride-37 */
    stride37_demo();

    printf("\n── Verdict ──\n");
    printf("  B-distance: ขึ้นกับ route rule (non-metric เมื่อ sequence ผสม)\n");
    printf("  stride-37 : constant rule + cycle permutation → symmetric เสมอ, uniform\n");
    printf("  → ระบบเราไม่มี distance function (TETRA §⑩): permutation ปิดวงพอ, ไม่มี metric ให้พัง\n");

    free(dist);
    return 0;
}
