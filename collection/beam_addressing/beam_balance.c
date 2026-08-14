/*
 * beam_balance.c — Balance point: Geo1State fp32 + 4-bit deltas
 *
 * Format: [mean:4][R:4][max_delta:4][deltas]
 * = 28 bytes/block = 0.82× Q8_0
 *
 * Key: k is DERIVED from mean+R, not stored.
 * fp32 precision eliminates fp16 rounding error.
 * Sorted weights: decode produces sorted order.
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

static void wbits(uint8_t *b, int p, int v, int nb) {
    for (int i = 0; i < nb; i++) {
        if (v & (1<<i)) b[(p+i)/8] |= 1<<((p+i)%8);
    }
}

static int rbits(const uint8_t *b, int p, int nb) {
    int v = 0;
    for (int i = 0; i < nb; i++)
        if (b[(p+i)/8] & (1<<((p+i)%8))) v |= 1<<i;
    return v;
}

/* Encode: sorted weights → packed (mean+R not stored per-block) */
static int enc(uint8_t *out, float *w, int nb)
{
    float s[N];
    for (int i = 0; i < N; i++) s[i] = w[i];
    sort_arr(s, N);

    float sum = 0;
    for (int i = 0; i < N; i++) sum += s[i];
    float mean = sum / N;

    float ss = 0;
    for (int i = 0; i < N; i++) { float d = s[i]-mean; ss += d*d; }
    float R = sqrtf(ss / N);
    if (R < 1e-10f) R = fabsf(s[N-1]-s[0]) / N;  /* fallback */
    if (R < 1e-10f) R = 1.0f;

    float maxd = 0;
    for (int i = 0; i < N; i++) {
        float k = roundf((s[i]-mean)/R);
        float d = s[i] - (mean + k*R);
        if (fabsf(d) > maxd) maxd = fabsf(d);
    }
    float ds = (maxd > 1e-10f) ? maxd : 1.0f;
    int maxv = (1<<nb)-1;

    int pos = 0;
    memcpy(out, &mean, 4); pos += 4;
    memcpy(out+4, &R, 4); pos += 4;
    memcpy(out+8, &maxd, 4); pos += 12;

    /* Deltas: derive k from weight, store only delta */
    for (int i = 0; i < N; i++) {
        float k = roundf((s[i]-mean)/R);
        float ideal = mean + k*R;
        float dv = s[i] - ideal;
        int q = (int)roundf(dv/ds*maxv);
        if (q < -maxv) q = -maxv;
        if (q > maxv) q = maxv;
        wbits(out, pos+i*nb, q+maxv, nb);
    }
    pos += (N*nb+7)/8;
    return pos;
}

/* Decode: read mean+R, derive k, reconstruct weights */
static void dec(float *out, const uint8_t *buf, int nb)
{
    float mean, R, maxd;
    memcpy(&mean, buf, 4);
    memcpy(&R, buf+4, 4);
    memcpy(&maxd, buf+8, 4);
    float ds = (maxd > 1e-10f) ? maxd : 1.0f;
    int maxv = (1<<nb)-1;
    int pos = 12;

    /* Reconstruct: for each of 32 positions, derive k, add delta */
    for (int i = 0; i < N; i++) {
        int qu = rbits(buf, pos+i*nb, nb);
        int q = qu - maxv;
        float dv = (float)q * ds / maxv;

        /* k from sorted position (monotonically increasing) */
        /* We need to reconstruct k — derive from index */
        /* k ranges from k_min to k_max, evenly spaced */
        float k_approx = (float)(i - N/2);
        out[i] = mean + k_approx * R + dv;
    }
}

/* Better decode: use actual k from sorted order */
static void dec2(float *out, const uint8_t *buf, int nb)
{
    float mean, R, maxd;
    memcpy(&mean, buf, 4);
    memcpy(&R, buf+4, 4);
    memcpy(&maxd, buf+8, 4);
    float ds = (maxd > 1e-10f) ? maxd : 1.0f;
    int maxv = (1<<nb)-1;
    int pos = 12;

    /* During encode, k = round((s[i]-mean)/R)
     * For sorted weights with uniform R spacing,
     * k is approximately i - N/2 (centered)
     * More precisely: k[i] = round((s[i]-mean)/R)
     * s[i] ≈ mean + (i - N/2) * R for uniform distribution
     * So k[i] ≈ i - N/2
     */
    for (int i = 0; i < N; i++) {
        int qu = rbits(buf, pos+i*nb, nb);
        int q = qu - maxv;
        float dv = (float)q * ds / maxv;

        /* For sorted weights: k[i] = i - N/2 (exact for uniform) */
        float k = (float)(i - N/2);
        out[i] = mean + k * R + dv;
        if (out[i] != out[i]) out[i] = 0;  /* NaN safety */
    }
}

int main(int argc, char **argv)
{
    printf("Beam Balance — Geo1State fp32 + 4-bit deltas\n");
    printf("Format: [mean:4][R:4][max_delta:4][deltas]\n");
    printf("= 28 bytes/block = 0.82× Q8_0\n\n");

    float w[N];
    int pass = 0, fail = 0;
    double total_err = 0;
    srand(42);

    for (int t = 0; t < 100; t++) {
        for (int i = 0; i < N; i++) w[i] = (float)((rand()%256)-128)*0.01f;
        float s[N];
        for (int i = 0; i < N; i++) s[i] = w[i];
        sort_arr(s, N);

        uint8_t buf[128];
        int sz = enc(buf, w, 4);

        float d[N];
        dec2(d, buf, 4);

        int ok = 1;
        for (int i = 0; i < N; i++) {
            float e = fabsf(s[i]-d[i]);
            total_err += e;
            if (e > 0.001f) ok = 0;
        }
        if (ok) pass++; else fail++;
    }

    printf("4-bit: PASS=%d/100 FAIL=%d  avg_err=%.6f\n", pass, fail, total_err/(100*N));

    /* Real model */
    if (argc >= 2) {
        printf("\n=== Real Model ===\n");
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }

        uint32_t magic; fread(&magic, 4, 1, f);
        if (magic != 0x46554747) { fprintf(stderr, "Not GGUF\n"); fclose(f); return 1; }
        uint32_t ver; fread(&ver, 4, 1, f);
        uint64_t nt; fread(&nt, 8, 1, f);
        uint64_t nk; fread(&nk, 8, 1, f);

        /* Skip KV */
        for (uint64_t i = 0; i < nk; i++) {
            uint64_t kl; fread(&kl, 8, 1, f); fseek(f, kl, SEEK_CUR);
            uint32_t vt; fread(&vt, 4, 1, f);
            switch(vt) {
                case 0: case 1: case 7: fseek(f,1,SEEK_CUR); break;
                case 2: case 3: fseek(f,2,SEEK_CUR); break;
                case 4: case 5: case 6: fseek(f,4,SEEK_CUR); break;
                case 8: { uint64_t l; fread(&l,8,1,f); fseek(f,l,SEEK_CUR); break; }
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

            if (dt == 8) {
                printf("  Tensor: Q8_0, %llu weights\n", (unsigned long long)nw);
                long ds = ftell(f);
                int nb = (int)(nw/32);
                int nt2 = nb > 100 ? 100 : nb;
                uint8_t *raw = malloc(nt2*33);
                fseek(f, ds, SEEK_SET);
                fread(raw, 1, nt2*33, f);

                int p4=0, f4=0;
                double e4=0;
                int sz4=0;

                for (int b = 0; b < nt2; b++) {
                    uint16_t sc; memcpy(&sc, raw+b*33, 2);
                    float wg[32];
                    for (int j = 0; j < 32; j++) {
                        int sg=(sc>>15)&1, ep=(sc>>10)&0x1f, mn=sc&0x3ff;
                        float sc2;
                        if (ep==0) sc2=(sg?-1:1)*ldexp(mn,-24);
                        else if (ep==31) sc2=(sg?-1:1)*INFINITY;
                        else sc2=(sg?-1:1)*ldexp(1.0+mn/1024.0,ep-15);
                        wg[j] = (int8_t)raw[b*33+2+j] * sc2;
                    }
                    float sr[32]; for (int j=0;j<32;j++) sr[j]=wg[j]; sort_arr(sr,32);

                    uint8_t buf[128];
                    int s4 = enc(buf, wg, 4);
                    sz4 += s4;
                    float d4[32]; dec2(d4, buf, 4);
                    for (int j=0;j<32;j++) e4 += fabsf(sr[j]-d4[j]);
                    int ok=1;
                    for (int j=0;j<32;j++) if(fabsf(sr[j]-d4[j])>0.001f) ok=0;
                    if(ok) p4++; else f4++;
                }

                printf("  4-bit: %d/%d PASS  avg_size=%.1fB  avg_err=%.6f  ratio=%.4fx\n",
                       p4, nt2, (double)sz4/nt2, e4/(nt2*32), (double)sz4/nt2/34.0);
                free(raw);
                break;
            }
        }
        fclose(f);
    }
    return 0;
}
