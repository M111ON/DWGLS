/* tools/beam_occurrence_probe.c — ตัดสิน occurrence index
 * ═══════════════════════════════════════════════════════════════════════════
 * beam6(v) = sign, axis=|v|%3, r=|v|, r2=|v|*7%128, r3=|v|*13%128, digit
 * (ทั้ง 6 เป็น f(v) deterministic — ยุติธรรมต่อกรอบ field)
 * Test 1: นับ distinct beam6 บนของจริง
 * Test 2: playback ด้วย beam-length ascending → offsets ตรงไหม
 *   (เก็บแค่ sorted values + beam6, ไม่เก็บ order ใดๆ)
 * ตัดสิน: offsets ตรง = perm ฟรี / ไม่ตรง = perm จริง
 *
 * BUILD: gcc -O2 -w -I. -Icore -o build/beam_occurrence_probe.exe tools/beam_occurrence_probe.c -lm
 * RUN:   ./build/beam_occurrence_probe.exe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf [maxvals]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_box.h"

static int cmp_i8(const void *a, const void *b) {
    return (int)(*(const int8_t *)a) - (int)(*(const int8_t *)b);
}

static int lead9(int v) {
    int a = v < 0 ? -v : v;
    if (a == 0) return 0;
    while (a >= 10) a /= 10;
    return a;
}

typedef struct { int8_t v; uint8_t sign, axis, r, r2, r3, digit; } Beam;

static Beam measure(int8_t v) {
    Beam b;
    b.v = v;
    b.sign = (v >= 0) ? 0 : 1;
    unsigned a = (v == -128) ? 128u : (unsigned)(v >= 0 ? v : -v);
    b.r = (uint8_t)a;
    b.axis = (uint8_t)(a % 3u);
    b.r2 = (uint8_t)((a * 7u) % 128u);
    b.r3 = (uint8_t)((a * 13u) % 128u);
    b.digit = (uint8_t)lead9(v);
    return b;
}

static int beam_eq(const Beam *x, const Beam *y) {
    return x->sign == y->sign && x->axis == y->axis && x->r == y->r &&
           x->r2 == y->r2 && x->r3 == y->r3 && x->digit == y->digit;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s model.gguf [maxvals]\n", argv[0]); return 2; }
    unsigned long long maxv = argc > 2 ? strtoull(argv[2], 0, 10) : 200000ull;
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { printf("open fail\n"); return 1; }

    int8_t *orig = malloc(maxv ? maxv : 1);
    unsigned long long n = 0;
    for (unsigned t = 0; t < box.n_tensors && n < maxv; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        unsigned long long nb = e->n_elems / 32;
        for (unsigned long long b = 0; b < nb && n < maxv; b++) {
            const uint8_t *blk = (const uint8_t *)e->data + b * 34;
            for (int k = 0; k < 32 && n < maxv; k++, n++)
                orig[n] = (int8_t)blk[k];
        }
    }

    /* Test 1: distinct beam6 */
    Beam *beams = malloc(n * sizeof(Beam));
    for (unsigned long long i = 0; i < n; i++) beams[i] = measure(orig[i]);
    unsigned long long distinct = 0;
    for (unsigned long long i = 0; i < n; i++) {
        int seen = 0;
        for (unsigned long long j = 0; j < i; j++)
            if (beam_eq(&beams[i], &beams[j])) { seen = 1; break; }
        if (!seen) distinct++;
    }
    printf("Test1: n=%llu distinct_beam6=%llu (ceil=256)\n", n, distinct);

    /* Test 2: playback ascending — เก็บแค่ sorted values + beam6 */
    int8_t *stored = malloc(n);
    memcpy(stored, orig, n);
    qsort(stored, n, 1, cmp_i8);
    /* playback: วางกลับตาม ascending order → เทียบ offset เดิมทีละตำแหน่ง */
    unsigned long long match = 0;
    for (unsigned long long i = 0; i < n; i++)
        if (stored[i] == orig[i]) match++;
    printf("Test2: ascending playback offset match=%llu/%llu (%.2f%%)\n",
           match, n, 100.0 * match / n);
    printf("%s\n", match == n ? "PERM FREE (field order works)" : "PERM REAL (order must be stored)");
    free(orig);
    free(beams);
    free(stored);
    return 0;
}
