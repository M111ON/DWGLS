/* tools/normal_map_probe.c — T1.1: normal/bump/displacement map บน data
 * ═══════════════════════════════════════════════════════════════════════
 * user: "เอาแนวคิด normal/bump/displacement map แบบ virtual มาใช้ —
 *        แปลงค่าเป็น intensity/depth แล้วย่อยเป็น int"
 *
 * data = height field (bytes) → วัดว่าการแปลงเป็น "map" ลด entropy ได้ไหม:
 *   H(raw)  — entropy ต่อ byte ของค่าตรง
 *   H(d1d)  — 1D diff (เส้นเดียว — เทียบ wave_delta_probe)
 *   H(dx)   — 2D horizontal gradient (normal map x-component)
 *   H(dy)   — 2D vertical gradient   (normal map y-component)
 *   small%  — สัดส่วน |gradient| ≤ k (บอกความ compressible ของ map)
 *
 * Lossless พิสูจน์: integrate กลับจาก (boundary col + dx) → memcmp เทียบต้นฉบับ
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -o build/normal_map_probe tools/normal_map_probe.c -lm
 * RUN:
 *   build/normal_map_probe --syn smooth 262144      (สังเคราะห์)
 *   build/normal_map_probe --syn sine2d 262144
 *   build/normal_map_probe --syn noise 262144
 *   build/normal_map_probe --file F:/notebookLM/xxx.pdf 512
 *   build/normal_map_probe --gguf /i/model/Qwen3-0.6B-Q8_0.gguf <tensor_idx>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gguf_reader.h"

#define MAXN  (8u << 20)   /* 8 MB cap ต่อชิ้น */
#define BINS  256u

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

/* ── 1D diff: d[i] = (x[i] − x[i−1]) & 0xFF ── */
static void hist_1d_diff(const uint8_t *x, uint32_t n, Hist *H) {
    hist_init(H);
    for (uint32_t i = 1; i < n; i++) {
        H->h[(x[i] - x[i - 1]) & 0xFFu]++;
        H->total++;
    }
}

/* ── 2D: reshape rows×cols → dx (แนวนอน), dy (แนวตั้ง) ──
   dx[r][c] = (x[r][c] − x[r][c−1]) & 0xFF  สำหรับ c ≥ 1
   dy[r][c] = (x[r][c] − x[r−1][c]) & 0xFF  สำหรับ r ≥ 1        */
static void hist_2d_normals(const uint8_t *x, uint32_t rows, uint32_t cols,
                            Hist *Hdx, Hist *Hdy) {
    hist_init(Hdx); hist_init(Hdy);
    for (uint32_t r = 0; r < rows; r++) {
        const uint8_t *row = x + (size_t)r * cols;
        for (uint32_t c = 0; c < cols; c++) {
            if (c >= 1) { Hdx->h[(row[c] - row[c - 1]) & 0xFFu]++; Hdx->total++; }
            if (r >= 1) {
                Hdy->h[(row[c] - x[(size_t)(r - 1) * cols + c]) & 0xFFu]++;
                Hdy->total++;
            }
        }
    }
}

/* ── Lossless: integrate กลับจาก boundary col + dx (scanline) ── */
static int reconstruct_lossless(const uint8_t *orig, uint32_t rows, uint32_t cols) {
    uint8_t *rec = (uint8_t *)malloc((size_t)rows * cols);
    if (!rec) return -1;
    /* boundary col = คอลัมน์แรก (เก็บตรงๆ) · dx = ผลต่างแนวนอน */
    for (uint32_t r = 0; r < rows; r++) {
        uint8_t acc = orig[(size_t)r * cols];      /* x[r][0] */
        rec[(size_t)r * cols] = acc;
        for (uint32_t c = 1; c < cols; c++) {
            uint8_t dx = (orig[(size_t)r * cols + c] - orig[(size_t)r * cols + c - 1]) & 0xFFu;
            acc = (uint8_t)(acc + dx);
            rec[(size_t)r * cols + c] = acc;
        }
    }
    int bad = memcmp(rec, orig, (size_t)rows * cols);
    free(rec);
    return bad;
}

/* ── smallness profile: สัดส่วน |signed diff| ≤ k ── */
static void small_profile(const uint8_t *x, uint32_t rows, uint32_t cols,
                          double *out_dx, double *out_dy) {
    uint64_t ndx = 0, ndy = 0;
    uint64_t sdx[5] = {0}, sdy[5] = {0};
    int ks[5] = {1, 2, 4, 8, 16};
    for (uint32_t r = 0; r < rows; r++) {
        const uint8_t *row = x + (size_t)r * cols;
        for (uint32_t c = 0; c < cols; c++) {
            if (c >= 1) {
                int d = (int)row[c] - (int)row[c - 1];
                if (d > 127) d -= 256;
                if (d < -127) d += 256;
                ndx++;
                for (int k = 0; k < 5; k++) if (abs(d) <= ks[k]) sdx[k]++;
            }
            if (r >= 1) {
                int d = (int)row[c] - (int)x[(size_t)(r - 1) * cols + c];
                if (d > 127) d -= 256;
                if (d < -127) d += 256;
                ndy++;
                for (int k = 0; k < 5; k++) if (abs(d) <= ks[k]) sdy[k]++;
            }
        }
    }
    for (int k = 0; k < 5; k++) {
        out_dx[k] = ndx ? (double)sdx[k] / (double)ndx : 1.0;
        out_dy[k] = ndy ? (double)sdy[k] / (double)ndy : 1.0;
    }
}

/* ── สังเคราะห์ ── */
static void synth_fill(uint8_t *x, uint32_t n, const char *kind, uint32_t *rows, uint32_t *cols) {
    uint32_t c = 512;                       /* ความกว้างตายตัว 512 */
    uint32_t r = n / c; if (r < 1) r = 1;
    *rows = r; *cols = c;
    if (strcmp(kind, "noise") == 0) {
        uint64_t s = 0x9E3779B97F4A7C15ull;
        for (uint32_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; x[i] = (uint8_t)s; }
    } else if (strcmp(kind, "smooth") == 0) {
        /* ไล่ระดับ 2 มิติ — ผิวเรียบที่สุด */
        for (uint32_t i = 0; i < n; i++)
            x[i] = (uint8_t)(((i / c) * 255u / r + (i % c) * 255u / c) / 2);
    } else if (strcmp(kind, "sine2d") == 0) {
        for (uint32_t i = 0; i < n; i++)
            x[i] = (uint8_t)(128 + 120 * sin((double)(i % c) * 0.1) * cos((double)(i / c) * 0.05));
    } else {
        /* ramp 1 มิติ */
        for (uint32_t i = 0; i < n; i++) x[i] = (uint8_t)(i * 255u / n);
    }
}

static void report(const char *name, const uint8_t *x, uint32_t rows, uint32_t cols) {
    uint32_t n = rows * cols;
    Hist Hraw, Hd1d, Hdx, Hdy;
    hist_init(&Hraw);
    for (uint32_t i = 0; i < n; i++) { Hraw.h[x[i]]++; Hraw.total++; }
    hist_1d_diff(x, n, &Hd1d);
    hist_2d_normals(x, rows, cols, &Hdx, &Hdy);
    double p_dx[5], p_dy[5];
    small_profile(x, rows, cols, p_dx, p_dy);

    double h_raw = hist_entropy(&Hraw);
    double h_d1d = hist_entropy(&Hd1d);
    double h_dx  = hist_entropy(&Hdx);
    double h_dy  = hist_entropy(&Hdy);

    /* total bits: raw = n·h_raw ·
       normal-map = boundary col (rows·8) + (n−rows)·h_dx (+ dy ถ้าเก็บทั้ง 2 องค์ประกอบ) */
    double bits_raw   = n * h_raw;
    double bits_dx    = (double)rows * 8.0 + (double)(n - rows) * h_dx;
    double bits_dxdy  = bits_dx + (double)(n - rows - cols + 1) * h_dy;

    int lossless = reconstruct_lossless(x, rows, cols);

    printf("%-26s %8ux%-4u ", name, rows, cols);
    printf("H(raw)=%5.2f  H(d1d)=%5.2f  H(dx)=%5.2f  H(dy)=%5.2f  ",
           h_raw, h_d1d, h_dx, h_dy);
    printf("bits/cell: raw %5.2f | dx-only %5.2f | dx+dy %5.2f  ",
           bits_raw / n, bits_dx / n, bits_dxdy / n);
    printf("small(|dx|≤1,2,4,8,16): %2d %2d %2d %2d %2d%%  ",
           (int)(p_dx[0]*100), (int)(p_dx[1]*100), (int)(p_dx[2]*100),
           (int)(p_dx[3]*100), (int)(p_dx[4]*100));
    printf("lossless=%s\n", lossless == 0 ? "OK" : "FAIL");
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
        report(argv[2], x, rows, cols);
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
        report(argv[2], x, rows, cols);
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
        if (!t) { printf("alloc fail %llu bytes\n", (unsigned long long)sz); return 1; }
        if (gguf_read_tensor(path, &r, idx, t, (uint32_t)(sz > 0xFFFFFFFFull ? 0xFFFFFFFFull : sz)) != 0) {
            printf("read fail\n"); free(t); return 1;
        }
        x = t;
        uint32_t cols = (uint32_t)r.dims[idx * 4];   /* innermost = แถว */
        if (cols == 0) cols = 1;
        uint32_t rows = (uint32_t)(sz / cols); if (rows < 1) rows = 1;
        char nm[80];
        snprintf(nm, sizeof(nm), "%s[%u] %s", path, idx, r.names[idx]);
        report(nm, x, rows, cols);
        return 0;
    }

    printf("unknown mode\n");
    return 1;
}
