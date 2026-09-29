/* gate_huff.c — fold gates + canonical Huffman, real ratio (experiment).
 * Reuses core/huff_codec.h (ladder rung 2 — no new codec).
 * Fold n=4, stride s: pivot raw (1B) + gates as zigzag symbols:
 *   zz 0..254 -> symbol zz ; zz >= 255 -> ESC(255) + 1 raw byte (zz-255).
 * Table cost counted honestly: lens[256] = 256 B per stream table.
 * Verifies by full decode + unfold + memcmp (lossless or it didn't happen).
 * Usage: gate_huff <file> <offset> <stride>   (stride 2 = 16-bit deinterleave)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../core/huff_codec.h"

static int8_t  g_data[1 << 20];
static uint8_t g_piv[1 << 20], g_sym[1 << 20], g_esc[1 << 20];
static uint8_t g_coded[1 << 20], g_dsym[1 << 20];

static unsigned zz(int v) { return (unsigned)((v << 1) ^ (v >> 31)); }
static int unzz(unsigned z) { return (int)((z >> 1) ^ -(int)(z & 1)); }

int main(int argc, char **argv) {
    if (argc < 4) { printf("usage: %s <file> <offset> <stride>\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 1; }
    fseek(f, atol(argv[2]), SEEK_SET);
    size_t got = fread(g_data, 1, sizeof g_data, f);
    fclose(f);
    if (got < 1024) { printf("short read\n"); return 1; }
    int nc = (int)(got & ~(size_t)63);
    int s = atoi(argv[3]);
    printf("SPEC: decode+unfold+memcmp must be exact; ratio counts pivots+coded+esc+256B table\n");
    long total = 0;
    for (int par = 0; par < s; par++) {
        int np = 0, ns = 0, ne = 0, span = 4 * s;
        for (int i = par; i + span <= nc; i += span) {
            g_piv[np++] = (uint8_t)g_data[i];
            for (int k = 1; k < 4; k++) {
                unsigned z = zz((int)g_data[i + k * s] - (int)g_data[i]);
                if (z < 255) g_sym[ns++] = (uint8_t)z;
                else { g_sym[ns++] = 255; g_esc[ne++] = (uint8_t)(z - 255); }
            }
        }
        uint64_t freq[256] = {0};
        for (int i = 0; i < ns; i++) freq[g_sym[i]]++;
        static HuffModel m, dm;
        huff_build(&m, freq);
        uint32_t cb = huff_encode(&m, g_sym, ns, g_coded, sizeof g_coded);
        if (cb == 0 && ns > 0) { printf("par%d ENCODE-FAIL\n", par); return 1; }
        /* clean verify */
        huff_rebuild(&dm, m.lens);
        if (huff_decode(&dm, g_coded, cb, g_dsym, ns) != 0) { printf("par%d DECODE-FAIL\n", par); return 1; }
        int si = 0, ei = 0, pi = 0, bad = 0;
        for (int i = par; i + span <= nc; i += span) {
            int8_t piv = (int8_t)g_piv[pi++];
            if (g_data[i] != piv) bad++;
            for (int k = 1; k < 4; k++) {
                uint8_t sym = g_dsym[si++];
                unsigned z = (sym == 255) ? 255u + g_esc[ei++] : sym;
                int8_t v = (int8_t)(piv + unzz(z));
                if (g_data[i + k * s] != v) bad++;
            }
        }
        long stream_total = np + cb + ne + 256;
        total += stream_total;
        printf("par%d: piv=%ld coded=%u esc=%d +tab=256 -> %ld  %s\n",
               par, (long)np, cb, ne, stream_total, bad ? "MISMATCH" : "EXACT");
    }
    printf("TOTAL %ld / %d = %.4f\n", total, nc, (double)total / nc);
    return 0;
}
