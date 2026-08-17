/* tools/scale_residual_probe.c — T1.1b: scale-predict → residual → gradient
 * ═══════════════════════════════════════════════════════════════════════
 * user: "predict จาก scale view แล้วเก็บ gradient ของส่วนต่าง —
 *        พิสูจน์ว่า residual หลัง predict sparse กว่าค่าเดิมแค่ไหนบนข้อมูลจริง"
 *
 * chain (lossless พิสูจน์ครบ):
 *   x (height field)
 *     → scale view:  base = block mean | block center   (rows/B × cols/B)
 *     → predict:     pred[r][c] = base[block]           (อ่านที่ scale หยาบ)
 *     → residual:    res = (x − pred) & 0xFF            (อ่านที่ scale ละเอียด = จ่ายส่วนต่าง)
 *     → normal map:  dx(res) — gradient ของส่วนต่าง
 *   reconstruct:  (pred + res) & 0xFF == x   และ res จาก boundary+dx == res
 *
 * วัด: H(raw) เทียบ H(res) เทียบ H(dx ของ res) + sparsity + bits/cell รวม (base+res)
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/scale_residual_probe tools/scale_residual_probe.c -lm
 * RUN:
 *   build/scale_residual_probe --syn smooth|sine2d|noise <n>
 *   build/scale_residual_probe --file <path> [cols]
 *   build/scale_residual_probe --gguf <model.gguf> <tensor_idx>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gguf_reader.h"

#define MAXN   (8u << 20)
#define BINS   256u

typedef struct { uint64_t h[BINS]; uint64_t total; } Hist;

static void hist_init(Hist *H) { memset(H, 0, sizeof(*H)); }

static double hist_entropy(const Hist *H) {
    if (!H->total) return 0.0;
    double e = 0.0;
    for (int i = 0; i < (int)BINS; i++) {
        if (!H->h[i]) continue;
        double p = (double)H->h[i] / (double)H->total;
        e -= p * log(p) * 1.4426950408889634; /* log2 */
    }
    return e;
}

/* signed diff ภายใน 1 byte */
static int sdiff(int a, int b) {
    int d = a - b;
    if (d > 127) d -= 256;
    if (d < -127) d += 256;
    return d;
}

/* ── scale view + predict + residual ──
   base[i*bc+j] = ตัวแทน block (mean | center)
   res[r*cols+c] = (x − pred) & 0xFF
   วัดบน sub-region (br*B) × (bc*B) เท่านั้น (ตัดเศษขอบทิ้ง)        */
static void scale_residual(const uint8_t *x, uint32_t rows, uint32_t cols,
                           uint32_t B, int use_mean,
                           uint8_t *base, uint32_t *pbr, uint32_t *pbc,
                           uint8_t *res, uint32_t *n_used,
                           Hist *Hres, Hist *Hdxr, Hist *Hdyr,
                           double *sp /* [5] */) {
    uint32_t br = rows / B, bc = cols / B;
    if (br < 1) br = 1;
    if (bc < 1) bc = 1;
    *pbr = br; *pbc = bc;

    /* 1) base */
    for (uint32_t i = 0; i < br; i++)
        for (uint32_t j = 0; j < bc; j++) {
            uint64_t sum = 0;
            uint8_t cen = 0;
            for (uint32_t dr = 0; dr < B; dr++)
                for (uint32_t dc = 0; dc < B; dc++) {
                    uint8_t v = x[(size_t)(i * B + dr) * cols + (j * B + dc)];
                    sum += v;
                    if (dr == B / 2 && dc == B / 2) cen = v;
                }
            uint8_t v = use_mean ? (uint8_t)((sum + B * B / 2) / (uint64_t)(B * B)) : cen;
            base[(size_t)i * bc + j] = v;
        }

    /* 2) residual + hist */
    hist_init(Hres); hist_init(Hdxr); hist_init(Hdyr);
    uint32_t rn = br * B, cn = bc * B;
    uint64_t s[5] = {0, 0, 0, 0, 0};
    int ks[5] = {1, 2, 4, 8, 16};
    for (uint32_t r = 0; r < rn; r++) {
        for (uint32_t c = 0; c < cn; c++) {
            uint8_t pred = base[(size_t)(r / B) * bc + (c / B)];
            uint8_t rr = (uint8_t)((x[(size_t)r * cols + c] - pred) & 0xFFu);
            res[(size_t)r * cn + c] = rr;
            Hres->h[rr]++; Hres->total++;
            int sd = sdiff((int)x[(size_t)r * cols + c], (int)pred);
            for (int k = 0; k < 5; k++) if (abs(sd) <= ks[k]) s[k]++;
            if (c >= 1) {
                uint8_t l = res[(size_t)r * cn + c - 1];
                Hdxr->h[(rr - l) & 0xFFu]++; Hdxr->total++;
            }
            if (r >= 1) {
                uint8_t u = res[(size_t)(r - 1) * cn + c];
                Hdyr->h[(rr - u) & 0xFFu]++; Hdyr->total++;
            }
        }
    }
    for (int k = 0; k < 5; k++) sp[k] = Hres->total ? (double)s[k] / (double)Hres->total : 1.0;
    *n_used = rn * cn;
}

/* ── lossless chain: (pred + res) == x · res จาก boundary+dx == res ── */
static int verify_chain(const uint8_t *x, uint32_t rows, uint32_t cols,
                        const uint8_t *base, uint32_t br, uint32_t bc,
                        uint32_t B, const uint8_t *res, uint32_t n_used) {
    uint32_t rn = br * B, cn = bc * B;
    if (n_used != rn * cn || rows < rn || cols < cn) return -1;
    uint8_t *rec = (uint8_t *)malloc((size_t)rn * cn);
    uint8_t *rrec = (uint8_t *)malloc((size_t)rn * cn);
    if (!rec || !rrec) { free(rec); free(rrec); return -1; }
    int bad = 0;
    /* res → rrec (boundary col + dx) */
    for (uint32_t r = 0; r < rn; r++) {
        uint8_t acc = res[(size_t)r * cn];
        rrec[(size_t)r * cn] = acc;
        for (uint32_t c = 1; c < cn; c++) {
            uint8_t d = res[(size_t)r * cn + c] - res[(size_t)r * cn + c - 1];
            acc = (uint8_t)(acc + d);
            rrec[(size_t)r * cn + c] = acc;
        }
    }
    /* (pred + rrec) == x */
    for (uint32_t r = 0; r < rn; r++)
        for (uint32_t c = 0; c < cn; c++) {
            uint8_t pred = base[(size_t)(r / B) * bc + (c / B)];
            rec[(size_t)r * cn + c] = (uint8_t)(pred + rrec[(size_t)r * cn + c]);
        }
    for (uint32_t r = 0; r < rn && !bad; r++)
        if (memcmp(rec + (size_t)r * cn, x + (size_t)r * cols, cn) != 0) bad = 1;
    free(rec); free(rrec);
    return bad;
}

/* ── สังเคราะห์ (เหมือน normal_map_probe) ── */
static void synth_fill(uint8_t *x, uint32_t n, const char *kind, uint32_t *rows, uint32_t *cols) {
    uint32_t c = 512;
    uint32_t r = n / c; if (r < 1) r = 1;
    *rows = r; *cols = c;
    if (strcmp(kind, "noise") == 0) {
        uint64_t s = 0x9E3779B97F4A7C15ull;
        for (uint32_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; x[i] = (uint8_t)s; }
    } else if (strcmp(kind, "smooth") == 0) {
        for (uint32_t i = 0; i < n; i++)
            x[i] = (uint8_t)(((i / c) * 255u / r + (i % c) * 255u / c) / 2);
    } else if (strcmp(kind, "sine2d") == 0) {
        for (uint32_t i = 0; i < n; i++)
            x[i] = (uint8_t)(128 + 120 * sin((double)(i % c) * 0.1) * cos((double)(i / c) * 0.05));
    } else {
        for (uint32_t i = 0; i < n; i++) x[i] = (uint8_t)(i * 255u / n);
    }
}

static void run_report(const char *name, const uint8_t *x, uint32_t rows, uint32_t cols) {
    uint32_t n = rows * cols;
    Hist Hraw; hist_init(&Hraw);
    for (uint32_t i = 0; i < n; i++) { Hraw.h[x[i]]++; Hraw.total++; }
    double h_raw = hist_entropy(&Hraw);

    uint8_t *base = (uint8_t *)malloc((size_t)rows * cols / 2 + 16);
    uint8_t *res  = (uint8_t *)malloc((size_t)rows * cols + 16);
    if (!base || !res) { printf("alloc fail\n"); free(base); free(res); return; }

    static const uint32_t BLKS[4] = {2, 4, 8, 16};
    static const char *MNAME[2] = {"mean", "center"};

    printf("── %s  %ux%u  H(raw)=%5.2f ──\n", name, rows, cols, h_raw);
    printf("%-8s %-6s %8s %8s %8s %8s %8s %11s %11s   %s\n",
           "mode", "B", "H(res)", "H(dxr)", "H(dyr)", "res<=1%", "res<=16%",
           "tot res", "tot dxr", "lossless");
    for (int m = 0; m < 2; m++) {
        for (int b = 0; b < 4; b++) {
            uint32_t br, bc, n_used;
            Hist Hres, Hdxr, Hdyr;
            double sp[5];
            scale_residual(x, rows, cols, BLKS[b], m == 0, base, &br, &bc,
                           res, &n_used, &Hres, &Hdxr, &Hdyr, sp);
            double h_res  = hist_entropy(&Hres);
            double h_dxr  = hist_entropy(&Hdxr);
            double h_dyr  = hist_entropy(&Hdyr);
            double base_bits = (double)br * bc * 8.0;
            double tot_res = (base_bits + (double)n_used * h_res) / n_used;
            double tot_dxr = (base_bits + (double)(n_used - br) * h_dxr + (double)br * 8.0) / n_used;
            int ok = verify_chain(x, rows, cols, base, br, bc, BLKS[b], res, n_used);
            printf("%-8s %-6u %8.2f %8.2f %8.2f %7d%% %7d%% %11.2f %11.2f   %s\n",
                   MNAME[m], BLKS[b], h_res, h_dxr, h_dyr,
                   (int)(sp[0] * 100), (int)(sp[4] * 100),
                   tot_res, tot_dxr, ok == 0 ? "OK" : "FAIL");
        }
    }
    free(base); free(res);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage:\n"
               "  %s --syn <smooth|sine2d|noise|ramp> <n>\n"
               "  %s --file <path> [cols]\n"
               "  %s --gguf <model.gguf> <tensor_idx>\n", argv[0], argv[0], argv[0]);
        return 1;
    }
    uint8_t *x = (uint8_t *)malloc(MAXN);
    if (!x) return 1;

    if (strcmp(argv[1], "--syn") == 0 && argc >= 4) {
        uint32_t n = (uint32_t)strtoul(argv[3], NULL, 10);
        if (n > MAXN) n = MAXN;
        uint32_t rows, cols;
        synth_fill(x, n, argv[2], &rows, &cols);
        run_report(argv[2], x, rows, cols);
        return 0;
    }

    if (strcmp(argv[1], "--file") == 0 && argc >= 3) {
        FILE *fp = fopen(argv[2], "rb");
        if (!fp) { printf("cannot open %s\n", argv[2]); return 1; }
        uint32_t n = (uint32_t)fread(x, 1, MAXN, fp);
        fclose(fp);
        uint32_t cols = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 10) : 512;
        if (cols == 0 || cols > n) cols = (n < 512 ? n : 512);
        uint32_t rows = n / cols; if (rows < 1) rows = 1;
        run_report(argv[2], x, rows, cols);
        return 0;
    }

    if (strcmp(argv[1], "--gguf") == 0 && argc >= 4) {
        const char *path = argv[2];
        uint32_t idx = (uint32_t)strtoul(argv[3], NULL, 10);
        GgufReader r;
        if (gguf_open(path, &r) != 0) { printf("cannot open %s\n", path); return 1; }
        if (idx >= r.n_tensors) { printf("idx %u out of range (%u)\n", idx, r.n_tensors); return 1; }
        uint64_t sz = r.sizes[idx];
        uint8_t *t = (uint8_t *)malloc((size_t)(sz > 0 ? sz : 1));
        if (!t) { printf("alloc fail %lu bytes\n", (unsigned long)sz); return 1; }
        if (gguf_read_tensor(path, &r, idx, t, (uint32_t)(sz > 0xFFFFFFFFull ? 0xFFFFFFFFull : sz)) != 0) {
            printf("read fail\n"); free(t); return 1;
        }
        x = t;
        uint32_t cols = (uint32_t)r.dims[idx * 4];
        if (cols == 0) cols = 1;
        uint32_t rows = (uint32_t)(sz / cols); if (rows < 1) rows = 1;
        char nm[96];
        snprintf(nm, sizeof(nm), "%s[%u] %s", path, idx, r.names[idx]);
        run_report(nm, x, rows, cols);
        return 0;
    }

    printf("unknown mode\n");
    return 1;
}
