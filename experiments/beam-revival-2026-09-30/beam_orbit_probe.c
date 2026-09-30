/* tools/beam_orbit_probe.c — Phase 2: D4/orbita canonicalization
 * ═══════════════════════════════════════════════════════════════════════════
 * คำกล่าว: 36 orientations → ~5 unique (36/8). ทดสอบด้วย octahedral group
 * (24 rotations + inversion = 48; ครอบ 36 ของแผน เพราะ 36 ไม่ใช่กรุ๊ปมาตรฐาน)
 * (a) sanity: 36 orientations ของ cube เดียว → ต้องได้ 1 orbit
 * (b) cubes จาก stream จริง 500 cubes → นับ orbits ( kill ถ้า ≈500 )
 *
 * BUILD: gcc -O2 -w -I. -Icore -o build/beam_orbit_probe.exe tools/beam_orbit_probe.c -lm
 * RUN:   ./build/beam_orbit_probe.exe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_box.h"

#define CUBE 1000u
#define NORB_TEST 500

/* 24 rotations ของ cube: (perm p[3], signs s[3], det=+1) — generate แบบ fix */
static int perms[24][3];
static int signs[24][3];
static int nrot = 0;

static void build_rots(void) {
    int P[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    int ps[6] = {1,-1,-1,1,1,-1}; /* parity ของ perm */
    for (int p = 0; p < 6; p++)
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2)
                for (int sz = -1; sz <= 1; sz += 2) {
                    int det = ps[p] * sx * sy * sz;
                    if (det != 1) continue;
                    perms[nrot][0] = P[p][0]; perms[nrot][1] = P[p][1]; perms[nrot][2] = P[p][2];
                    signs[nrot][0] = sx; signs[nrot][1] = sy; signs[nrot][2] = sz;
                    nrot++;
                }
}

/* coord (x,y,z) 0..9 ภายใต้ rotation r (จุดกลาง 4.5) → index ใหม่ */
static unsigned rot_idx(unsigned x, unsigned y, unsigned z, int r, int invert) {
    int c[3] = {(int)x, (int)y, (int)z};
    int nc[3];
    for (int i = 0; i < 3; i++) {
        int v = c[perms[r][i]];
        int s = signs[r][i];
        /* map 0..9 รอบจุดกลาง: v' = s>=0 ? v : 9-v */
        nc[i] = (s >= 0) ? v : (9 - v);
    }
    if (invert) { nc[0] = 9 - nc[0]; nc[1] = 9 - nc[1]; nc[2] = 9 - nc[2]; }
    return (unsigned)(nc[0] * 100 + nc[1] * 10 + nc[2]);
}

/* canonical hash: min FNV ทุก (rot, invert) */
static uint64_t canon_hash(const uint8_t *cube) {
    uint64_t best = ~0ull;
    uint8_t tmp[CUBE];
    for (int inv = 0; inv < 2; inv++)
        for (int r = 0; r < nrot; r++) {
            for (unsigned i = 0; i < CUBE; i++) {
                unsigned x = i / 100, y = (i / 10) % 10, z = i % 10;
                tmp[rot_idx(x, y, z, r, inv)] = cube[i];
            }
            uint64_t h = 1469598103934665603ull;
            for (unsigned i = 0; i < CUBE; i++) {
                h ^= tmp[i];
                h *= 1099511628211ull;
            }
            if (h < best) best = h;
        }
    return best;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s model.gguf\n", argv[0]); return 2; }
    build_rots();
    printf("rotations: %d (+inv = %d)\n", nrot, nrot * 2);
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { printf("open fail\n"); return 1; }

    /* โหลด stream cubes (unsorted, stream order) */
    static uint8_t cubes[NORB_TEST][CUBE];
    unsigned got = 0;
    unsigned long long pos = 0;
    for (unsigned t = 0; t < box.n_tensors && got < NORB_TEST; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        unsigned long long nb = e->n_elems / 32;
        for (unsigned long long b = 0; b < nb && got < NORB_TEST; b++) {
            const uint8_t *blk = (const uint8_t *)e->data + b * 34;
            for (int k = 0; k < 32; k++) {
                cubes[got][pos++] = blk[k];
                if (pos == CUBE) { pos = 0; got++; if (got >= NORB_TEST) break; }
            }
        }
    }
    printf("cubes: %u\n", got);

    /* (a) sanity: orientations ของ cube แรก */
    uint8_t t0[CUBE], t1[CUBE];
    memcpy(t0, cubes[0], CUBE);
    uint64_t h0 = canon_hash(t0);
    int sane = 1;
    for (int inv = 0; inv < 2 && sane; inv++)
        for (int r = 0; r < nrot && sane; r++) {
            for (unsigned i = 0; i < CUBE; i++) {
                unsigned x = i / 100, y = (i / 10) % 10, z = i % 10;
                t1[rot_idx(x, y, z, r, inv)] = t0[i];
            }
            if (canon_hash(t1) != h0) sane = 0;
        }
    printf("sanity (48 orientations → 1 orbit): %s\n", sane ? "OK" : "FAIL");

    /* (b) orbit count */
    static uint64_t hs[NORB_TEST];
    for (unsigned i = 0; i < got; i++) hs[i] = canon_hash(cubes[i]);
    unsigned orbits = 0;
    for (unsigned i = 0; i < got; i++) {
        int seen = 0;
        for (unsigned j = 0; j < i; j++)
            if (hs[j] == hs[i]) { seen = 1; break; }
        if (!seen) orbits++;
    }
    printf("orbits: %u / %u cubes\n", orbits, got);
    printf("%s\n", orbits <= 60 ? "ORBIT COLLAPSE (Phase2 viable)" : "NO COLLAPSE (Phase2 as-specified dead)");
    return 0;
}
