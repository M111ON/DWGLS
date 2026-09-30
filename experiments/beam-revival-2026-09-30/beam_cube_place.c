/* tools/beam_cube_place.c — hilbert-field placement: tick → cell
 * ═══════════════════════════════════════════════════════════════════════════
 * สนาม hilbert (ยาวไม่จำกัดผ่าน N-cube sequence): tick → cube_no=tick/1000,
 * cell H(tick)=(tick*37)%1000 → (x,y,z). 37 coprime กับ 1000 → bijective
 * ใน cube เดียวกัน = ไม่มีชน. ค่า zigzag (sphere/axis/angle/r) หิ้วไปใน cell.
 * กู้: เดิน tick order อ่าน cell เดิม → เทียบค่าเดิม memcmp.
 *
 * BUILD: gcc -O2 -w -I. -Icore -o build/beam_cube_place.exe tools/beam_cube_place.c -lm
 * RUN:   ./build/beam_cube_place.exe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf [maxvals]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_box.h"

#define K_ANG 137u
#define CUBE 1000u
#define STRIDE_H 37u

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s model.gguf [maxvals]\n", argv[0]); return 2; }
    unsigned long long maxv = argc > 2 ? strtoull(argv[2], 0, 10) : 200000ull;
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { printf("open fail\n"); return 1; }

    /* เก็บ (value + r) ต่อ cell ต่อ cube: flat [ncubes][1000] */
    int8_t *orig = malloc(maxv ? maxv : 1);
    unsigned long long n = 0, tick = 0;
    /* pass 1: อ่านค่ามากองก่อน (รู้ n ถึงจะจอง cubes ได้) */
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
    unsigned long long ncubes = (n + CUBE - 1) / CUBE;
    int8_t *field = malloc(ncubes * CUBE);
    unsigned char *filled = calloc(ncubes * CUBE, 1);
    unsigned long long collisions = 0;
    tick = 0;
    for (unsigned long long i = 0; i < n; i++, tick++) {
        unsigned long long cno = tick / CUBE;
        unsigned h = (unsigned)((tick * STRIDE_H) % CUBE);
        unsigned long long at = cno * CUBE + h;
        if (filled[at]) collisions++;
        field[at] = orig[i];
        filled[at] = 1;
    }
    /* กู้ตาม tick order */
    unsigned long long bad = 0;
    for (unsigned long long i = 0; i < n; i++) {
        unsigned long long cno = i / CUBE;
        unsigned h = (unsigned)((i * STRIDE_H) % CUBE);
        if (field[cno * CUBE + h] != orig[i]) bad++;
    }
    printf("n=%llu cubes=%llu collisions=%llu bad=%llu\n",
           n, ncubes, collisions, bad);
    printf("%s\n", bad == 0 && collisions == 0 ? "HILBERT PLACE OK" : "HILBERT PLACE FAIL");
    free(orig);
    free(field);
    free(filled);
    return (bad == 0 && collisions == 0) ? 0 : 1;
}
