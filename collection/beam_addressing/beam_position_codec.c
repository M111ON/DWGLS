/*
 * beam_position_codec.c — Weight = compute(position, time)
 *
 * Core principle: "Coordinate IS data"
 *   weight = f(frame_seek_position, fibo_tick_time)
 *
 * No compression. No lossy approximation.
 * Weight is COMPUTED from geometric structure + time.
 *
 * Storage: position (enc) + time per block
 * Decoder: compute weight from position + time
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ══════════════════════════════════════════════════════════════
   FRAME SEEK — 1440 positions, stride-37
   ══════════════════════════════════════════════════════════════ */

#define FRAME_CYCLE 1440u
#define FRAME_STRIDE 37u

/* enc(t) = (t × 37) % 1440 */
static inline uint16_t frame_enc(uint16_t t) {
    return (uint16_t)((t * FRAME_STRIDE) % FRAME_CYCLE);
}

/* seek(enc) → t — inverse: 37^{-1} mod 1440 = 973 */
static inline uint16_t frame_seek(uint16_t enc) {
    return (uint16_t)((enc * 973u) % FRAME_CYCLE);
}

/* ══════════════════════════════════════════════════════════════
   WEIGHT → POSITION MAPPING
   ══════════════════════════════════════════════════════════════
 *
 * Weight (int8, -128..127) → position on 1440 timeline
 *
 * Mapping: weight_to_pos(w) = frame_enc(w + 128)
 *   w=-128 → pos=0
 *   w=0    → pos=frame_enc(128) = (128×37)%1440 = 4736%1440 = 416
 *   w=127  → pos=frame_enc(255) = (255×37)%1440 = 9435%1440 = 9435-6×1440=9435-8640=795
 *
 * Inverse: pos_to_weight(pos) = frame_seek(pos) - 128
 *   This is the COMPUTE step — no lookup table needed!
 */

static inline uint16_t weight_to_pos(int8_t weight) {
    return frame_enc((uint16_t)(weight + 128));
}

static inline int8_t pos_to_weight(uint16_t pos) {
    return (int8_t)(frame_seek(pos) - 128);
}

/* ══════════════════════════════════════════════════════════════
   BIT PACKING
   ══════════════════════════════════════════════════════════════ */

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

/* ══════════════════════════════════════════════════════════════
   FIBO TICK — time dimension
   ══════════════════════════════════════════════════════════════
 *
 * fibo_tick provides temporal context:
 *   - tick (0..11): which of 12 ticks in current cycle
 *   - pipe (0..1727): which of 1728 pipes
 *   - The tick determines the "phase" of the geometric structure
 *
 * For weight computation:
 *   weight = pos_to_weight(pos) + tick_offset(tick)
 *   where tick_offset encodes how the geometric structure
 *   evolves over time
 *
 * For now: tick_offset = 0 (simplest case)
 * The structure is deterministic — same pos + same tick = same weight
 */

/* ══════════════════════════════════════════════════════════════
   ENCODE: 32 int8 weights → positions + time
   ══════════════════════════════════════════════════════════════ */

static int encode_block(uint8_t *out, const int8_t *weights, int n, uint16_t tick)
{
    int pos = 0;

    /* Header: n (8 bits) + tick (16 bits) */
    out[pos++] = (uint8_t)n;
    out[pos++] = (uint8_t)(tick & 0xFF);
    out[pos++] = (uint8_t)((tick >> 8) & 0xFF);

    /* Positions: each weight → enc on 1440 timeline */
    /* 1440 positions needs 11 bits (2^11 = 2048 > 1440) */
    for (int i = 0; i < n; i++) {
        uint16_t p = weight_to_pos(weights[i]);
        wbits(out, pos, p, 11);
        pos += 11;
    }

    return (pos + 7) / 8;  /* bytes */
}

/* ══════════════════════════════════════════════════════════════
   DECODE: positions + time → weights (COMPUTE, not decompress)
   ══════════════════════════════════════════════════════════════ */

static void decode_block(int8_t *out, const uint8_t *buf, int max_n)
{
    int pos = 0;
    int n = buf[pos++];
    uint16_t tick = buf[pos] | ((uint16_t)buf[pos+1] << 8);
    pos += 2;

    if (n > max_n) n = max_n;

    /* Compute weights from positions + tick */
    for (int i = 0; i < n; i++) {
        uint16_t p = (uint16_t)rbits(buf, pos, 11);
        pos += 11;

        /* THE COMPUTE STEP: weight = f(position, time) */
        out[i] = pos_to_weight(p);
        /* Future: out[i] = pos_to_weight(p) + tick_offset(tick); */
    }
}

/* ══════════════════════════════════════════════════════════════
   TEST
   ══════════════════════════════════════════════════════════════ */

static void test_basic(void)
{
    printf("=== Basic Roundtrip Test ===\n");

    int pass = 0, fail = 0;

    for (int w = -128; w <= 127; w++) {
        uint16_t pos = weight_to_pos((int8_t)w);
        int8_t decoded = pos_to_weight(pos);

        if (decoded == (int8_t)w) pass++;
        else {
            fail++;
            if (fail <= 5) printf("  FAIL: w=%d pos=%u decoded=%d\n", w, pos, decoded);
        }
    }

    printf("  PASS: %d/256  FAIL: %d/256\n\n", pass, fail);
}

static void test_block(void)
{
    printf("=== Block Encode/Decode Test ===\n");

    int8_t weights[32];
    srand(42);
    for (int i = 0; i < 32; i++) weights[i] = (int8_t)(rand() % 256 - 128);

    uint8_t buf[128];
    int sz = encode_block(buf, weights, 32, 0);

    int8_t decoded[32];
    decode_block(decoded, buf, 32);

    int pass = 0;
    for (int i = 0; i < 32; i++) {
        if (decoded[i] == weights[i]) pass++;
    }

    printf("  Block size: %d bytes\n", sz);
    printf("  PASS: %d/32\n", pass);
    printf("  Storage: %d bytes for 32 weights = %.1f bytes/weight\n\n",
           sz, (double)sz / 32);
}

static void test_real_model(const char *path)
{
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
            int nb = (int)(nw/32);
            int nt2 = nb > 100 ? 100 : nb;
            uint8_t *raw = malloc(nt2*33);
            fseek(f, ds, SEEK_SET);
            fread(raw, 1, nt2*33, f);

            int total_sz = 0;
            int lossless = 1;
            int total_pass = 0;

            for (int b = 0; b < nt2; b++) {
                int8_t w8[32];
                for (int j = 0; j < 32; j++)
                    w8[j] = (int8_t)raw[b*33+2+j];

                uint8_t buf[128];
                int sz = encode_block(buf, w8, 32, 0);
                total_sz += sz;

                int8_t dec[32];
                decode_block(dec, buf, 32);
                for (int j = 0; j < 32; j++) {
                    if (dec[j] != w8[j]) { lossless = 0; break; }
                }
                int block_pass = 0;
                for (int j = 0; j < 32; j++) if (dec[j] == w8[j]) block_pass++;
                total_pass += block_pass;
            }

            double avg = (double)total_sz / nt2;
            printf("  Blocks: %d\n", nt2);
            printf("  Avg size: %.1f bytes/block\n", avg);
            printf("  Lossless: %s\n", lossless ? "YES ✓" : "NO");
            printf("  Exact values: %d/%d\n", total_pass, nt2*32);
            printf("  vs Q8_0 (34 B): %.4fx\n", avg / 34.0);

            free(raw);
            break;
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Beam Position Codec — weight = compute(pos, time)     ║\n");
    printf("║  frame_seek: 1440 positions, stride-37, O(1)           ║\n");
    printf("║  No compression. No lossy. Deterministic.              ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_basic();
    test_block();

    if (argc >= 2) test_real_model(argv[1]);

    return 0;
}
