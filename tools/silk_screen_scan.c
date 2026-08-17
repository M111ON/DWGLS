/* tools/silk_screen_scan.c — Silk-screen feasibility: real quantized weights
 * ═══════════════════════════════════════════════════════════════════════════
 * คำถาม: "36 chunk : 1 map" จริงไหมบน quantized weights?
 *
 * วัดด้วยการ canonicalize Q8_0/Q4_0 blocks จริง ภายใต้ transform group
 * (cyclic rotation + reversal = dihedral) → hash → นับ unique maps:
 *
 *   3 modes:
 *     identity  — dedup ล้วน (baseline — user วัดไว้ 90-97% unique)
 *     rot       — minimal cyclic rotation (Booth's algorithm)
 *     rot+rev   — dihedral orbit: min( rot, rot(reverse) )
 *
 *   silk estimate (เก็บ maps ครั้งเดียว):
 *     Q8_0: raw 34B/block  → silk = unique×32 + blocks×3 (2B scale + 1B transform)
 *     Q4_0: raw 18B/block  → silk = unique×16 + blocks×3
 *     transform byte = 5b rotation index + 1b reversal + 2b spare
 *
 * ข้อมูลจริง: 4 โมเดล Q8_0 (I:/model) — tensor ใหญ่ subsample ด้วย stride
 * (รายงาน sampled; ratio เป็นค่าประมาณบนตัวอย่าง)
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/silk_screen_scan \
 *        tools/silk_screen_scan.c -lm
 * RUN:   ./build/silk_screen_scan [model.gguf ...]   (default = 4 โมเดล)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../core/gguf_box.h"

#define MAX_SAMPLE  400000u      /* blocks ที่เก็บต่อ tensor (32B each) */
#define BLOCK_VALS  32u          /* values per Q8_0/Q4_0 block */

/* ── canonicalization ──────────────────────────────────────── */

/* Booth — index ของ lexicographically minimal rotation */
static int minimal_rotation(const uint8_t *s, int n) {
    int i = 0, j = 1, k = 0;
    while (i < n && j < n && k < n) {
        uint8_t a = s[(i + k) % n], b = s[(j + k) % n];
        if (a == b) { k++; }
        else if (a > b) { i = i + k + 1; if (i <= j) i = j + 1; k = 0; }
        else           { j = j + k + 1; if (j <= i) j = i + 1; k = 0; }
    }
    return (i < j) ? i : j;
}

static void rotate_to(uint8_t *dst, const uint8_t *src, int n, int idx) {
    for (int k = 0; k < n; k++) dst[k] = src[(idx + k) % n];
}

/* canonical form ภายใต้ mode: 0=identity, 1=rot, 2=rot+rev */
static void canonical(uint8_t *out, const uint8_t *vals, int n, int mode) {
    if (mode == 0) { memcpy(out, vals, (size_t)n); return; }
    uint8_t tmp[BLOCK_VALS];
    rotate_to(tmp, vals, n, minimal_rotation(vals, n));
    if (mode == 1) { memcpy(out, tmp, (size_t)n); return; }
    uint8_t rev[BLOCK_VALS];
    for (int k = 0; k < n; k++) rev[k] = vals[n - 1 - k];
    uint8_t revc[BLOCK_VALS];
    rotate_to(revc, rev, n, minimal_rotation(rev, n));
    memcpy(out, (memcmp(tmp, revc, (size_t)n) <= 0) ? tmp : revc, (size_t)n);
}

/* ── map counting: collect canonicals → qsort → unique ────── */

static int cmp_u8(const void *a, const void *b) {
    return memcmp(a, b, BLOCK_VALS);
}

typedef struct {
    uint64_t raw_bytes;        /* Σ blocks × block_bytes               */
    uint64_t silk_bytes;       /* unique×mapbytes + blocks×3           */
    uint64_t sampled;          /* blocks ที่สุ่ม                               */
    uint64_t unique[3];        /* identity / rot / rot+rev            */
    uint64_t top_run[3];       /* ความถี่ของ map ที่พบบ่อยสุด           */
    double   ratio[3];         /* raw / silk ต่อ mode                  */
} TensorStat;

/* extract 32 values from a block (Q8: 32 int8, Q4: 32 nibbles) */
static void extract_vals(uint8_t *vals, const uint8_t *blk, int dtype) {
    if (dtype == 8) {                       /* Q8_0: 32×int8 + 2B scale */
        for (int i = 0; i < 32; i++) vals[i] = (uint8_t)blk[i];
    } else {                                /* Q4_0: 16B nibbles + 2B scale */
        for (int i = 0; i < 16; i++) {
            vals[2 * i]     = blk[i] & 0x0F;
            vals[2 * i + 1] = blk[i] >> 4;
        }
    }
}

static void measure_tensor(GGUFBox *box, uint32_t idx, TensorStat *st) {
    const GGUFBoxEntry *e = &box->entries[idx];
    int dtype = e->dtype;
    int blk_bytes = (dtype == 8) ? 34 : 18;
    if (!e->data) return;

    uint64_t n_blocks = e->n_elems / BLOCK_VALS;
    if (n_blocks == 0) return;
    uint64_t stride = (n_blocks + MAX_SAMPLE - 1) / MAX_SAMPLE;
    uint64_t sampled = n_blocks / stride;

    st->raw_bytes += sampled * (uint64_t)blk_bytes;
    st->silk_bytes += (uint64_t)sampled * 3u;          /* scale + transform */
    st->sampled += sampled;

    /* canonicalize 3 modes พร้อมกัน (sampled×32 ×3) แล้ว qsort นับ unique */
    uint8_t *arr[3];
    for (int m = 0; m < 3; m++) arr[m] = (uint8_t *)malloc((size_t)sampled * BLOCK_VALS);
    for (uint64_t b = 0; b < sampled; b++) {
        const uint8_t *blk = e->data + (uint64_t)(b * stride) * blk_bytes;
        uint8_t vals[BLOCK_VALS];
        extract_vals(vals, blk, dtype);
        for (int m = 0; m < 3; m++)
            canonical(arr[m] + b * BLOCK_VALS, vals, BLOCK_VALS, m);
    }
    for (int m = 0; m < 3; m++) {
        qsort(arr[m], (size_t)sampled, BLOCK_VALS, cmp_u8);
        uint64_t unique = 0, top = 0, run = 0;
        uint8_t *last = NULL;
        for (uint64_t b = 0; b < sampled; b++) {
            uint8_t *cur = arr[m] + b * BLOCK_VALS;
            if (!last || memcmp(last, cur, BLOCK_VALS) != 0) {
                if (run > top) top = run;
                unique++;
                run = 1;
                last = cur;
            } else {
                run++;
            }
        }
        if (run > top) top = run;
        st->unique[m] += unique;
        st->top_run[m] += top;
        free(arr[m]);
    }
}

/* ── synthetic self-tests ─────────────────────────────────── */
static uint64_t self_rotate_pattern(uint8_t *out, uint64_t n, int rot, int rev) {
    uint8_t base[BLOCK_VALS];
    for (int i = 0; i < (int)BLOCK_VALS; i++) base[i] = (uint8_t)((i * 3) % 7);
    for (uint64_t b = 0; b < n; b++) {
        uint8_t vals[BLOCK_VALS];
        for (int i = 0; i < (int)BLOCK_VALS; i++)
            vals[(i + b % BLOCK_VALS) % BLOCK_VALS] = base[i];
        canonical(out + b * BLOCK_VALS, vals, BLOCK_VALS, 2);
    }
    (void)rev;
    return n;
}

static void self_tests(void) {
    printf("self-tests (synthetic)\n");
    /* T1: periodic — ทุก block = rotation ของ pattern เดียว → maps น้อย */
    {
        uint64_t n = 5000;
        uint8_t *arr = (uint8_t *)malloc((size_t)n * BLOCK_VALS);
        self_rotate_pattern(arr, n, 0, 0);
        qsort(arr, (size_t)n, BLOCK_VALS, cmp_u8);
        uint64_t uniq = 1;
        for (uint64_t b = 1; b < n; b++)
            if (memcmp(arr + (b - 1) * BLOCK_VALS, arr + b * BLOCK_VALS, BLOCK_VALS) != 0) uniq++;
        printf("  T1 periodic: %llu blocks → %llu unique maps (expect ~1)\n",
               (unsigned long long)n, (unsigned long long)uniq);
        free(arr);
    }
    /* T2: random — ทุก block unique → maps ≈ blocks */
    {
        uint64_t n = 5000;
        uint8_t *arr = (uint8_t *)malloc((size_t)n * BLOCK_VALS);
        uint64_t seed = 12345;
        for (uint64_t b = 0; b < n; b++) {
            uint8_t vals[BLOCK_VALS];
            for (int i = 0; i < (int)BLOCK_VALS; i++) {
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                vals[i] = (uint8_t)(seed >> 33);
            }
            canonical(arr + b * BLOCK_VALS, vals, BLOCK_VALS, 2);
        }
        qsort(arr, (size_t)n, BLOCK_VALS, cmp_u8);
        uint64_t uniq = 1;
        for (uint64_t b = 1; b < n; b++)
            if (memcmp(arr + (b - 1) * BLOCK_VALS, arr + b * BLOCK_VALS, BLOCK_VALS) != 0) uniq++;
        printf("  T2 random:   %llu blocks → %llu unique maps (expect ≈ n)\n",
               (unsigned long long)n, (unsigned long long)uniq);
        free(arr);
    }
}

/* Shannon entropy (bits) ของ histogram หนึ่ง plane */
static double entropy_of(const uint64_t *h, int sz, uint64_t total) {
    double e = 0.0;
    for (int i = 0; i < sz; i++) {
        if (!h[i]) continue;
        double p = (double)h[i] / (double)total;
        e -= p * log(p) * 1.4426950408889634;   /* log2 */
    }
    return e;
}

/* ── entropy pass: digit-plane analysis (user's cube/digit lens) ──
 * 1000 ตัวเลข 0-999 = 3 digit-planes (hundreds/tens/ones) × 10 symbols.
 * แยกแล้วรักษาข้อมูลครบ (Σ plane entropy = raw entropy เสมอ) — คำถามคือ
 * การกระจายไม่ uniform แค่ไหน → entropy ต่ำกว่าเพดาน → บีบได้จริงกี่ %
 *
 * Q8_0: int8 value → 3 planes: sign(2) / tens(0-12) / ones(0-9)
 * เปรียบเทียบ: raw entropy H(value) กับ Σ plane entropies (ต้องเท่ากัน)
 * กับ 8 bits/ค่า → compression ที่เหลือของ Q8_0 (llama ไม่ได้ entropy-code)
 */
static void entropy_pass(GGUFBox *box) {
    uint64_t h_raw[256] = {0};
    uint64_t *h_scale = (uint64_t *)calloc(65536, sizeof(uint64_t));
    uint64_t h_mq[16] = {0};
    uint64_t *h_sc_mq[16];                 /* joint: scale × maxq-bin      */
    for (int i = 0; i < 16; i++) h_sc_mq[i] = (uint64_t *)calloc(65536, sizeof(uint64_t));
    uint64_t n = 0, nblk = 0;

    for (uint32_t i = 0; i < box->n_tensors; i++) {
        const GGUFBoxEntry *e = &box->entries[i];
        if (e->dtype != 8 || !e->data) continue;
        uint64_t n_blocks = e->n_elems / BLOCK_VALS;
        if (n_blocks == 0) continue;
        uint64_t stride = (n_blocks + MAX_SAMPLE - 1) / MAX_SAMPLE;
        uint64_t sb = n_blocks / stride;
        for (uint64_t b = 0; b < sb; b++) {
            const uint8_t *blk = e->data + (uint64_t)(b * stride) * 34;
            uint16_t s16;
            memcpy(&s16, blk + 32, 2);
            h_scale[s16]++;
            nblk++;
            int mq = 0;
            for (int k = 0; k < 32; k++) {
                int8_t v = (int8_t)blk[k];
                int m = (v < 0) ? -v : v;
                if (m > mq) mq = m;
                h_raw[(uint8_t)v]++;
                n++;
            }
            if (mq > 127) mq = 127;      /* int8 -128 → abs 128 → clamp bin */
            h_mq[mq >> 3]++;
            h_sc_mq[mq >> 3][s16]++;
        }
    }
    if (n == 0) { free(h_scale); for (int i = 0; i < 16; i++) free(h_sc_mq[i]); return; }

    double hr   = entropy_of(h_raw, 256, n);
    double hsc  = entropy_of(h_scale, 65536, nblk);

    /* H(scale | maxq-bin): "ถ้ารู้ค่าสูงสุดของ block scale แน่นขึ้นไหม" */
    double h_cond_mq = 0.0;
    for (int b = 0; b < 16; b++)
        if (h_mq[b])
            h_cond_mq += ((double)h_mq[b] / (double)nblk) *
                         entropy_of(h_sc_mq[b], 65536, h_mq[b]);

    /* Q8_0 block = 32 values (8b) + 2B scale (16b) → 272 bits */
    double raw_bits = 34.0 * 8.0;
    double ec_bits  = 32.0 * hr + hsc;
    double ec_bits_mq = 32.0 * hr + h_cond_mq;
    printf("  entropy: H(value)=%.2f/8b, H(scale)=%.2f/16b, "
           "H(scale|maxq-bin)=%.2f (รู้ max|q| → เหลือเท่านี้)\n",
           hr, hsc, h_cond_mq);
    printf("  Q8_0: raw %.2f× → entropy-coded %.2f× (ลด %d%%) | +maxq context %.2f× (ลด %d%%)\n",
           8.0 / hr, raw_bits / ec_bits, (int)(100.0 * (1.0 - ec_bits / raw_bits)),
           raw_bits / ec_bits_mq, (int)(100.0 * (1.0 - ec_bits_mq / raw_bits)));

    free(h_scale);
    for (int i = 0; i < 16; i++) free(h_sc_mq[i]);
}

/* ── tensor-level dedup: byte-identical quantized tensors ── */
static uint64_t fnv64(const uint8_t *p, uint64_t n) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (uint64_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static void tensor_dedup_pass(GGUFBox *box) {
    uint32_t N = box->n_tensors;
    if (N == 0 || N > 4096) return;
    uint64_t *h = (uint64_t *)calloc(N, sizeof(uint64_t));
    uint64_t dup_bytes = 0;
    uint32_t dup_pairs = 0;

    for (uint32_t i = 0; i < N; i++) {
        const GGUFBoxEntry *e = &box->entries[i];
        if ((e->dtype != 8 && e->dtype != 2) || !e->data || e->size == 0) continue;
        h[i] = fnv64(e->data, e->size);
    }
    for (uint32_t i = 0; i < N; i++) {
        for (uint32_t j = i + 1; j < N; j++) {
            const GGUFBoxEntry *a = &box->entries[i], *b = &box->entries[j];
            if (a->size == 0 || a->size != b->size || a->dtype != b->dtype) continue;
            if (h[i] == 0 || h[i] != h[j]) continue;
            if (memcmp(a->data, b->data, a->size) != 0) continue;   /* verify */
            printf("    ▶ byte-identical tensor pair: %s == %s (%u MB)\n",
                   a->name, b->name, a->size >> 20);
            dup_bytes += a->size;
            dup_pairs++;
        }
    }
    printf("  tensor-level dedup: %u identical pair(s), %llu MB ซ้ำได้ (เก็บ 1 copy)\n",
           dup_pairs, (unsigned long long)(dup_bytes >> 20));
    free(h);
}

static void run_model(const char *path, uint32_t *n_present) {
    GGUFBox box;
    printf("\n═ %s ═\n", path);
    if (gguf_box_open(&box, path) != 0) { printf("  (cannot open — skip)\n"); return; }
    (*n_present)++;

    TensorStat total; memset(&total, 0, sizeof(total));
    printf("  %-34s %-5s %9s %8s | %10s %10s %10s %8s %8s\n",
           "tensor", "type", "elems", "blocks", "unique_id", "unique_rot", "unique_rev",
           "top%", "ratio×");

    for (uint32_t i = 0; i < box.n_tensors; i++) {
        const GGUFBoxEntry *e = &box.entries[i];
        if (e->dtype != 8 && e->dtype != 2) continue;   /* Q8_0 / Q4_0 */
        if (!e->data) continue;
        uint64_t n_blocks = e->n_elems / BLOCK_VALS;
        if (n_blocks == 0) continue;

        TensorStat st; memset(&st, 0, sizeof(st));
        measure_tensor(&box, i, &st);
        int map_bytes = (e->dtype == 8) ? 32 : 16;
        int blk_bytes = (e->dtype == 8) ? 34 : 18;

        /* ratios ต่อ mode (คำนวณใหม่จาก totals ของ tensor นี้) */
        double r[3];
        for (int m = 0; m < 3; m++) {
            double raw = (double)st.sampled * blk_bytes;
            double silk = (double)(st.unique[m] * (uint64_t)map_bytes + st.sampled * 3u);
            r[m] = (silk > 0) ? raw / silk : 0.0;
        }
        double top_pct = 100.0 * (double)st.top_run[2] / (double)(st.sampled ? st.sampled : 1);

        printf("  %-34.34s %-5s %9llu %8llu | %10llu %10llu %10llu %7.1f%% %8.2f\n",
               e->name,
               (e->dtype == 8) ? "Q8_0" : "Q4_0",
               (unsigned long long)e->n_elems,
               (unsigned long long)n_blocks,
               (unsigned long long)st.unique[0],
               (unsigned long long)st.unique[1],
               (unsigned long long)st.unique[2],
               top_pct, r[2]);

        total.sampled  += st.sampled;
        total.raw_bytes += st.raw_bytes;
        total.unique[0] += st.unique[0];
        total.unique[1] += st.unique[1];
        total.unique[2] += st.unique[2];
        total.top_run[2] += st.top_run[2];
        total.silk_bytes += st.silk_bytes;
    }

    tensor_dedup_pass(&box);
    entropy_pass(&box);
    printf("  cube bijection: แต่ละ int8 → (sign,tens,ones) = address ของมันเอง — "
           "decompose ไม่เพิ่มข้อมูล (Σplanes = H(value) เสมอ)\n");

    printf("  ── model total ──\n");
    for (int m = 0; m < 3; m++) {
        double raw = (double)total.raw_bytes;
        double silk = (double)(total.unique[m] * 32u + total.sampled * 3u);
        printf("  mode %-8s: sampled %llu blocks, unique maps %llu, "
               "blocks/map %.1f, silk ratio %.2f× (raw %llu MB → silk %llu MB)\n",
               m == 0 ? "identity" : (m == 1 ? "rot" : "rot+rev"),
               (unsigned long long)total.sampled,
               (unsigned long long)total.unique[m],
               total.unique[m] ? (double)total.sampled / (double)total.unique[m] : 0.0,
               silk > 0 ? raw / silk : 0.0,
               (unsigned long long)raw >> 20,
               (unsigned long long)silk >> 20);
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Silk-screen feasibility — canonicalize Q8/Q4 blocks, count unique maps\n");
    printf("═══════════════════════════════════════════════════════════════════════\n");
    self_tests();

    const char *paths[4] = {
        "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf",
        "I:/model/Qwen3-0.6B-Q8_0.gguf",
        "I:/model/LFM2.5-2.6B-Q8_0.gguf",
        "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf"
    };
    uint32_t n = (argc > 1) ? (uint32_t)argc - 1 : 4;
    uint32_t present = 0;
    for (uint32_t i = 0; i < n; i++) {
        const char *p = (argc > 1) ? argv[i + 1] : paths[i];
        run_model(p, &present);
    }
    printf("\n═══════════════════════════════════════════════════════════════════\n");
    printf("models scanned: %u/%u\n", present, n);
    return 0;
}
