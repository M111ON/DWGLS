/* beam_compress_fixed.c — corrected measurement of beam_compress codec
 *
 * Fixes versus the original beam_compress.c test_real_model():
 *  1. STRIDE: 34 bytes per Q8_0 block [2B f16 scale + 32 int8 weights],
 *     NOT 33. Original read with stride 33 -> misaligned after block 0.
 *  2. SCALE: the 2-byte f16 scale was dropped. A real block must store it too.
 *
 * Logic (encode_best/decode_best) is copied verbatim from the original —
 * only the harness that feeds bytes is corrected.
 *
 * Compile: gcc -O2 beam_compress_fixed.c -o beam_compress_fixed.exe
 * Run:     ./beam_compress_fixed.exe <model.gguf>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

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

/* ---- codec (verbatim from beam_compress.c) ---- */
static int encode_raw(uint8_t *out, const int8_t *w, int n) {
    out[0] = 0x0A;
    for (int i = 0; i < n; i++) out[1 + i] = (uint8_t)(w[i] + 128);
    return 1 + n;
}
static void decode_raw(int8_t *out, const uint8_t *buf, int n) {
    for (int i = 0; i < n; i++) out[i] = (int8_t)(buf[1 + i] - 128);
}
static int encode_bitmap(uint8_t *out, const int8_t *w, int n) {
    int freq[256] = {0};
    for (int i = 0; i < n; i++) freq[w[i] + 128]++;
    int best = 0, best_freq = 0;
    for (int i = 0; i < 256; i++) if (freq[i] > best_freq) { best_freq = freq[i]; best = i - 128; }
    int exceptions = n - best_freq;
    int size_bits = 48 + exceptions * 8;
    int size_bytes = (size_bits + 7) / 8;
    if (size_bytes >= 33) return -1;
    memset(out, 0, size_bytes + 1);
    out[0] = 0x0B;
    int pos = 8;
    wbits(out, pos, best + 128, 8); pos += 8;
    uint32_t bitmap = 0;
    for (int i = 0; i < n; i++) if (w[i] != best) bitmap |= (1u << i);
    wbits(out, pos, bitmap, 32); pos += 32;
    for (int i = 0; i < n; i++) if (w[i] != best) { wbits(out, pos, w[i] + 128, 8); pos += 8; }
    return (pos + 7) / 8;
}
static void decode_bitmap(int8_t *out, const uint8_t *buf, int n) {
    int pos = 8;
    int base = rbits(buf, pos, 8) - 128; pos += 8;
    uint32_t bitmap = (uint32_t)rbits(buf, pos, 32); pos += 32;
    for (int i = 0; i < n; i++) {
        if (bitmap & (1u << i)) { out[i] = (int8_t)(rbits(buf, pos, 8) - 128); pos += 8; }
        else out[i] = (int8_t)base;
    }
}
static int encode_group4(uint8_t *out, const int8_t *w, int n) {
    (void)n;
    out[0] = 0x0C;
    int pos = 8;
    for (int g = 0; g < 8; g++) {
        int base = w[g*4+0];
        int max_abs_off = 0;
        for (int j = 1; j < 4; j++) { int off = w[g*4+j]-base; if (off<0) off=-off; if (off>max_abs_off) max_abs_off=off; }
        int off_bits = 1;
        while ((1 << off_bits) <= 2 * max_abs_off) off_bits++;
        if (off_bits > 8) off_bits = 8;
        wbits(out, pos, w[g*4+0] + 128, 8); pos += 8;
        wbits(out, pos, off_bits, 3); pos += 3;
        for (int j = 1; j < 4; j++) {
            int off = w[g*4+j]-w[g*4+0];
            int shifted = off + (1 << (off_bits - 1));
            if (shifted < 0) shifted = 0;
            if (shifted >= (1 << off_bits)) shifted = (1 << off_bits) - 1;
            wbits(out, pos, shifted, off_bits); pos += off_bits;
        }
    }
    return (pos + 7) / 8;
}
static void decode_group4(int8_t *out, const uint8_t *buf, int n) {
    (void)n;
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
static int encode_adaptive(uint8_t *out, const int8_t *w, int n) {
    int mn = 127, mx = -128;
    for (int i = 0; i < n; i++) { if (w[i] < mn) mn = w[i]; if (w[i] > mx) mx = w[i]; }
    int range = mx - mn;
    int range_bits = 1;
    while ((1 << range_bits) <= range) range_bits++;
    if (range_bits > 8) return -1;
    int size_bits = 19 + n * range_bits;
    int size_bytes = (size_bits + 7) / 8;
    if (size_bytes >= 33) return -1;
    memset(out, 0, size_bytes + 1);
    out[0] = 0x0D;
    int pos = 8;
    wbits(out, pos, mn + 128, 8); pos += 8;
    wbits(out, pos, range_bits, 3); pos += 3;
    for (int i = 0; i < n; i++) { int off = w[i] - mn; wbits(out, pos, off, range_bits); pos += range_bits; }
    return (pos + 7) / 8;
}
static void decode_adaptive(int8_t *out, const uint8_t *buf, int n) {
    int pos = 8;
    int base = rbits(buf, pos, 8) - 128; pos += 8;
    int range_bits = rbits(buf, pos, 3); pos += 3;
    for (int i = 0; i < n; i++) { int off = rbits(buf, pos, range_bits); pos += range_bits; out[i] = (int8_t)(base + off); }
}
static void decode_best(int8_t *out, const uint8_t *buf, int n) {
    switch (buf[0]) {
        case 0x0A: decode_raw(out, buf, n); break;
        case 0x0B: decode_bitmap(out, buf, n); break;
        case 0x0C: decode_group4(out, buf, n); break;
        case 0x0D: decode_adaptive(out, buf, n); break;
        default: decode_raw(out, buf, n); break;
    }
}
static int encode_best(uint8_t *out, const int8_t *w, int n) {
    uint8_t tmp[128];
    int best_sz = 999, best_method = 0, sz;
    memset(tmp, 0, sizeof(tmp)); sz = encode_raw(tmp, w, n); if (sz < best_sz) { best_sz = sz; best_method = 0; }
    memset(tmp, 0, sizeof(tmp)); sz = encode_bitmap(tmp, w, n); if (sz > 0 && sz < best_sz) { best_sz = sz; best_method = 1; }
    memset(tmp, 0, sizeof(tmp)); sz = encode_group4(tmp, w, n); if (sz > 0 && sz < best_sz) { best_sz = sz; best_method = 2; }
    memset(tmp, 0, sizeof(tmp)); sz = encode_adaptive(tmp, w, n); if (sz > 0 && sz < best_sz) { best_sz = sz; best_method = 3; }
    memset(out, 0, 128);
    switch (best_method) {
        case 0: encode_raw(out, w, n); break;
        case 1: encode_bitmap(out, w, n); break;
        case 2: encode_group4(out, w, n); break;
        case 3: encode_adaptive(out, w, n); break;
        default: encode_raw(out, w, n); break;
    }
    int8_t verify[32];
    decode_best(verify, out, n);
    for (int i = 0; i < n; i++) if (verify[i] != w[i]) {
        memset(out, 0, 128);
        encode_raw(out, w, n);
        return 1 + n;
    }
    return best_sz;
}

/* ---- corrected GGUF harness: stride 34 + store scale ---- */
int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <model.gguf>\n", argv[0]); return 1; }
    const char *path = argv[1];
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    uint32_t magic; fread(&magic, 4, 1, f);
    if (magic != 0x46554747) { fprintf(stderr, "Not GGUF\n"); return 1; }
    uint32_t ver; fread(&ver, 4, 1, f);
    uint64_t nt, nk; fread(&nt, 8, 1, f); fread(&nk, 8, 1, f);
    for (uint64_t i = 0; i < nk; i++) {
        uint64_t kl; fread(&kl, 8, 1, f); fseek(f, kl, SEEK_CUR);
        uint32_t vt; fread(&vt, 4, 1, f);
        switch (vt) {
            case 0: case 1: case 7: fseek(f,1,SEEK_CUR); break;
            case 2: case 3: fseek(f,2,SEEK_CUR); break;
            case 4: case 5: case 6: fseek(f,4,SEEK_CUR); break;
            case 8: { uint64_t l; fread(&l,8,1,f); fseek(f,l,SEEK_CUR); break; }
            case 9: { uint32_t et; fread(&et,4,1,f); uint64_t al; fread(&al,8,1,f);
                      for (uint64_t j=0;j<al;j++){ if(et==8){uint64_t l2;fread(&l2,8,1,f);fseek(f,l2,SEEK_CUR);}
                      else fseek(f,(et<=1?1:et<=3?2:et<=6?4:et==7?1:8),SEEK_CUR);} break; }
            case 10: case 11: case 12: fseek(f,8,SEEK_CUR); break;
            default: fclose(f); return 1;
        }
    }
    for (uint64_t i = 0; i < nt; i++) {
        uint64_t nl; fread(&nl, 8, 1, f); fseek(f, nl, SEEK_CUR);
        uint32_t nd; fread(&nd, 4, 1, f);
        uint64_t nw = 1;
        for (uint32_t d = 0; d < nd && d < 4; d++) { uint64_t dm; fread(&dm,8,1,f); nw *= dm; }
        uint32_t dt; fread(&dt, 4, 1, f);
        uint64_t off; fread(&off, 8, 1, f);
        if (dt == 8) { /* Q8_0 block = 34 bytes: 2B scale + 32 weights */
            printf("Tensor: Q8_0, %llu weights\n", (unsigned long long)nw);
            long ds = ftell(f);
            int nb = (int)(nw / 32);
            int nt2 = nb > 4000 ? 4000 : nb;
            uint8_t *raw = malloc((size_t)nt2 * 34);
            fseek(f, ds, SEEK_SET);
            fread(raw, 1, (size_t)nt2 * 34, f);
            uint64_t w_total=0, s_total=0;
            int lossless = 1, exact = 0;
            int method_count[4]={0}; int method_bytes[4]={0};
            for (int b = 0; b < nt2; b++) {
                uint8_t *blk = raw + b * 34;          /* stride 34 */
                int16_t scale16; memcpy(&scale16, blk, 2);
                int8_t w8[32];
                for (int j = 0; j < 32; j++) w8[j] = (int8_t)blk[2 + j];
                int8_t wzero[32]; memcpy(wzero, w8, 32);
                uint8_t buf2[128]; memset(buf2,0,sizeof(buf2));
                int sz = encode_best(buf2, w8, 32);
                int8_t dec[32]; memset(dec,0,sizeof(dec));
                decode_best(dec, buf2, 32);
                for (int j = 0; j < 32; j++) if (dec[j] != wzero[j]) lossless = 0;
                for (int j = 0; j < 32; j++) if (dec[j] == wzero[j]) exact++;
                int method = -1;
                switch (buf2[0]) { case 0x0A:method=0;break; case 0x0B:method=1;break;
                                   case 0x0C:method=2;break; case 0x0D:method=3;break; }
                if (method>=0){ method_count[method]++; method_bytes[method]+=sz; }
                w_total  += sz;              /* codec bytes for weights */
                full_total+= sz;             /* will add scale below */
                s_total  += 2;               /* scale = 2 raw bytes stored as-is */
            }
            double avg_w = (double)w_total / nt2;
            double avg_full = (double)(w_total + s_total) / nt2;   /* weights+scale */
            printf("Blocks: %d\n", nt2);
            printf("Codec weights only: %.2f B/block\n", avg_w);
            printf("CLUSTER full (weights+scale 2B): %.2f B/block\n", avg_full);
            printf("Lossless (weights roundtrip): %s\n", lossless ? "YES" : "NO");
            printf("Exact: %d/%d\n", exact, nt2*32);
            printf("vs Q8_0 (34 B): %.4fx\n", avg_full / 34.0);
            printf("vs Raw weights-only (32 B): %.4fx\n", avg_w / 32.0);
            printf("Method usage:\n");
            const char *names[] = {"A:raw","B:bitmap","C:grp4","D:adapt"};
            for (int m=0;m<4;m++) if (method_count[m]>0)
                printf("  %s: %d blocks, avg %.1f B\n", names[m], method_count[m], (double)method_bytes[m]/method_count[m]);
            free(raw);
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    printf("No Q8_0 tensor found\n");
    return 1;
}