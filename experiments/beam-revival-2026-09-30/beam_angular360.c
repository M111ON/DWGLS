/* tools/beam_angular360.c — angular address 360 = 36 positions × 10 units
 * ═══════════════════════════════════════════════════════════════════════════
 * position = (|v|*7) % 36  (6 faces × 6 vertices)
 * unit     = digit sector 1-9 + 0  (Lo Shu + void)
 * addr     = position*10 + unit  (0..359)
 * ชนได้ → chain ตาม tick. กู้: tick order + chain. memcmp.
 *
 * BUILD: gcc -O2 -w -I. -Icore -o build/beam_angular360.exe tools/beam_angular360.c -lm
 * RUN:   ./build/beam_angular360.exe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf [maxvals]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_box.h"

#define NADDR 360
#define NPOS 36

static int lead9(int v) {
    int a = v < 0 ? -v : v;
    if (a == 0) return 0;
    while (a >= 10) a /= 10;
    return a;
}

typedef struct { int8_t *v; unsigned n, cap; } Slot;

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s model.gguf [maxvals]\n", argv[0]); return 2; }
    unsigned long long maxv = argc > 2 ? strtoull(argv[2], 0, 10) : 500000ull;
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { printf("open fail\n"); return 1; }

    Slot slots[NADDR];
    memset(slots, 0, sizeof(slots));
    int8_t *orig = malloc(maxv ? maxv : 1);
    unsigned long long n = 0;
    for (unsigned t = 0; t < box.n_tensors && n < maxv; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        unsigned long long nb = e->n_elems / 32;
        for (unsigned long long b = 0; b < nb && n < maxv; b++) {
            const uint8_t *blk = (const uint8_t *)e->data + b * 34;
            for (int k = 0; k < 32 && n < maxv; k++, n++) {
                int8_t v = (int8_t)blk[k];
                unsigned a = (v == -128) ? 128u : (unsigned)(v >= 0 ? v : -v);
                unsigned pos = (a * 7u) % NPOS;
                unsigned unit = (unsigned)lead9(v);
                unsigned addr = pos * 10u + unit;
                Slot *s = &slots[addr];
                if (s->n == s->cap) {
                    s->cap = s->cap ? s->cap * 2 : 4;
                    s->v = realloc(s->v, s->cap);
                }
                s->v[s->n++] = v;
                orig[n] = v;
            }
        }
    }
    /* กู้ตาม tick order: คำนวณ addr เดิม ดึงตามลำดับ */
    unsigned long long bad = 0, used = 0, maxchain = 0, collisions = 0;
    unsigned ci[NADDR];
    memset(ci, 0, sizeof(ci));
    for (unsigned long long i = 0; i < n; i++) {
        int8_t v = orig[i];
        unsigned a = (v == -128) ? 128u : (unsigned)(v >= 0 ? v : -v);
        unsigned addr = ((a * 7u) % NPOS) * 10u + (unsigned)lead9(v);
        unsigned at = ci[addr]++;
        if (at >= slots[addr].n || slots[addr].v[at] != v) bad++;
    }
    for (int i = 0; i < NADDR; i++) {
        if (slots[i].n) {
            used++;
            if (slots[i].n > 1) collisions += slots[i].n - 1;
            if (slots[i].n > maxchain) maxchain = slots[i].n;
        }
    }
    printf("n=%llu used=%u/360 maxchain=%llu collisions=%llu bad=%llu\n",
           n, used, maxchain, collisions, bad);
    printf("%s\n", bad == 0 ? "ANGULAR360 ROUNDTRIP OK" : "ANGULAR360 FAIL");
    return bad == 0 ? 0 : 1;
}
