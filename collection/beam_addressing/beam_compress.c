/*
 * beam_compress.c — Compress 33B block below Q8_0
 *
 * Key insight from analysis:
 *   Block range avg=218.6 → nearly full range → delta/group can't compress much
 *   But: 13% zeros, 21% small weights → bitmap can help for some blocks
 *   And: per-group min+delta helps when group range is small
 *
 * Methods (all lossless, all preserve original order):
 *   A) Raw 8-bit (33B) — fallback
 *   B) Bitmap + exceptions — if many weights equal one value
 *   C) Per-group min + variable delta — if groups have small range
 *   D) Adaptive bit-width — store block range, use fewer bits if range is small
 *
 * Compile: gcc -O2 beam_compress.c -o beam_compress.exe
 * Run: ./beam_compress.exe [model.gguf]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════
   BIT PACKING
   ══════════════════════════════════════════════════════════════ */

static void wbits(uint8_t *b, int p, int v, int nb) {
    for (int i = 0; i < nb; i++)
        if (v & (1 << i)) b[(p + i) / 8] |= 1 << ((p + i) % 8);
}

static int rbits(const uint8_t *b, int p, int nb) {
    int v = 0;
    for (int i = 0; i < nb; i++)
        if (b[(p + i) / 8] & (1 << ((p + i) % 8))) v |= 1 << i;
    return v;
}

/* ══════════════════════════════════════════════════════════════
   METHOD A: Raw 8-bit (33 bytes)
   ══════════════════════════════════════════════════════════════ */

static int encode_raw(uint8_t *out, const int8_t *w, int n) {
    out[0] = 0x0A;
    for (int i = 0; i < n; i++) out[1 + i] = (uint8_t)(w[i] + 128);
    return 1 + n;
}

static void decode_raw(int8_t *out, const uint8_t *buf, int n) {
    for (int i = 0; i < n; i++) out[i] = (int8_t)(buf[1 + i] - 128);
}

/* ══════════════════════════════════════════════════════════════
   METHOD B: Bitmap + Exceptions
   ══════════════════════════════════════════════════════════════
 *
 * Find most frequent weight → base
 * [tag:4][base:8][bitmap:32 bits][exception values]
 * Size = ceil((44 + exceptions×8) / 8) bytes
 */

static int encode_bitmap(uint8_t *out, const int8_t *w, int n) {
    int freq[256] = {0};
    for (int i = 0; i < n; i++) freq[w[i] + 128]++;
    int best = 0, best_freq = 0;
    for (int i = 0; i < 256; i++) {
        if (freq[i] > best_freq) { best_freq = freq[i]; best = i - 128; }
    }

    int exceptions = n - best_freq;
    int size_bits = 48 + exceptions * 8;
    int size_bytes = (size_bits + 7) / 8;
    if (size_bytes >= 33) return -1;

    memset(out, 0, size_bytes + 1);
    out[0] = 0x0B;
    int pos = 8;
    wbits(out, pos, best + 128, 8); pos += 8;

    uint32_t bitmap = 0;
    for (int i = 0; i < n; i++) {
        if (w[i] != best) bitmap |= (1u << i);
    }
    wbits(out, pos, bitmap, 32); pos += 32;

    for (int i = 0; i < n; i++) {
        if (w[i] != best) {
            wbits(out, pos, w[i] + 128, 8); pos += 8;
        }
    }

    return (pos + 7) / 8;
}

static void decode_bitmap(int8_t *out, const uint8_t *buf, int n) {
    int pos = 8;
    int base = rbits(buf, pos, 8) - 128; pos += 8;
    uint32_t bitmap = (uint32_t)rbits(buf, pos, 32); pos += 32;

    for (int i = 0; i < n; i++) {
        if (bitmap & (1u << i)) {
            out[i] = (int8_t)(rbits(buf, pos, 8) - 128); pos += 8;
        } else {
            out[i] = (int8_t)base;
        }
    }
}

/* ══════════════════════════════════════════════════════════════
   METHOD C: Group-of-4 Min + Variable Delta
   ══════════════════════════════════════════════════════════════
 *
 * 8 groups of 4 weights.
 * Per group: min(8 bits) + offset_bits(3) + 3×offset
 * Total = 4 + 8×(11 + 3×offset_bits) bits
 */

static int encode_group4(uint8_t *out, const int8_t *w, int n) {
    out[0] = 0x0C;
    int pos = 8;

    for (int g = 0; g < 8; g++) {
        int base = w[g*4+0];
        int max_abs_off = 0;
        for (int j = 1; j < 4; j++) {
            int off = w[g*4+j] - base;
            if (off < 0) off = -off;
            if (off > max_abs_off) max_abs_off = off;
        }

        /* Need off_bits to represent [-max_abs_off, +max_abs_off] */
        int off_bits = 1;
        while ((1 << off_bits) <= 2 * max_abs_off) off_bits++;
        if (off_bits > 8) off_bits = 8;

        wbits(out, pos, w[g*4+0] + 128, 8); pos += 8;
        wbits(out, pos, off_bits, 3); pos += 3;
        for (int j = 1; j < 4; j++) {
            int off = w[g*4+j] - w[g*4+0];
            int shifted = off + (1 << (off_bits - 1));
            if (shifted < 0) shifted = 0;
            if (shifted >= (1 << off_bits)) shifted = (1 << off_bits) - 1;
            wbits(out, pos, shifted, off_bits); pos += off_bits;
        }
    }

    return (pos + 7) / 8;
}

static void decode_group4(int8_t *out, const uint8_t *buf, int n) {
    int pos = 8;
    for (int g = 0; g < 8; g++) {
        int base = rbits(buf, pos, 8) - 128; pos += 8;
        int off_bits = rbits(buf, pos, 3); pos += 3;
        out[g*4+0] = (int8_t)base;
        for (int j = 1; j < 4; j++) {
            int shifted = rbits(buf, pos, off_bits); pos += off_bits;
            int off = shifted - (1 << (off_bits - 1));
            out[g*4+j] = (int8_t)(base + off);
        }
    }
}

/* ══════════════════════════════════════════════════════════════
   METHOD D: Adaptive Bit-Width
   ══════════════════════════════════════════════════════════════
 *
 * Find block range [min, max].
 * If range < 256: store min(8) + range_bits(3) + 32×range_bits
 * If range ≥ 256: fall back to raw
 *
 * Size = 4 + 8 + 3 + 32×range_bits = 15 + 32×range_bits bits
 */

static int encode_adaptive(uint8_t *out, const int8_t *w, int n) {
    int mn = 127, mx = -128;
    for (int i = 0; i < n; i++) {
        if (w[i] < mn) mn = w[i];
        if (w[i] > mx) mx = w[i];
    }
    int range = mx - mn;

    int range_bits = 1;
    while ((1 << range_bits) <= range) range_bits++;
    if (range_bits > 8) return -1; /* fall back to raw */

    int size_bits = 19 + n * range_bits;
    int size_bytes = (size_bits + 7) / 8;
    if (size_bytes >= 33) return -1;

    memset(out, 0, size_bytes + 1);
    out[0] = 0x0D;
    int pos = 8;
    wbits(out, pos, mn + 128, 8); pos += 8;
    wbits(out, pos, range_bits, 3); pos += 3;

    for (int i = 0; i < n; i++) {
        int off = w[i] - mn;
        wbits(out, pos, off, range_bits); pos += range_bits;
    }

    return (pos + 7) / 8;
}

static void decode_adaptive(int8_t *out, const uint8_t *buf, int n) {
    int pos = 8;
    int base = rbits(buf, pos, 8) - 128; pos += 8;
    int range_bits = rbits(buf, pos, 3); pos += 3;

    for (int i = 0; i < n; i++) {
        int off = rbits(buf, pos, range_bits); pos += range_bits;
        out[i] = (int8_t)(base + off);
    }
}

/* Forward declaration */
static void decode_best(int8_t *out, const uint8_t *buf, int n);

/* ══════════════════════════════════════════════════════════════
   BEST-FIT ENCODER
   ══════════════════════════════════════════════════════════════ */

static int encode_best(uint8_t *out, const int8_t *w, int n) {
    uint8_t tmp[128];
    int best_sz = 999;
    int best_method = 0;

    int sz;
    memset(tmp, 0, sizeof(tmp));
    sz = encode_raw(tmp, w, n);
    if (sz < best_sz) { best_sz = sz; best_method = 0; }

    memset(tmp, 0, sizeof(tmp));
    sz = encode_bitmap(tmp, w, n);
    if (sz > 0 && sz < best_sz) { best_sz = sz; best_method = 1; }

    memset(tmp, 0, sizeof(tmp));
    sz = encode_group4(tmp, w, n);
    if (sz > 0 && sz < best_sz) { best_sz = sz; best_method = 2; }

    memset(tmp, 0, sizeof(tmp));
    sz = encode_adaptive(tmp, w, n);
    if (sz > 0 && sz < best_sz) { best_sz = sz; best_method = 3; }

    memset(out, 0, 128);
    switch (best_method) {
        case 0: encode_raw(out, w, n); break;
        case 1: encode_bitmap(out, w, n); break;
        case 2: encode_group4(out, w, n); break;
        case 3: encode_adaptive(out, w, n); break;
        default: encode_raw(out, w, n); break;
    }

    /* Internal verify: encode→decode must match */
    int8_t verify[32];
    decode_best(verify, out, n);
    for (int i = 0; i < n; i++) {
        if (verify[i] != w[i]) {
            /* Fall back to raw */
            memset(out, 0, 128);
            encode_raw(out, w, n);
            return 1 + n;
        }
    }

    return best_sz;
}

static void decode_best(int8_t *out, const uint8_t *buf, int n) {
    uint8_t tag = buf[0];
    switch (tag) {
        case 0x0A: decode_raw(out, buf, n); break;
        case 0x0B: decode_bitmap(out, buf, n); break;
        case 0x0C: decode_group4(out, buf, n); break;
        case 0x0D: decode_adaptive(out, buf, n); break;
        default: decode_raw(out, buf, n); break;
    }
}

/* ══════════════════════════════════════════════════════════════
   TESTS
   ══════════════════════════════════════════════════════════════ */

static void test_roundtrip(void) {
    printf("=== Roundtrip Test ===\n");
    int pass = 0, fail = 0;
    srand(42);

    for (int trial = 0; trial < 10000; trial++) {
        int8_t w[32];
        for (int i = 0; i < 32; i++) w[i] = (int8_t)(rand() % 256 - 128);

        uint8_t buf[128];
        memset(buf, 0, sizeof(buf));
        int sz = encode_best(buf, w, 32);

        int8_t dec[32];
        memset(dec, 0, sizeof(dec));
        decode_best(dec, buf, 32);

        int ok = 1;
        for (int i = 0; i < 32; i++) {
            if (dec[i] != w[i]) { ok = 0; break; }
        }
        if (ok) pass++; else {
            fail++;
            if (fail <= 3) {
                printf("  FAIL trial %d: method=0x%02X sz=%d\n", trial, buf[0], sz);
                printf("    orig: ");
                for (int i = 0; i < 8; i++) printf("%d ", w[i]);
                printf("...\n    dec:  ");
                for (int i = 0; i < 8; i++) printf("%d ", dec[i]);
                printf("...\n");
            }
        }
    }

    printf("  PASS: %d/10000  FAIL: %d\n\n", pass, fail);
}

static void test_real_model(const char *path) {
    printf("=== Real Model Test ===\n");

    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return; }
    uint32_t magic; fread(&magic, 4, 1, f);
    if (magic != 0x46554747) { fprintf(stderr, "Not GGUF\n"); fclose(f); return; }
    uint32_t ver; fread(&ver, 4, 1, f);
    uint64_t nt; fread(&nt, 8, 1, f);
    uint64_t nk; fread(&nk, 8, 1, f);

    for (uint64_t i = 0; i < nk; i++) {
        uint64_t kl; fread(&kl, 8, 1, f); fseek(f, kl, SEEK_CUR);
        uint32_t vt; fread(&vt, 4, 1, f);
        switch(vt) {
            case 0: case 1: case 7: fseek(f,1,SEEK_CUR); break;
            case 2: case 3: fseek(f,2,SEEK_CUR); break;
            case 4: case 5: case 6: fseek(f,4,SEEK_CUR); break;
            case 8: { uint64_t l; fread(&l,8,1,f); fseek(f,l,SEEK_CUR); break; }
            case 9: { uint32_t et; fread(&et,4,1,f); uint64_t al; fread(&al,8,1,f);
                      for(uint64_t j=0;j<al;j++){if(et==8){uint64_t l2;fread(&l2,8,1,f);fseek(f,l2,SEEK_CUR);}
                      else fseek(f,(et<=1?1:et<=3?2:et<=6?4:et==7?1:8),SEEK_CUR);} break; }
            case 10: case 11: case 12: fseek(f,8,SEEK_CUR); break;
            default: fclose(f); return;
        }
    }

    for (uint64_t i = 0; i < nt; i++) {
        uint64_t nl; fread(&nl, 8, 1, f); fseek(f, nl, SEEK_CUR);
        uint32_t nd; fread(&nd, 4, 1, f);
        uint64_t nw = 1;
        for (uint32_t d = 0; d < nd && d < 4; d++) { uint64_t dm; fread(&dm,8,1,f); nw *= dm; }
        uint32_t dt; fread(&dt, 4, 1, f);
        uint64_t off; fread(&off, 8, 1, f);

        if (dt == 8) {
            printf("  Tensor: Q8_0, %llu weights\n", (unsigned long long)nw);
            long ds = ftell(f);
            int nb = (int)(nw / 32);
            int nt2 = nb > 2000 ? 2000 : nb;
            uint8_t *raw = malloc((size_t)nt2 * 33);
            fseek(f, ds, SEEK_SET);
            fread(raw, 1, (size_t)nt2 * 33, f);

            int total_sz = 0;
            int lossless = 1;
            int total_pass = 0;
            int method_count[4] = {0};
            int method_bytes[4] = {0};

            for (int b = 0; b < nt2; b++) {
                int8_t w8[32];
                for (int j = 0; j < 32; j++)
                    w8[j] = (int8_t)raw[b * 33 + 2 + j];

                uint8_t buf2[128];
                memset(buf2, 0, sizeof(buf2));
                int sz = encode_best(buf2, w8, 32);
                total_sz += sz;

                int method = -1;
                switch (buf2[0]) {
                    case 0x0A: method = 0; break;
                    case 0x0B: method = 1; break;
                    case 0x0C: method = 2; break;
                    case 0x0D: method = 3; break;
                }
                if (method >= 0) { method_count[method]++; method_bytes[method] += sz; }

                int8_t dec[32];
                memset(dec, 0, sizeof(dec));
                decode_best(dec, buf2, 32);
                for (int j = 0; j < 32; j++) {
                    if (dec[j] != w8[j]) { lossless = 0; }
                }
                for (int j = 0; j < 32; j++) if (dec[j] == w8[j]) total_pass++;
            }

            double avg = (double)total_sz / nt2;
            printf("  Blocks: %d\n", nt2);
            printf("  Avg size: %.2f bytes/block\n", avg);
            printf("  Lossless: %s\n", lossless ? "YES ✓" : "NO");
            printf("  Exact: %d/%d\n", total_pass, nt2 * 32);
            printf("  vs Q8_0 (34 B): %.4fx\n", avg / 34.0);
            printf("  vs Raw (33 B):  %.4fx\n", avg / 33.0);
            printf("\n  Method usage:\n");
            const char *names[] = {"A:raw", "B:bitmap", "C:grp4", "D:adapt"};
            for (int m = 0; m < 4; m++) {
                if (method_count[m] > 0)
                    printf("    %s: %d blocks, avg %.1f B\n", names[m],
                           method_count[m], (double)method_bytes[m]/method_count[m]);
            }

            free(raw);
            break;
        }
    }
    fclose(f);
}

int main(int argc, char **argv) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Beam Compress — Adaptive Block Codec                  ║\n");
    printf("║  A:raw(33B) B:bitmap C:grp4min+delta D:adapt bits    ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_roundtrip();

    if (argc >= 2) test_real_model(argv[1]);
    else printf("Usage: %s <model.gguf>\n", argv[0]);

    return 0;
}
