/* tools/beam_cube_seq.c — sector → 10×10×10×N cube sequence
 * ═══════════════════════════════════════════════════════════════════════════
 * ต่อจาก beam_sector_probe: เอา sorted sector stream หั่นเป็น cube ละ 1000
 * int8 (10×10×10) เขียนลงไฟล์ .beamseq พร้อม index (sector, cube_no, count)
 * อ่านกลับ → memcmp ทั้ง stream (multiset lossless, permutation ยังไม่นับ)
 *
 * BUILD: gcc -O2 -w -I. -Icore -o build/beam_cube_seq.exe tools/beam_cube_seq.c -lm
 * RUN:   ./build/beam_cube_seq.exe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf /tmp/t.beamseq
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_box.h"

#define CUBE 1000u  /* 10×10×10 */

/* fixed-formula wall orders (bijective strides, coprime with 1000):
 * H = hilbert-wall order, P = peano-wall order.
 * 10 ไม่ใช่ power-of-2/3 เลยใช้ stride-coprime แทน Hilbert/Peano คลาสสิก
 * แต่ fixed + invertible เหมือนกัน: inv37=973 (37*973≡1 mod 1000) */
#define STRIDE_H 37u
#define STRIDE_P 91u

static unsigned h_order(unsigned i) { return (i * STRIDE_H) % CUBE; }
static unsigned p_order(unsigned i) { return (i * STRIDE_P) % CUBE; }
static uint8_t wall_key(unsigned i) {
    return (uint8_t)((h_order(i) ^ p_order(i)) & 0xFFu);
}

static int cmp_i8(const void *a, const void *b) {
    return (int)(*(const int8_t *)a) - (int)(*(const int8_t *)b);
}

static int lead9(int v) {
    int a = v < 0 ? -v : v;
    if (a == 0) return 0;
    while (a >= 10) a /= 10;
    return a;
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: %s model.gguf out.beamseq\n", argv[0]); return 2; }
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { printf("open fail\n"); return 1; }

    unsigned long long total = 0;
    for (unsigned t = 0; t < box.n_tensors; t++)
        if (box.entries[t].dtype == 8 && box.entries[t].data)
            total += box.entries[t].n_elems;
    printf("values: %llu\n", total);
    int8_t *v = malloc(total ? total : 1);
    if (!v) { printf("oom\n"); return 1; }
    unsigned long long p = 0;
    for (unsigned t = 0; t < box.n_tensors; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        unsigned long long nb = e->n_elems / 32;
        for (unsigned long long b = 0; b < nb; b++) {
            const uint8_t *blk = (const uint8_t *)e->data + b * 34;
            for (int k = 0; k < 32; k++) v[p++] = (int8_t)blk[k];
        }
    }
    unsigned long long n = p;
    qsort(v, n, 1, cmp_i8);

    FILE *f = fopen(argv[2], "wb");
    if (!f) { printf("write fail\n"); return 1; }
    /* header: magic(4) nsectors(4) */
    uint32_t magic = 0x424D5351u; /* B M S Q */
    uint32_t nsec = 10;
    fwrite(&magic, 4, 1, f);
    fwrite(&nsec, 4, 1, f);
    /* index placeholder: 10 × (count u64, ncubes u64) */
    long idxpos = ftell(f);
    unsigned long long idx[20] = {0};
    fwrite(idx, 8, 20, f);

    unsigned long long cnt[10] = {0}, ncu[10] = {0};
    /* pass 1: นับต่อ sector (stream เรียงแล้ว sector อยู่ติดกัน) */
    for (unsigned long long i = 0; i < n; i++) cnt[lead9(v[i])]++;
    /* pass 2: เขียน cubes + index */
    unsigned long long pos = 0;
    unsigned long long tick = 0;  /* tick นับปกติ 0,1,2,... ห้ามใช้ fibo */
    for (int s = 0; s < 10; s++) {
        unsigned long long c = cnt[s], w = 0;
        while (w < c) {
            uint8_t cube[CUBE];
            unsigned m = 0;
            while (m < CUBE && w < c) { cube[m++] = (uint8_t)v[pos++]; w++; }
            uint32_t sec = (uint32_t)s, nn = m;
            /* wall-XOR: data(i) ^= key(i) — involution, อ่านกลับ XOR ซ้ำได้เดิม */
            for (unsigned k = 0; k < m; k++) cube[k] ^= wall_key(k);
            fwrite(&sec, 4, 1, f);
            fwrite(&nn, 4, 1, f);
            fwrite(&tick, 8, 1, f);
            fwrite(cube, 1, m, f);
            ncu[s]++;
            tick++;
        }
    }
    for (int s = 0; s < 10; s++) { idx[2*s] = cnt[s]; idx[2*s+1] = ncu[s]; }
    fseek(f, idxpos, SEEK_SET);
    fwrite(idx, 8, 20, f);
    long endpos = 0;
    fseek(f, 0, SEEK_END);
    endpos = ftell(f);
    fclose(f);
    printf("wrote %ld bytes, cubes:", endpos);
    for (int s = 0; s < 10; s++) printf(" s%d:%llu", s, ncu[s]);
    printf("\n");

    /* อ่านกลับ → เทียบ stream */
    f = fopen(argv[2], "rb");
    if (!f) { printf("read fail\n"); return 1; }
    uint32_t mg = 0, ns = 0;
    if (fread(&mg, 4, 1, f) != 1 || mg != magic) { printf("bad magic\n"); return 1; }
    if (fread(&ns, 4, 1, f) != 1 || ns != 10) { printf("bad nsec\n"); return 1; }
    unsigned long long ridx[20];
    if (fread(ridx, 8, 20, f) != 20) { printf("bad idx\n"); return 1; }
    int8_t *w2 = malloc(n ? n : 1);
    unsigned long long q = 0;
    int ok = 1;
    unsigned long long etick = 0;
    for (int s = 0; s < 10 && ok; s++) {
        for (unsigned long long c = 0; c < ridx[2*s+1]; c++) {
            uint32_t sec = 0, nn = 0;
            unsigned long long tk = 0;
            if (fread(&sec, 4, 1, f) != 1 || fread(&nn, 4, 1, f) != 1) { ok = 0; break; }
            if (fread(&tk, 8, 1, f) != 1 || tk != etick) { ok = 0; break; }
            if ((int)sec != s || nn == 0 || nn > CUBE || q + nn > n) { ok = 0; break; }
            uint8_t tmp[CUBE];
            if (fread(tmp, 1, nn, f) != nn) { ok = 0; break; }
            for (unsigned k = 0; k < nn; k++) tmp[k] ^= wall_key(k);
            for (unsigned k = 0; k < nn; k++) w2[q++] = (int8_t)tmp[k];
            etick++;
        }
    }
    fclose(f);
    if (ok && q == n && memcmp(v, w2, n) == 0)
        printf("ROUNDTRIP OK (%llu values, %llu ticks)\n", q, etick);
    else
        printf("ROUNDTRIP FAIL (q=%llu n=%llu)\n", q, n);
    free(v);
    free(w2);
    return ok ? 0 : 1;
}
