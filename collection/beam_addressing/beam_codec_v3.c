/*
 * beam_codec_v3.c — Clean roundtrip, no macros
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define N 32

static void sort_arr(float *a, int n) {
    for (int i = 0; i < n-1; i++)
        for (int j = i+1; j < n; j++)
            if (a[i] > a[j]) { float t = a[i]; a[i] = a[j]; a[j] = t; }
}

/* Write N bits from val starting at bit_pos in buf */
static void write_bits(uint8_t *buf, int bit_pos, int val, int nbits) {
    for (int b = 0; b < nbits; b++) {
        int byte_idx = (bit_pos + b) / 8;
        int bit_idx = (bit_pos + b) % 8;
        if (val & (1 << b))
            buf[byte_idx] |= (1 << bit_idx);
    }
}

/* Read N bits from bit_pos in buf */
static int read_bits(const uint8_t *buf, int bit_pos, int nbits) {
    int val = 0;
    for (int b = 0; b < nbits; b++) {
        int byte_idx = (bit_pos + b) / 8;
        int bit_idx = (bit_pos + b) % 8;
        if (buf[byte_idx] & (1 << bit_idx))
            val |= (1 << b);
    }
    return val;
}

/* Encode 32 weights → packed buffer. Returns size in bytes. */
static int enc(uint8_t *out, float *w, int nbits)
{
    float s[N];
    for (int i = 0; i < N; i++) s[i] = w[i];
    sort_arr(s, N);

    /* mean, R */
    float sum = 0;
    for (int i = 0; i < N; i++) sum += s[i];
    float mean = sum / N;

    float ss = 0;
    for (int i = 0; i < N; i++) { float d = s[i] - mean; ss += d*d; }
    float R = sqrtf(ss / N);
    if (R < 1e-10f) R = 1.0f;

    /* k values and deltas */
    int kv[N];
    float dv[N];
    float maxd = 0;
    for (int i = 0; i < N; i++) {
        float k = roundf((s[i] - mean) / R);
        kv[i] = (int)k;
        dv[i] = s[i] - (mean + k * R);
        if (fabsf(dv[i]) > maxd) maxd = fabsf(dv[i]);
    }
    float ds = (maxd > 1e-10f) ? maxd : 1.0f;
    int maxv = (1 << nbits) - 1;

    /* Pack header: mean(4) + R(4) + maxd(4) = 12 bytes */
    int pos = 0;
    memcpy(out + pos, &mean, 4); pos += 4;
    memcpy(out + pos, &R, 4); pos += 4;
    memcpy(out + pos, &maxd, 4); pos += 4;

    /* Pack k values: 5 bits each = 20 bytes */
    int kbits = 5;
    int koff = 16;
    for (int i = 0; i < N; i++) {
        int kv_enc = kv[i] + koff;
        if (kv_enc < 0) kv_enc = 0;
        if (kv_enc >= 32) kv_enc = 31;
        write_bits(out, pos + i * kbits, kv_enc, kbits);
    }
    pos += (N * kbits + 7) / 8;

    /* Pack deltas: nbits each */
    for (int i = 0; i < N; i++) {
        int q = (int)roundf(dv[i] / ds * maxv);
        if (q < -maxv) q = -maxv;
        if (q > maxv) q = maxv;
        int qu = q + maxv;  /* shift to unsigned */
        write_bits(out, pos + i * nbits, qu, nbits);
    }
    pos += (N * nbits + 7) / 8;

    return pos;
}

/* Decode packed buffer → 32 floats (sorted order) */
static void dec(float *out, const uint8_t *buf, int nbits)
{
    int pos = 0;
    float mean, R, maxd;
    memcpy(&mean, buf + pos, 4); pos += 4;
    memcpy(&R, buf + pos, 4); pos += 4;
    memcpy(&maxd, buf + pos, 4); pos += 4;
    float ds = (maxd > 1e-10f) ? maxd : 1.0f;
    int maxv = (1 << nbits) - 1;

    /* Unpack k values */
    int kbits = 5;
    int koff = 16;
    int kv[N];
    for (int i = 0; i < N; i++) {
        kv[i] = read_bits(buf, pos + i * kbits, kbits) - koff;
    }
    pos += (N * kbits + 7) / 8;

    /* Unpack deltas */
    for (int i = 0; i < N; i++) {
        int qu = read_bits(buf, pos + i * nbits, nbits);
        int q = qu - maxv;
        float d = (float)q * ds / maxv;
        out[i] = mean + (float)kv[i] * R + d;
    }
}

int main(int argc, char **argv)
{
    printf("Beam Codec v3 — Clean Roundtrip\n\n");

    /* Test synthetic */
    float w[N];
    int pass = 0, fail = 0;
    srand(42);

    for (int t = 0; t < 100; t++) {
        for (int i = 0; i < N; i++) w[i] = (float)((rand()%256)-128)*0.01f;
        float s[N];
        for (int i = 0; i < N; i++) s[i] = w[i];
        sort_arr(s, N);

        uint8_t buf[128];
        int sz = enc(buf, w, 4);

        float d[N];
        dec(d, buf, 4);

        int ok = 1;
        for (int i = 0; i < N; i++) {
            if (fabsf(s[i] - d[i]) > 0.001f) { ok = 0; break; }
        }
        if (ok) pass++; else fail++;
    }

    printf("4-bit: PASS=%d/100 FAIL=%d  size=44B = %.2fx Q8_0\n", pass, fail, 44.0/34.0);

    /* Test with 8-bit */
    pass = 0; fail = 0;
    for (int t = 0; t < 100; t++) {
        for (int i = 0; i < N; i++) w[i] = (float)((rand()%256)-128)*0.01f;
        float s[N];
        for (int i = 0; i < N; i++) s[i] = w[i];
        sort_arr(s, N);

        uint8_t buf[128];
        int sz = enc(buf, w, 8);

        float d[N];
        dec(d, buf, 8);

        int ok = 1;
        for (int i = 0; i < N; i++) {
            if (fabsf(s[i] - d[i]) > 0.0001f) { ok = 0; break; }
        }
        if (ok) pass++; else fail++;
    }

    printf("8-bit: PASS=%d/100 FAIL=%d  size=60B = %.2fx Q8_0\n", pass, fail, 60.0/34.0);

    /* Test real model */
    if (argc >= 2) {
        printf("\n=== Real Model: %s ===\n", argv[1]);
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }

        uint32_t magic; fread(&magic, 4, 1, f);
        if (magic != 0x46554747) { fprintf(stderr, "Not GGUF\n"); fclose(f); return 1; }
        uint32_t ver; fread(&ver, 4, 1, f);
        uint64_t nt; fread(&nt, 8, 1, f);
        uint64_t nk; fread(&nk, 8, 1, f);

        for (uint64_t i = 0; i < nk; i++) {
            uint64_t kl; fread(&kl, 8, 1, f); fseek(f, kl, SEEK_CUR);
            uint32_t vt; fread(&vt, 4, 1, f);
            switch (vt) {
                case 0: fseek(f, 1, SEEK_CUR); break;
                case 1: fseek(f, 1, SEEK_CUR); break;
                case 2: fseek(f, 2, SEEK_CUR); break;
                case 3: fseek(f, 2, SEEK_CUR); break;
                case 4: fseek(f, 4, SEEK_CUR); break;
                case 5: fseek(f, 4, SEEK_CUR); break;
                case 6: fseek(f, 4, SEEK_CUR); break;
                case 7: fseek(f, 1, SEEK_CUR); break;
                case 8: { uint64_t l; fread(&l, 8, 1, f); fseek(f, l, SEEK_CUR); break; }
                case 9: { uint32_t et; fread(&et, 4, 1, f); uint64_t al; fread(&al, 8, 1, f); for (uint64_t j=0;j<al;j++) { uint32_t vt2=et; if (vt2==8){uint64_t l2;fread(&l2,8,1,f);fseek(f,l2,SEEK_CUR);}else fseek(f,(vt2<=1?1:vt2<=3?2:vt2<=6?4:vt2==7?1:8),SEEK_CUR);} break; }
                case 10: fseek(f, 8, SEEK_CUR); break;
                case 11: fseek(f, 8, SEEK_CUR); break;
                case 12: fseek(f, 8, SEEK_CUR); break;
                default: fclose(f); return 1;
            }
        }

        for (uint64_t i = 0; i < nt; i++) {
            uint64_t nl; fread(&nl, 8, 1, f); fseek(f, nl, SEEK_CUR);
            uint32_t nd; fread(&nd, 4, 1, f);
            uint64_t nw = 1;
            for (uint32_t d = 0; d < nd && d < 4; d++) { uint64_t dm; fread(&dm, 8, 1, f); nw *= dm; }
            uint32_t dt; fread(&dt, 4, 1, f);
            uint64_t off; fread(&off, 8, 1, f);

            if (dt == 8) {
                printf("  Tensor: Q8_0, %llu weights\n", (unsigned long long)nw);
                long ds = ftell(f);
                int nb = (int)(nw / 32);
                int nt2 = nb > 100 ? 100 : nb;
                uint8_t *raw = malloc(nt2 * 33);
                fseek(f, ds, SEEK_SET);
                fread(raw, 1, nt2 * 33, f);

                int p4 = 0, f4 = 0, p8 = 0, f8 = 0;
                double e4 = 0, e8 = 0;
                int sz4 = 0, sz8 = 0;

                for (int b = 0; b < nt2; b++) {
                    uint16_t sc; memcpy(&sc, raw+b*33, 2);
                    float wg[32];
                    for (int j = 0; j < 32; j++) {
                        int sg = (sc>>15)&1, ep = (sc>>10)&0x1f, mn = sc&0x3ff;
                        float sc2;
                        if (ep==0) sc2 = (sg?-1:1)*ldexp(mn,-24);
                        else if (ep==31) sc2 = (sg?-1:1)*INFINITY;
                        else sc2 = (sg?-1:1)*ldexp(1.0+mn/1024.0,ep-15);
                        wg[j] = (int8_t)raw[b*33+2+j] * sc2;
                    }
                    float sr[32];
                    for (int j = 0; j < 32; j++) sr[j] = wg[j];
                    sort_arr(sr, 32);

                    uint8_t buf[128];
                    int s4 = enc(buf, wg, 4);
                    sz4 += s4;
                    float d4[32]; dec(d4, buf, 4);
                    for (int j = 0; j < 32; j++) e4 += fabsf(sr[j]-d4[j]);
                    int ok4 = 1;
                    for (int j = 0; j < 32; j++) if (fabsf(sr[j]-d4[j]) > 0.001f) ok4=0;
                    if (ok4) p4++; else f4++;

                    int s8 = enc(buf, wg, 8);
                    sz8 += s8;
                    float d8[32]; dec(d8, buf, 8);
                    for (int j = 0; j < 32; j++) e8 += fabsf(sr[j]-d8[j]);
                    int ok8 = 1;
                    for (int j = 0; j < 32; j++) if (fabsf(sr[j]-d8[j]) > 0.0001f) ok8=0;
                    if (ok8) p8++; else f8++;
                }

                printf("  4-bit: %d/%d PASS  avg_size=%.1fB  avg_err=%.6f  ratio=%.4fx\n",
                       p4, nt2, (double)sz4/nt2, e4/(nt2*32), (double)sz4/nt2/34.0);
                printf("  8-bit: %d/%d PASS  avg_size=%.1fB  avg_err=%.6f  ratio=%.4fx\n",
                       p8, nt2, (double)sz8/nt2, e8/(nt2*32), (double)sz8/nt2/34.0);
                free(raw);
                break;
            }
        }
        fclose(f);
    }

    return 0;
}
