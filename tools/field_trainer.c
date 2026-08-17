/* tools/field_trainer.c — T1.2: evolutionary search over integer knobs
 * ═══════════════════════════════════════════════════════════════════════
 * user: "field trainer — evolutionary search เหนือ integer knobs (stride,
 *        offset, gate, orbit) วัดบน GGUF จริง วนหา champion rule set
 *        โดยไม่เขียนสูตรเอง"
 *
 * Genes (5 integer knobs — ค้นหาเอาเอง ไม่จูนมือ):
 *   stride : step ของ rank→scale walk, coprime กับ 144  (default 37)
 *   offset : shift ของ walk                           (default 0)
 *   gate   : ROI gate → k_max = envelope_depth(gate)  (default 1.0 → 5)
 *   orbit  : symmetry partition ของ capacity 20736/O per orbit (default 1)
 *   chunk  : chunk size (4K..64K)                     (default 16K)
 *
 * Fitness วัดจาก REAL workload (GGUF tensor set — 1 tensor = 1 file):
 *   per chunk: w = (stride·rank + offset) % 144
 *     w > k_max            → LIFT (ghost — 0 field cost)
 *     used[orbit] + fp(w) > 20736/O → REJECT (deterministic — นับ, ไม่ silent)
 *     else                 → ADMIT (used[orbit] += fp(w))
 *   fitness = Σ used + 1e9·rejects        (reject = ข้อมูลไม่เข้า — ภัยพิบัติ)
 *   รายงาน: field slots, lifts, rejects, windows
 *
 * Search: population 32, tournament-3 selection, elite 2, uniform crossover,
 *   mutate 1 gene (stride → random coprime · offset ± rand · gate ± step ·
 *   orbit → ถัดไปใน {1,4,12,24} · chunk → ×2/÷2) — splitmix64 RNG (reproducible)
 *
 * BUILD: gcc -O2 -I. -Icore -Icore/infra -o build/field_trainer tools/field_trainer.c -lm
 * RUN:
 *   build/field_trainer --gguf /i/model/LFM2.5-2.6B-Q8_0.gguf
 *   build/field_trainer --gguf <model> --gens 80 --pop 40 --seed 7
 *   build/field_trainer --gguf <model> --eval 37,0,1.0,1,16384   (วัดค่าเดียว)
 *   build/field_trainer --folder F:/notebookLM
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../core/geo_ghost_envelope.h"
#include "../core/geo_cap_account.h"   /* CAP_RULE_* — trained default placement */
#include "gguf_reader.h"

#define CHUNKS_MAX 60000000ull   /* 60M chunks total cap */

/* ── genes ── */
typedef struct {
    uint16_t stride;
    uint8_t  offset;
    double   gate;
    uint8_t  orbit;      /* 1, 4, 12, 24 */
    uint32_t chunk;      /* 4096 .. 65536, power of 2 */
} Genes;

typedef struct {
    uint64_t field_slots;   /* Σ used */
    uint64_t lifts;
    uint64_t rejects;
    uint64_t chunks;
} Eval;

/* ── coprimes of 144 (= 2⁴·3²): odd & not divisible by 3 ── */
static const uint16_t COP[] = {
     5,  7, 11, 13, 17, 19, 23, 25, 29, 31, 35, 37, 41, 43, 47, 49,
    53, 55, 59, 61, 65, 67, 71, 73, 77, 79, 83, 85, 89, 91, 95, 97,
   101, 103, 107, 109, 113, 115, 119, 121, 125, 127, 131, 133, 137, 139, 143
};
#define N_COP ((int)(sizeof(COP) / sizeof(COP[0])))

/* ════════════════════════════════════════════════════════════════════
   ROTATION THEOREM (T1.3b §15.68) — wired as search-space invariant
   ──
   gcd(s,144)=1 ⇒ w_r = (s·r + o) mod 144 เป็น permutation 1 cycle ครบ 144
   และ ∀ L|144 (lane = residue mod L):
     · ทุก L ก้าวติดกันครอบทุก lane พอดี 1 ครั้ง (rotation)
     · แต่ละ lane ปรากฏ 144/L ครั้ง/144 ก้าว (uniform)
   — constant rule ⇒ symmetric + uniform (T1.3: mixed rule = non-metric)
   — บังคับ: stride ทุกตัวใน COP ต้องผ่าน rotation_verify (อัตโนมัติตอน boot)
   ════════════════════════════════════════════════════════════════════ */
static int stride_coprime(uint16_t s) {
    if (s == 0 || s >= 144) return 0;
    for (uint32_t d = 2; d <= 144; d++)
        if (144 % d == 0 && s % d == 0) return 0;
    return 1;
}

/* full theorem check for a (stride, offset): 1 = rotates+uniform every lane */
static int rotation_verify(uint16_t s, uint8_t o) {
    if (!stride_coprime(s)) return 0;
    int seen[144] = {0};
    int w = o;
    for (int r = 0; r < 144; r++) { seen[w] = 1; w = (w + s) % 144; }
    for (int i = 0; i < 144; i++) if (!seen[i]) return 0;      /* 1 cycle ครบ */
    for (int L = 2; L <= 144; L++) {
        if (144 % L) continue;
        int hist[144] = {0};
        w = o;
        for (int r = 0; r < 144; r++) { hist[w % L]++; w = (w + s) % 144; }
        for (int l = 0; l < L; l++) if (hist[l] != 144 / L) return 0;  /* uniform */
        for (int r0 = 0; r0 < 144; r0 += L) {                          /* rotation */
            int wl[144] = {0};
            w = (o + s * r0) % 144;
            for (int k = 0; k < L; k++) { wl[w % L]++; w = (w + s) % 144; }
            for (int l = 0; l < L; l++) if (wl[l] != 1) return 0;
        }
    }
    return 1;
}

static const double GATES[12] = {
    0.25, 0.50, 0.75, 1.00, 1.25, 1.50, 1.75, 2.00, 2.25, 2.50, 2.75, 3.00
};
#define N_GATES ((int)(sizeof(GATES) / sizeof(GATES[0])))

static const uint8_t ORBITS[4] = { 1, 4, 12, 24 };
#define N_ORBITS ((int)(sizeof(ORBITS) / sizeof(ORBITS[0])))

static const uint32_t CHUNKS[] = { 4096u, 8192u, 16384u, 32768u, 65536u, 131072u, 262144u };
#define N_CHUNKS ((int)(sizeof(CHUNKS) / sizeof(CHUNKS[0])))

/* ── splitmix64 RNG (reproducible) ── */
static uint64_t rng_state;
static uint64_t rng_next(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint64_t rng_range(uint64_t n) { return n ? rng_next() % n : 0; }

/* ── workloads: list of size arrays (bytes) — multi-model joint training ── */
typedef struct { uint64_t *sizes; uint64_t n; char name[512]; } Workload;
static Workload wls[8];
static int n_wls;

/* admission chain over ONE workload (per-model breakdown) */
static void eval_one(const Genes *g, int wi, Eval *ev) {
    memset(ev, 0, sizeof(*ev));
    uint64_t used[24]; memset(used, 0, sizeof(used));
    uint32_t k_max = ght_envelope_depth(g->gate);
    uint64_t cap_per = GHT_WIN / g->orbit;
    const Workload *wl = &wls[wi];

    for (uint64_t f = 0; f < wl->n; f++) {
        uint64_t nchunks = (wl->sizes[f] + g->chunk - 1) / g->chunk;
        if (nchunks == 0) continue;
        for (uint64_t r = 0; r < nchunks; r++) {
            ev->chunks++;
            uint8_t w = (uint8_t)(((uint64_t)g->stride * r + g->offset) % 144u);
            if (w > k_max) { ev->lifts++; continue; }
            uint64_t env = ght_fp(w);
            uint8_t b = (uint8_t)(r % g->orbit);
            if (used[b] + env > cap_per) { ev->rejects++; continue; }
            used[b] += env;
            ev->field_slots += env;
        }
    }
}

/* admission chain over ALL workloads — fitness = Σ per-model cost */
static void evaluate(const Genes *g, Eval *ev) {
    memset(ev, 0, sizeof(*ev));
    for (int wi = 0; wi < n_wls; wi++) {
        Eval e;
        eval_one(g, wi, &e);
        ev->field_slots += e.field_slots;
        ev->lifts += e.lifts;
        ev->rejects += e.rejects;
        ev->chunks += e.chunks;
    }
}

/* fitness — rejects ครอง (ข้อมูลไม่เข้า = ภัยพิบัติ) แล้ว field footprint
   + ค่า ghost ตาม cost model ของระบบ: 1 lift = 1 replay event = 8 slots
   (GHT_REPLAY_EVENT) — field ว่างทั้งสนาม = แพง (ทุก chunk เข้า ghost)  */
#define LIFT_COST ((double)GHT_REPLAY_EVENT)

static double fitness_of(const Genes *g, const Eval *ev) {
    (void)g;
    return (double)ev->field_slots + LIFT_COST * (double)ev->lifts
         + 1e9 * (double)ev->rejects;
}

/* ── workload builders (append to list — repeatable --gguf/--folder) ── */
static int load_gguf(const char *path) {
    GgufReader r;
    if (gguf_open(path, &r) != 0) { printf("cannot open %s\n", path); return -1; }
    if (n_wls >= 8) { printf("too many workloads\n"); return -1; }
    Workload *wl = &wls[n_wls++];
    wl->sizes = (uint64_t *)malloc((size_t)r.n_tensors * sizeof(uint64_t));
    if (!wl->sizes) return -1;
    wl->n = 0;
    for (uint32_t i = 0; i < r.n_tensors; i++) wl->sizes[wl->n++] = r.sizes[i];
    snprintf(wl->name, sizeof(wl->name), "%s", path);
    printf("workload %d: %s — %lu tensors\n", n_wls, path, (unsigned long)wl->n);
    return 0;
}

typedef struct { uint64_t size; } FEntry;

static void walk_dir(const char *dir, FEntry *out, uint32_t cap, uint32_t *n) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && *n < cap) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[1120];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { walk_dir(full, out, cap, n); continue; }
        out[*n].size = (uint64_t)st.st_size;
        (*n)++;
    }
    closedir(d);
}

static int load_folder(const char *root) {
    FEntry *files = (FEntry *)malloc(50000u * sizeof(FEntry));
    uint32_t nf = 0;
    walk_dir(root, files, 50000u, &nf);
    if (n_wls >= 8) { free(files); printf("too many workloads\n"); return -1; }
    Workload *wl = &wls[n_wls++];
    wl->sizes = (uint64_t *)malloc((size_t)nf * sizeof(uint64_t));
    if (!wl->sizes) { free(files); return -1; }
    for (uint32_t i = 0; i < nf; i++) wl->sizes[i] = files[i].size;
    wl->n = nf;
    snprintf(wl->name, sizeof(wl->name), "%s", root);
    printf("workload %d: %s — %u files\n", n_wls, root, nf);
    free(files);
    return 0;
}

/* ── print one gene row ── */
static void print_genes(const Genes *g, const Eval *ev, const char *tag) {
    uint64_t wins = (ev->field_slots + GHT_WIN - 1) / GHT_WIN;
    printf("  %-14s stride=%3u offset=%3u gate=%4.2f(kmax=%u) orbit=%2u chunk=%5u | "
           "field=%8llu slots (~%llu win) lift=%8llu rej=%llu  %s\n",
           tag, g->stride, g->offset, g->gate,
           (unsigned)ght_envelope_depth(g->gate), g->orbit, g->chunk,
           (unsigned long long)ev->field_slots, (unsigned long long)wins,
           (unsigned long long)ev->lifts, (unsigned long long)ev->rejects,
           ev->rejects ? "✗REJECT" : "");
}

/* ── parse "s,o,g,O,chunk" ── */
static int parse_genes(const char *s, Genes *g) {
    int stride, offset, orbit, chunk; double gate;
    if (sscanf(s, "%d,%d,%lf,%d,%d", &stride, &offset, &gate, &orbit, &chunk) != 5)
        return -1;
    g->stride = (uint16_t)stride; g->offset = (uint8_t)offset;
    g->gate = gate; g->orbit = (uint8_t)orbit; g->chunk = (uint32_t)chunk;
    return 0;
}

static void genes_default(Genes *g) {
    /* trained default (§15.71 unified champion) — single source: core/geo_cap_account.h */
    g->stride = CAP_RULE_STRIDE;
    g->offset = (uint8_t)CAP_RULE_OFFSET;
    g->gate   = CAP_RULE_GATE;
    g->orbit  = CAP_RULE_ORBIT;
    g->chunk  = CAP_RULE_CHUNK;
}
static void genes_random(Genes *g) {
    g->stride = COP[rng_range(N_COP)];
    g->offset = (uint8_t)rng_range(144);
    g->gate   = GATES[rng_range(N_GATES)];
    g->orbit  = ORBITS[rng_range(N_ORBITS)];
    g->chunk  = CHUNKS[rng_range(N_CHUNKS)];
}

#define POP_MAX 64

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *eval_genes = NULL;
    int gens = 120, pop = 32;
    uint64_t seed = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--gguf") == 0 && i + 1 < argc) { if (load_gguf(argv[++i]) != 0) return 1; }
        else if (strcmp(argv[i], "--folder") == 0 && i + 1 < argc) { if (load_folder(argv[++i]) != 0) return 1; }
        else if (strcmp(argv[i], "--gens") == 0 && i + 1 < argc) gens = atoi(argv[++i]);
        else if (strcmp(argv[i], "--pop") == 0 && i + 1 < argc) pop = atoi(argv[++i]);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--eval") == 0 && i + 1 < argc) eval_genes = argv[++i];
    }
    if (pop > POP_MAX) pop = POP_MAX;
    if (n_wls == 0) {
        printf("usage:\n  %s --gguf <model.gguf> [--gguf <model2> ...] [--gens N] [--pop N] [--seed S] [--eval s,o,g,O,chunk]\n"
               "  %s --folder <root> ...\n", argv[0], argv[0]);
        return 1;
    }
    printf("joint training workloads: %d\n", n_wls);

    /* ── rotation theorem: verify the COP search table (invariant) ── */
    int cop_bad = 0;
    for (int i = 0; i < N_COP; i++) {
        if (!rotation_verify(COP[i], 0)) { cop_bad++; printf("  ✗ COP[%d]=%u fails rotation theorem\n", i, COP[i]); }
    }
    printf("rotation theorem: COP table %d strides — %s (ทุก stride coprime 144 + rotates + uniform)\n",
           N_COP, cop_bad ? "✗ FAIL" : "✓ verified");

    /* ── --eval mode: evaluate one rule set (warn ถ้า stride ไม่ผ่านทฤษฎี) ── */
    if (eval_genes) {
        Genes g; Eval ev;
        if (parse_genes(eval_genes, &g) != 0) { printf("bad genes\n"); return 1; }
        if (!rotation_verify(g.stride, g.offset))
            printf("  ⚠ stride %u ไม่ใช่ coprime ของ 144 — นอก search space (rotation ไม่รับประกัน)\n", g.stride);
        evaluate(&g, &ev);
        printf("eval %s\n", eval_genes);
        print_genes(&g, &ev, "evaluated");
        return 0;
    }

    rng_state = seed;

    /* baseline: default rule set */
    Genes dft; genes_default(&dft);
    Eval ev_dft; evaluate(&dft, &ev_dft);
    printf("\nBASELINE (current system defaults)\n");
    print_genes(&dft, &ev_dft, "default");

    /* ── population init: default + mutants + random ── */
    static Genes popg[POP_MAX];
    static Eval  pope[POP_MAX];
    static double popf[POP_MAX];
    int n = 0;
    popg[n] = dft; n++;
    for (int i = 1; i < pop; i++) { genes_random(&popg[n]); n++; }

    Genes champ = dft; Eval champ_ev = ev_dft; double champ_f = fitness_of(&dft, &ev_dft);
    int champ_gen = -1;

    printf("\nEVOLVING  pop=%d gens=%d seed=%llu\n", pop, gens, (unsigned long long)seed);
    printf("  fitness = field_slots + 8·lifts + 1e9·rejects  (ต่ำ = ดี)\n");

    for (int gen = 0; gen < gens; gen++) {
        static Genes next[POP_MAX];
        /* evaluate */
        for (int i = 0; i < n; i++) {
            evaluate(&popg[i], &pope[i]);
            popf[i] = fitness_of(&popg[i], &pope[i]);
        }
        /* best this gen */
        int bi = 0;
        for (int i = 1; i < n; i++) if (popf[i] < popf[bi]) bi = i;
        if (popf[bi] < champ_f) {
            champ_f = popf[bi]; champ = popg[bi]; champ_ev = pope[bi]; champ_gen = gen;
        }
        if (gen % 10 == 0 || gen == gens - 1) {
            printf("  gen %3d/%d  best fitness %12.0f  %s (champ gen %d)\n",
                   gen, gens, popf[bi],
                   popf[bi] >= 1e9 ? "✗reject" : "ok", champ_gen);
        }
        if (gen == gens - 1) break;

        /* restart-on-stagnation: 15 gens ไม่ดีขึ้น → รีเซ็ตประชากร (เก็บ champion) */
        if (champ_gen >= 0 && gen - champ_gen >= 15) {
            next[0] = champ;
            next[1] = popg[bi];
            for (int i = 2; i < n; i++) genes_random(&next[i]);
            memcpy(popg, next, sizeof(Genes) * (size_t)n);
            printf("  ── restart @gen %d (stagnant 15, champ kept) ──\n", gen);
            continue;
        }

        /* next generation: elite 2 + tournament-3 + mutate/crossover */
        /* elite: ALL-TIME champion (ไม่หลุด) + best of current pop */
        next[0] = champ;
        next[1] = popg[bi];
        for (int i = 2; i < n; i++) {
            /* tournament of 3 */
            int a = (int)rng_range(n), b = (int)rng_range(n), c = (int)rng_range(n);
            int w = popf[a] < popf[b] ? a : b;
            if (popf[c] < popf[w]) w = c;
            Genes child = popg[w];
            /* uniform crossover with a second parent 50% */
            if (rng_range(2)) {
                int p2 = (int)rng_range(n);
                if (rng_range(2)) child.stride = popg[p2].stride;
                if (rng_range(2)) child.offset = popg[p2].offset;
                if (rng_range(2)) child.gate   = popg[p2].gate;
                if (rng_range(2)) child.orbit  = popg[p2].orbit;
                if (rng_range(2)) child.chunk  = popg[p2].chunk;
            }
            /* mutate exactly one gene */
            int m = (int)rng_range(5);
            if (m == 0) child.stride = COP[rng_range(N_COP)];
            else if (m == 1) {
                int d = (int)rng_range(17) - 8; if (d == 0) d = 1;
                child.offset = (uint8_t)((child.offset + 144 + d) % 144);
            } else if (m == 2) {
                int d = rng_range(2) ? 1 : -1;
                for (int gi = 0; gi < N_GATES; gi++)
                    if (GATES[gi] == child.gate) {
                        int ni = gi + d;
                        if (ni >= 0 && ni < N_GATES) child.gate = GATES[ni];
                        break;
                    }
            } else if (m == 3) {
                for (int oi = 0; oi < N_ORBITS; oi++)
                    if (ORBITS[oi] == child.orbit) {
                        child.orbit = ORBITS[(oi + 1) % N_ORBITS];
                        break;
                    }
            } else {
                for (int ci = 0; ci < N_CHUNKS; ci++)
                    if (CHUNKS[ci] == child.chunk) {
                        int ni = ci + (rng_range(2) ? 1 : -1);
                        if (ni >= 0 && ni < N_CHUNKS) child.chunk = CHUNKS[ni];
                        break;
                    }
            }
            next[i] = child;
        }
        memcpy(popg, next, sizeof(Genes) * (size_t)n);
    }

    /* ── local polish: hill-climb champion — mutate 1 gene, keep ถ้าดีขึ้น ── */
    Eval cand_ev; Genes cand;
    for (int it = 0; it < 400; it++) {
        cand = champ;
        int m = (int)rng_range(5);
        if (m == 0) cand.stride = COP[rng_range(N_COP)];
        else if (m == 1) cand.offset = (uint8_t)rng_range(144);
        else if (m == 2) cand.gate = GATES[rng_range(N_GATES)];
        else if (m == 3) cand.orbit = ORBITS[rng_range(N_ORBITS)];
        else cand.chunk = CHUNKS[rng_range(N_CHUNKS)];
        evaluate(&cand, &cand_ev);
        if (fitness_of(&cand, &cand_ev) < champ_f) {
            champ = cand; champ_ev = cand_ev; champ_f = fitness_of(&cand, &cand_ev);
        }
    }

    /* ── champion must still satisfy the rotation theorem ── */
    int c_rot = rotation_verify(champ.stride, champ.offset);
    int d_rot = rotation_verify(dft.stride, dft.offset);
    printf("rotation theorem: default (37,0) %s · champion (%u,%u) %s\n",
           d_rot ? "✓ rotates+uniform" : "✗", champ.stride, champ.offset,
           c_rot ? "✓ rotates+uniform" : "✗");

    printf("\nCHAMPION (found by search + local polish)\n");
    print_genes(&champ, &champ_ev, "champion");
    printf("\nCOMPARISON\n");
    print_genes(&dft, &ev_dft, "default");

    uint64_t saved = ev_dft.field_slots > champ_ev.field_slots
                   ? ev_dft.field_slots - champ_ev.field_slots : 0;
    if (champ_ev.field_slots > 0 && ev_dft.field_slots > 0)
        printf("  field footprint: default %.1f → champion %.1f  (↓ %.1f%%)\n",
               (double)ev_dft.field_slots, (double)champ_ev.field_slots,
               100.0 * (1.0 - (double)champ_ev.field_slots / (double)ev_dft.field_slots));
    printf("  saved %llu slots (~%llu windows) | rejects default=%llu champion=%llu\n",
           (unsigned long long)saved, (unsigned long long)((saved + GHT_WIN - 1) / GHT_WIN),
           (unsigned long long)ev_dft.rejects, (unsigned long long)champ_ev.rejects);

    /* per-model breakdown: default vs champion on EACH workload (joint-fit) */
    printf("\nPER-MODEL BREAKDOWN (fitness = Σ per-model cost)\n");
    printf("  %-12s %8s %8s %6s | %8s %8s %6s\n", "model", "d_field", "d_lift", "d_rej",
           "c_field", "c_lift", "c_rej");
    int all_zero = 1;
    for (int wi = 0; wi < n_wls; wi++) {
        Eval de, ce;
        eval_one(&dft, wi, &de);
        eval_one(&champ, wi, &ce);
        if (ce.rejects != 0) all_zero = 0;
        printf("  %-12s %8llu %8llu %6llu | %8llu %8llu %6llu%s\n",
               wls[wi].name + (strlen(wls[wi].name) > 12 ? strlen(wls[wi].name) - 12 : 0),
               (unsigned long long)de.field_slots, (unsigned long long)de.lifts,
               (unsigned long long)de.rejects,
               (unsigned long long)ce.field_slots, (unsigned long long)ce.lifts,
               (unsigned long long)ce.rejects, ce.rejects ? "  ✗" : "");
    }
    printf("  → champion 0 rejects ทุกโมเดล: %s\n", all_zero ? "YES ✓" : "NO");

    for (int wi = 0; wi < n_wls; wi++) free(wls[wi].sizes);
    return 0;
}
