/*
 * beam_lossless.c — Lossless Geometric Codec
 *
 * Key insight: Q8_0 block = 32 int8 weights + 1 fp16 scale
 * Weights are int8 ∈ [-128, 127] — only 256 possible values
 *
 * Approach: Grid centroids + per-weight index + small delta
 * For weights that cluster well, index is small (2-3 bits)
 *
 * Format: [mean:2][R:2][max_delta:2][k_indices][deltas]
 * If k_indices average 3 bits: 6 + 32×3/8 + 32×3/8 ≈ 30B
 * = 0.88× Q8_0 (lossless!)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define N 32

static void wbits(uint8_t *b, int p, int v, int nb) {
    for (int i = 0; i < nb; i++)
        if (v & (1<<i)) b[(p+i)/8] |= 1<<((p+i)%8);
}

static int rbits(const uint8_t *b, int p, int nb) {
    int v = 0;
    for (int i = 0; i < nb; i++)
        if (b[(p+i)/8] & (1<<((p+i)%8))) v |= 1<<i;
    return v;
}

/* Float → fp16 */
static uint16_t f2h(float f) {
    uint32_t u; memcpy(&u, &f, 4);
    uint32_t s = (u>>16)&0x8000;
    int32_t e = ((u>>23)&0xFF) - 127 + 15;
    uint32_t m = (u>>13)&0x3FF;
    if (e<=0) return (uint16_t)s;
    if (e>=31) return (uint16_t)(s|0x7C00);
    return (uint16_t)(s|(e<<10)|m);
}

/* fp16 → float */
static float h2f(uint16_t h) {
    uint32_t s=(h>>15)&1, e=(h>>10)&0x1F, m=h&0x3FF;
    uint32_t f;
    if (e==0) f=(s<<31)|(m<<13);
    else if(e==31) f=(s<<31)|0x7F800000|(m<<13);
    else { e+=127-15; f=(s<<31)|(e<<23)|(m<<13); }
    float v; memcpy(&v,&f,4); return v;
}

/*
 * Lossless encode: 32 int8 weights → packed buffer
 *
 * For each weight w[i]:
 *   k[i] = round((w[i] - mean) / R)  — which grid point
 *   d[i] = w[i] - (mean + k[i]*R)    — residual (small)
 *
 * Storage:
 *   Header: mean(fp16=2B) + R(fp16=2B) + max_delta(fp16=2B) = 6B
 *   k_indices: N × k_bits bits
 *   deltas: N × delta_bits bits
 *
 * For lossless: delta_bits must represent exact residual
 * For int8 weights: max residual < R, so delta_bits = ceil(log2(2*R+1))
 */
static int enc_lossless(uint8_t *out, int8_t *weights, int n, int k_bits, int d_bits)
{
    /* Convert to float for grid computation */
    float w[N];
    for (int i = 0; i < n; i++) w[i] = (float)weights[i];

    /* Compute mean */
    float sum = 0;
    for (int i = 0; i < n; i++) sum += w[i];
    float mean = sum / n;

    /* Compute R (std) */
    float ss = 0;
    for (int i = 0; i < n; i++) { float d = w[i]-mean; ss += d*d; }
    float R = sqrtf(ss / n);
    if (R < 1e-10f) R = 1.0f;

    /* Compute k and delta for each weight */
    int kv[N];
    int dv[N];
    int max_dv = 0;
    for (int i = 0; i < n; i++) {
        float k = roundf((w[i]-mean)/R);
        kv[i] = (int)k;
        int d = (int)w[i] - (int)(mean + k*R);  /* exact integer residual */
        dv[i] = d;
        if (abs(d) > max_dv) max_dv = abs(d);
    }

    /* Determine actual bits needed */
    int actual_d_bits = 1;
    while ((1 << actual_d_bits) <= 2 * max_dv) actual_d_bits++;
    if (actual_d_bits > d_bits) actual_d_bits = d_bits;

    /* Pack header */
    int pos = 0;
    uint16_t mh = f2h(mean), rh = f2h(R);
    memcpy(out, &mh, 2); pos += 2;
    memcpy(out+2, &rh, 2); pos += 2;
    /* Store actual_d_bits in header */
    out[pos] = (uint8_t)actual_d_bits;
    pos += 1;

    /* Pack k values */
    for (int i = 0; i < n; i++) {
        int kv_enc = kv[i] + (1 << (k_bits-1));  /* shift to unsigned */
        if (kv_enc < 0) kv_enc = 0;
        if (kv_enc >= (1<<k_bits)) kv_enc = (1<<k_bits)-1;
        wbits(out, pos + i*k_bits, kv_enc, k_bits);
    }
    pos += (n*k_bits+7)/8;

    /* Pack deltas */
    for (int i = 0; i < n; i++) {
        int dv_enc = dv[i] + (1 << (actual_d_bits-1));
        wbits(out, pos + i*actual_d_bits, dv_enc, actual_d_bits);
    }
    pos += (n*actual_d_bits+7)/8;

    return pos;
}

/* Lossless decode */
static void dec_lossless(int8_t *out, const uint8_t *buf, int n, int k_bits)
{
    int pos = 0;
    uint16_t mh, rh;
    memcpy(&mh, buf, 2); pos += 2;
    memcpy(&rh, buf+2, 2); pos += 2;
    float mean = h2f(mh), R = h2f(rh);
    int d_bits = buf[pos]; pos += 1;

    /* Unpack k values */
    int kv[N];
    for (int i = 0; i < n; i++) {
        kv[i] = rbits(buf, pos+i*k_bits, k_bits) - (1<<(k_bits-1));
    }
    pos += (n*k_bits+7)/8;

    /* Unpack deltas */
    for (int i = 0; i < n; i++) {
        int dv = rbits(buf, pos+i*d_bits, d_bits) - (1<<(d_bits-1));
        float k = (float)kv[i];
        int reconstructed = (int)roundf(mean + k*R) + dv;
        /* Clamp to int8 */
        if (reconstructed < -128) reconstructed = -128;
        if (reconstructed > 127) reconstructed = 127;
        out[i] = (int8_t)reconstructed;
    }
}

int main(int argc, char **argv)
{
    printf("Beam Lossless — Geometric Grid + Exact Delta\n\n");

    /* Test with real Q8_0 model */
    if (argc < 2) {
        printf("Usage: %s <model.gguf>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }

    /* Read GGUF */
    uint32_t magic; fread(&magic, 4, 1, f);
    if (magic != 0x46554747) { fprintf(stderr, "Not GGUF\n"); fclose(f); return 1; }
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
            case 9: { uint32_t et; fread(&et,4,1,f); uint64_t al; fread(&al,8,1,f); for(uint64_t j=0;j<al;j++){if(et==8){uint64_t l2;fread(&l2,8,1,f);fseek(f,l2,SEEK_CUR);}else fseek(f,(et<=1?1:et<=3?2:et<=6?4:et==7?1:8),SEEK_CUR);} break; }
            case 10: case 11: case 12: fseek(f,8,SEEK_CUR); break;
            default: fprintf(stderr, "Unknown KV type %u\n", vt); fclose(f); return 1;
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
            printf("Tensor: Q8_0, %llu weights\n", (unsigned long long)nw);
            long ds = ftell(f);
            int nb = (int)(nw/32);
            int nt2 = nb > 200 ? 200 : nb;
            uint8_t *raw = malloc(nt2*33);
            fseek(f, ds, SEEK_SET);
            fread(raw, 1, nt2*33, f);

            /* Test different k_bits and d_bits */
            printf("\n  k_bits  d_bits  avg_size  lossless?  ratio\n");
            printf("  ------  ------  --------  ---------  -----\n");

            for (int kb = 3; kb <= 5; kb++) {
                for (int db = 3; db <= 6; db++) {
                    int total_sz = 0;
                    int lossless = 1;

                    for (int b = 0; b < nt2; b++) {
                        /* Dequantize Q8_0 → int8 */
                        uint16_t sc; memcpy(&sc, raw+b*33, 2);
                        int8_t w8[32];
                        for (int j = 0; j < 32; j++) {
                            w8[j] = (int8_t)raw[b*33+2+j];
                        }

                        uint8_t buf[128];
                        int sz = enc_lossless(buf, w8, 32, kb, db);
                        total_sz += sz;

                        /* Verify lossless */
                        int8_t dec[32];
                        dec_lossless(dec, buf, 32, kb);
                        for (int j = 0; j < 32; j++) {
                            if (dec[j] != w8[j]) { lossless = 0; break; }
                        }
                    }

                    double avg = (double)total_sz / nt2;
                    printf("  %5d   %5d   %6.1f    %s      %.4fx\n",
                           kb, db, avg,
                           lossless ? "YES ✓" : "NO",
                           avg / 34.0);
                }
            }

            free(raw);
            break;
        }
    }
    fclose(f);
    return 0;
}
