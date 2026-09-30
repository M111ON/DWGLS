/* tools/beam_carry_xfield.c — carry ข้ามแดน: local-coord vs global-coord
 * field A: groups of 12 → fold x:-x, carry = (group, idx_local, value)
 * ข้ามไป field B: groups of 8 → re-anchor carry
 *   local: เอา (group, idx) เดิมมาใช้ดื้อๆ → นับ orphan (idx>=8 / ค่าผิด)
 *   global: แปลงเป็น global pos ก่อนข้าม → re-resolve → ต้องรอด 100%
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_box.h"

#define GA 12
#define GB 8

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s model.gguf [N]\n", argv[0]); return 2; }
    unsigned long long N = argc > 2 ? strtoull(argv[2], 0, 10) : 24000ull;
    N -= N % GA;
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { printf("open fail\n"); return 1; }
    int8_t *v = malloc(N ? N : 1);
    unsigned long long n = 0;
    for (unsigned t = 0; t < box.n_tensors && n < N; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        unsigned long long nb = e->n_elems / 32;
        for (unsigned long long b = 0; b < nb && n < N; b++) {
            const uint8_t *blk = (const uint8_t *)e->data + b * 34;
            for (int k = 0; k < 32 && n < N; k++, n++)
                v[n] = (int8_t)blk[k];
        }
    }
    /* fold field A: greedy local pairs per group, carry=(gA, idx, val) */
    typedef struct { unsigned g; unsigned idx; int8_t val; unsigned long long gpos; } Carry;
    Carry *car = malloc(n * sizeof(Carry));
    unsigned long long ncar = 0;
    for (unsigned long long g = 0; g < n / GA; g++) {
        int used[GA] = {0};
        for (int i = 0; i < GA; i++) {
            if (used[i] || v[g * GA + i] == 0) continue;
            for (int j = i + 1; j < GA; j++) {
                if (!used[j] && v[g * GA + j] == (int8_t)(-v[g * GA + i])) {
                    used[i] = used[j] = 1;
                    car[ncar].g = (unsigned)g;
                    car[ncar].idx = (unsigned)i;
                    car[ncar].val = v[g * GA + i];
                    car[ncar].gpos = g * GA + i;
                    ncar++;
                    break;
                }
            }
        }
    }
    /* ข้ามแดน: local re-anchor (ใช้ g,idx เดิมใน field B) */
    unsigned long long orph_local = 0, wrong_local = 0, checked = 0;
    for (unsigned long long c = 0; c < ncar; c++) {
        /* field B: group = g % (n/GB), idx ต้อง < GB */
        unsigned gB = car[c].g % (unsigned)(n / GB);
        if (car[c].idx >= GB) { orph_local++; continue; }
        /* ค่าที่ตำแหน่งนั้นใน field B (interpret ดื้อๆ): คนละค่า = ผิดบ้าน */
        unsigned long long at = (unsigned long long)gB * GB + car[c].idx;
        if (at >= n) { orph_local++; continue; }
        checked++;
        if (v[at] != car[c].val && v[at] != (int8_t)(-car[c].val)) wrong_local++;
    }
    /* global re-anchor: gpos ตรง → รอดเสมอ */
    unsigned long long orph_global = 0;
    for (unsigned long long c = 0; c < ncar; c++)
        if (car[c].gpos >= n) orph_global++;
    printf("carries=%llu local: orphan=%llu wrong_home=%llu/%llu | global: orphan=%llu\n",
           ncar, orph_local, wrong_local, checked, orph_global);
    free(v);
    free(car);
    return 0;
}
