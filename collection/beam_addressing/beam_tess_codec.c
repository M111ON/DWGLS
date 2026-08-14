/*
 * beam_tess_codec.c — Position from Tessellation (real structure)
 *
 * Tessellation: 20736 nodes on icosahedron
 *   GEO_FULL = 20736 = 12 pentagons × 1728
 *   1728 = 12 shells × 144 cells
 *   node_id = face × 1728 + shell × 144 + local
 *
 * Weight → tessellation node_id (collision-free).
 * Position IS the tessellation coordinate.
 * Decode: node_id → weight via LUT.
 *
 * Storage: node_id per weight (15 bits for 20736 values)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ══════════════════════════════════════════════════════════════
   TESSELLATION CONSTANTS (from geo_jump.h)
   ══════════════════════════════════════════════════════════════ */

#define GEO_FULL        20736u
#define GEO_PENTAGONS   12u
#define GEO_TOWER       144u
#define GEO_SHELL_TICK  12u
#define GEO_FIBO_CLOCK  1440u
#define FACE_STRIDE     (GEO_FULL / GEO_PENTAGONS)  /* 1728 */

/* Tessellation decompose/recompose */
static inline uint32_t tess_face(uint32_t node)  { return node / FACE_STRIDE; }
static inline uint32_t tess_shell(uint32_t node) { return (node / GEO_TOWER) % GEO_SHELL_TICK; }
static inline uint32_t tess_local(uint32_t node) { return node % GEO_TOWER; }
static inline uint32_t tess_node(uint32_t face, uint32_t shell, uint32_t local) {
    return face * FACE_STRIDE + shell * GEO_TOWER + local;
}

/* ══════════════════════════════════════════════════════════════
   WEIGHT → TESSELLATION NODE (collision-free)
   ══════════════════════════════════════════════════════════════
 *
 * 256 weights → 20736 nodes.
 * Mapping uses tessellation structure directly:
 *   weight w → (face, shell, local) on 20736 grid.
 *
 * Distribute 256 weights across faces and shells evenly.
 * Within each face+shell, pick a unique local position.
 *
 * Strategy: stride through (face, shell) pairs, then local.
 * Total available: 12 faces × 12 shells × 144 locals = 20736
 * We need only 256, so we have plenty of room.
 */
static uint32_t weight_to_node(int8_t weight)
{
    /* Map weight to 0..255 */
    uint32_t w = (uint32_t)(weight + 128);

    /*
     * Tessellation mapping (collision-free):
     *   face  = w / 22  (0..11, 22 weights per face)
     *   shell = (w % 22) / 2  (0..10)
     *   local = ((w % 22) % 2) × 72 + (w / 22) × 3  (spread within shell)
     *
     * This distributes weights across faces and shells.
     * Each (face, shell) gets at most 2 weights.
     */
    uint32_t face  = w / 22;
    uint32_t rem   = w % 22;
    uint32_t shell = rem / 2;
    uint32_t sub   = rem % 2;

    /* Local position: spread within shell to avoid clustering */
    uint32_t local = sub * 72 + (w % 72);

    if (face >= GEO_PENTAGONS) face = GEO_PENTAGONS - 1;
    if (shell >= GEO_SHELL_TICK) shell = GEO_SHELL_TICK - 1;
    if (local >= GEO_TOWER) local = local % GEO_TOWER;

    return tess_node(face, shell, local);
}

/* ══════════════════════════════════════════════════════════════
   LUT: node_id → weight
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t node_id;
    int8_t   weight;
} NodeLut;

static NodeLut node_lut[256];

static void node_lut_build(void)
{
    for (int w = -128; w <= 127; w++) {
        uint32_t node = weight_to_node((int8_t)w);
        node_lut[w + 128].node_id = node;
        node_lut[w + 128].weight = (int8_t)w;
    }
}

static int8_t node_to_weight(uint32_t node)
{
    for (int i = 0; i < 256; i++) {
        if (node_lut[i].node_id == node)
            return node_lut[i].weight;
    }
    return 0;
}

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
   ENCODE / DECODE
   ══════════════════════════════════════════════════════════════
 *
 * Store per weight:
 *   face:  0..11  → 4 bits
 *   shell: 0..11  → 4 bits
 *   local: 0..143 → 8 bits
 *   Total: 16 bits per weight (2 bytes)
 */

#define NODE_BITS 16

static int encode_block(uint8_t *out, const int8_t *weights, int n)
{
    memset(out, 0, n * 2 + 4);
    int pos = 0;

    out[pos++] = (uint8_t)n;

    for (int i = 0; i < n; i++) {
        uint32_t node = weight_to_node(weights[i]);
        uint32_t face  = tess_face(node);
        uint32_t shell = tess_shell(node);
        uint32_t local = tess_local(node);

        int val = (face << 12) | (shell << 8) | local;
        wbits(out, pos, val, NODE_BITS);
        pos += NODE_BITS;
    }

    return (pos + 7) / 8;
}

static void decode_block(int8_t *out, const uint8_t *buf, int max_n)
{
    int pos = 0;
    int n = buf[pos++];
    if (n > max_n) n = max_n;

    for (int i = 0; i < n; i++) {
        int val = rbits(buf, pos, NODE_BITS);
        pos += NODE_BITS;

        uint32_t face  = (val >> 12) & 0xF;
        uint32_t shell = (val >> 8) & 0xF;
        uint32_t local = val & 0xFF;

        uint32_t node = tess_node(face, shell, local);
        out[i] = node_to_weight(node);
    }
}

/* ══════════════════════════════════════════════════════════════
   TESTS
   ══════════════════════════════════════════════════════════════ */

static void test_collision(void)
{
    printf("=== Collision Test ===\n");

    node_lut_build();

    /* Check all 256 weights map to unique nodes */
    uint32_t seen[GEO_FULL];
    memset(seen, 0, sizeof(seen));
    int pass = 0, fail = 0;

    for (int w = -128; w <= 127; w++) {
        uint32_t node = weight_to_node((int8_t)w);
        if (node < GEO_FULL) {
            if (seen[node] == 0) {
                seen[node] = 1;
                pass++;
            } else {
                fail++;
                if (fail <= 5) printf("  COLLISION: w=%d → node=%u (already used)\n", w, node);
            }
        } else {
            fail++;
        }
    }
    printf("  Unique nodes: PASS %d/256  FAIL %d\n\n", pass, fail);
}

static void test_roundtrip(void)
{
    printf("=== Roundtrip Test ===\n");

    node_lut_build();

    int pass = 0, fail = 0;
    for (int w = -128; w <= 127; w++) {
        uint32_t node = weight_to_node((int8_t)w);
        int8_t decoded = node_to_weight(node);
        if (decoded == (int8_t)w) pass++;
        else { fail++; if (fail <= 5) printf("  FAIL: w=%d node=%u decoded=%d\n", w, node, decoded); }
    }
    printf("  PASS: %d/256  FAIL: %d\n\n", pass, fail);
}

static void test_block(void)
{
    printf("=== Block Test ===\n");

    node_lut_build();

    int8_t weights[32];
    srand(42);
    for (int i = 0; i < 32; i++) weights[i] = (int8_t)(rand() % 256 - 128);

    uint8_t buf[128];
    int sz = encode_block(buf, weights, 32);

    int8_t decoded[32];
    decode_block(decoded, buf, 32);

    int pass = 0;
    for (int i = 0; i < 32; i++) {
        if (decoded[i] == weights[i]) pass++;
    }

    printf("  Block size: %d bytes\n", sz);
    printf("  PASS: %d/32\n", pass);
    printf("  vs Q8_0 (34 B): %.4fx\n\n", (double)sz / 34.0);
}

static void test_real_model(const char *path)
{
    printf("=== Real Model Test ===\n");

    node_lut_build();

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
            printf("  Tensor: Q8_0, %I64u weights\n", (unsigned long long)nw);
            long ds = ftell(f);
            int nb = (int)(nw / 32);
            int nt2 = nb > 100 ? 100 : nb;
            uint8_t *raw = malloc(nt2 * 33);
            fseek(f, ds, SEEK_SET);
            fread(raw, 1, nt2 * 33, f);

            int total_sz = 0;
            int lossless = 1;
            int total_pass = 0;

            for (int b = 0; b < nt2; b++) {
                int8_t w8[32];
                for (int j = 0; j < 32; j++)
                    w8[j] = (int8_t)raw[b * 33 + 2 + j];

                uint8_t buf2[128];
                int sz = encode_block(buf2, w8, 32);
                total_sz += sz;

                int8_t dec[32];
                decode_block(dec, buf2, 32);
                for (int j = 0; j < 32; j++) {
                    if (dec[j] != w8[j]) { lossless = 0; }
                }
                for (int j = 0; j < 32; j++) if (dec[j] == w8[j]) total_pass++;
            }

            double avg = (double)total_sz / nt2;
            printf("  Blocks: %d\n", nt2);
            printf("  Avg size: %.1f bytes/block\n", avg);
            printf("  Lossless: %s\n", lossless ? "YES ✓" : "NO");
            printf("  Exact values: %d/%d\n", total_pass, nt2 * 32);
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
    printf("║  Beam Tess Codec — position from tessellation          ║\n");
    printf("║  20736 nodes, 12 pentagons, 12 shells, 144 cells     ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_collision();
    test_roundtrip();
    test_block();

    if (argc >= 2) test_real_model(argv[1]);

    return 0;
}
