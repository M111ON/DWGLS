/* geo_kv_real_bench.c — KV-cache workload จาก GGUF จริง (Qwen2.5-0.5B)
 *
 * อ่าน metadata จริงผ่าน gguf_reader.h:
 *   n_layers  = จำนวน blk.*.attn_k.weight
 *   n_kv_head = dims[0] / head_dim (head_dim มาตรฐาน 128)
 * จากนั้นจำลอง access pattern ของ llama.cpp:
 *   PREFILL : เขียน K/V block ที่ position ใหม่ทุก layer (seq pos × stride layer)
 *   DECODE  : attention อ่าน K/V ของทุก position เก่า (block 1KB, ตาม layer)
 * เทียบ MAP (geometric, fixed slot 1KB) vs CLASSIC (contiguous + fseek)
 *
 * BUILD/RUN:
 *   gcc -O2 -Wall -I. -Icore -o build/geo_kv_real_bench tools/geo_kv_real_bench.c -lm
 *   ./build/geo_kv_real_bench "I:\DWGLS\build\qwen05-direct.gguf"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "../core/gguf_reader.h"
#include "../core/infra/geo_dram_tile.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ns(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
}
#else
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
#endif

#define HEAD_DIM 128
#define N_CTX    2048
#define KV_BLOCK (2 * HEAD_DIM)          /* K or V block = n_kv_head × head_dim, fp16 */

static int    kv_n_layers;
static int    kv_n_kv_head;
static size_t kv_n_blocks;               /* layer × ctx  (K+V แยกกัน) */
static size_t kv_logical;
static uint8_t **blk_data;               /* source per block */
static size_t  *blk_sz;
static int     *blk_layer;

/* MAP: block k → geometric address (dense), slot = KV_BLOCK bytes */
static size_t  kv_slot_addr;
static uint8_t *kv_base;
static size_t  kv_window;
#if defined(_WIN32)
static HANDLE g_kf = INVALID_HANDLE_VALUE, g_km = NULL;
#else
static int g_kfd = -1;
#endif
#define KV_FILE "build/geo_kv.bin"

static int kv_map_create(void) {
#if defined(_WIN32)
    g_kf = CreateFileA(KV_FILE, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_kf == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz; sz.QuadPart = (LONGLONG)kv_window;
    SetFilePointerEx(g_kf, sz, NULL, FILE_BEGIN);
    SetEndOfFile(g_kf);
    g_km = CreateFileMappingA(g_kf, NULL, PAGE_READWRITE,
                              (DWORD)(kv_window >> 32), (DWORD)kv_window, NULL);
    if (!g_km) return -1;
    kv_base = (uint8_t*)MapViewOfFile(g_km, FILE_MAP_ALL_ACCESS, 0, 0, kv_window);
    return kv_base ? 0 : -1;
#else
    g_kfd = open(KV_FILE, O_RDWR | O_CREAT, 0644);
    if (g_kfd < 0) return -1;
    if (ftruncate(g_kfd, (off_t)kv_window) != 0) return -1;
    kv_base = (uint8_t*)mmap(NULL, kv_window, PROT_READ | PROT_WRITE, MAP_SHARED, g_kfd, 0);
    return (kv_base == MAP_FAILED) ? -1 : 0;
#endif
}
static void kv_map_close(void) {
#if defined(_WIN32)
    if (kv_base) UnmapViewOfFile(kv_base);
    if (g_km) CloseHandle(g_km);
    if (g_kf != INVALID_HANDLE_VALUE) CloseHandle(g_kf);
    kv_base = NULL; g_km = NULL; g_kf = INVALID_HANDLE_VALUE;
#else
    if (kv_base) munmap(kv_base, kv_window);
    if (g_kfd >= 0) close(g_kfd);
    kv_base = NULL; g_kfd = -1;
#endif
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: geo_kv_real_bench <model.gguf>\n"); return 1; }
    GgufReader gr;
    if (gguf_open(argv[1], &gr) != 0) { printf("gguf_open failed\n"); return 1; }

    kv_n_layers = 0;
    kv_n_kv_head = 0;
    for (uint32_t i = 0; i < gr.n_tensors; i++) {
        const char *n = gr.names[i];
        if (strstr(n, "blk.") && strstr(n, ".attn_k.weight")) {
            kv_n_layers++;
            if (kv_n_kv_head == 0 && gr.n_dims[i] >= 2) {
                /* attn_k: [n_kv_head×head_dim, n_embd] → dim0 / HEAD_DIM */
                kv_n_kv_head = (int)(gr.dims[(size_t)i * 4 + 0] / HEAD_DIM);
            }
        }
    }
    if (kv_n_layers == 0) { printf("no attn_k.weight tensors found\n"); return 1; }
    if (kv_n_kv_head == 0) kv_n_kv_head = 4;

    printf("REAL GGUF KV-cache: %s\n", argv[1]);
    printf("  layers=%d  n_kv_head=%d  head_dim=%d  n_ctx=%d\n",
           kv_n_layers, kv_n_kv_head, HEAD_DIM, N_CTX);
    printf("  KV block = %u B (K หรือ V 1 (layer,pos))\n", (unsigned)KV_BLOCK);

    kv_n_blocks = (size_t)kv_n_layers * N_CTX;          /* K blocks */
    kv_logical = kv_n_blocks * KV_BLOCK;
    printf("  K blocks = %zu, logical (K+V) = %.2f MB\n",
           kv_n_blocks, (double)kv_logical * 2 / 1e6);

    /* source data per K block (K กับ V ใช้ data เดียวกันเพื่อ save RAM) */
    blk_sz = (size_t*)malloc(kv_n_blocks * sizeof(size_t));
    blk_layer = (int*)malloc(kv_n_blocks * sizeof(int));
    blk_data = (uint8_t**)malloc(kv_n_blocks * sizeof(uint8_t*));
    for (size_t b = 0; b < kv_n_blocks; b++) {
        blk_sz[b] = KV_BLOCK;
        blk_layer[b] = (int)(b / N_CTX);
        blk_data[b] = (uint8_t*)malloc(KV_BLOCK);
        uint8_t *p = blk_data[b];
        for (size_t j = 0; j < KV_BLOCK; j++)
            p[j] = (uint8_t)((j * 2654435761u) >> 24) ^ (uint8_t)b;
    }

    /* MAP: dense geometric addresses 0..kv_n_blocks-1, slot = KV_BLOCK */
    kv_window = kv_n_blocks * 2 * KV_BLOCK;   /* K + V */
    kv_slot_addr = kv_n_blocks;
    printf("  MAP window = %.2f MB (%.0f%% of logical, budget <= 50%%)\n",
           (double)kv_window / 1e6, 100.0 * (double)kv_window / ((double)kv_logical * 2));

    uint8_t *scratch = (uint8_t*)malloc(KV_BLOCK);
    if (kv_map_create() != 0) { printf("kv_map_create failed\n"); return 1; }

    /* ── PREFILL write: pos 0..ctx-1, ทุก layer ตามลำดับ (K แล้ว V) ── */
    double t0 = now_ns();
    for (size_t pos = 0; pos < (size_t)N_CTX; pos++)
        for (int l = 0; l < kv_n_layers; l++) {
            size_t b = (size_t)l * N_CTX + pos;
            memcpy(kv_base + b * KV_BLOCK, blk_data[b], KV_BLOCK);           /* K */
            memcpy(kv_base + (kv_n_blocks + b) * KV_BLOCK, blk_data[b], KV_BLOCK); /* V */
        }
    double w_map = now_ns() - t0;
    printf("  PREFILL write: MAP %.2f GB/s (%8.0f ns/block)\n",
           (double)kv_logical * 2 / 1e9 / (w_map / 1e9), w_map / (kv_n_blocks * 2));

    /* ── DECODE read: attention อ่าน K/V ทั้งหมด (ทุก layer × ทุก pos) ── */
    t0 = now_ns();
    int bad = 0;
    for (int l = 0; l < kv_n_layers; l++)
        for (size_t pos = 0; pos < (size_t)N_CTX; pos++) {
            size_t b = (size_t)l * N_CTX + pos;
            if (memcmp(kv_base + b * KV_BLOCK, blk_data[b], KV_BLOCK) != 0) bad++;
            if (memcmp(kv_base + (kv_n_blocks + b) * KV_BLOCK, blk_data[b], KV_BLOCK) != 0) bad++;
        }
    double r_map = now_ns() - t0;
    printf("  DECODE read (zero-copy verify): MAP %.2f GB/s (%8.0f ns/block)  %s (%d bad)\n",
           (double)kv_logical * 2 / 1e9 / (r_map / 1e9), r_map / (kv_n_blocks * 2),
           bad == 0 ? "PASS" : "FAIL", bad);

    kv_map_close();

    /* ── CLASSIC: contiguous layout, index per (layer,pos) ── */
    size_t *cls_off = (size_t*)malloc(kv_n_blocks * 2 * sizeof(size_t));
    size_t cum = 0;
    for (size_t b = 0; b < kv_n_blocks; b++) {
        cls_off[b] = cum; cum += KV_BLOCK;              /* K */
        cls_off[kv_n_blocks + b] = cum; cum += KV_BLOCK; /* V */
    }
    FILE *f = fopen("build/geo_kv_cls.bin", "wb");
    t0 = now_ns();
    for (int l = 0; l < kv_n_layers; l++)
        for (size_t pos = 0; pos < (size_t)N_CTX; pos++) {
            size_t b = (size_t)l * N_CTX + pos;
            fwrite(blk_data[b], 1, KV_BLOCK, f);
            fwrite(blk_data[b], 1, KV_BLOCK, f);
        }
    double w_cls = now_ns() - t0;
    fclose(f);
    printf("  PREFILL write: CLASSIC %.2f GB/s (%8.0f ns/block)\n",
           (double)kv_logical * 2 / 1e9 / (w_cls / 1e9), w_cls / (kv_n_blocks * 2));

    f = fopen("build/geo_kv_cls.bin", "rb");
    bad = 0;
    t0 = now_ns();
    for (int l = 0; l < kv_n_layers; l++)
        for (size_t pos = 0; pos < (size_t)N_CTX; pos++) {
            size_t b = (size_t)l * N_CTX + pos;
            fseek(f, (long)cls_off[b], SEEK_SET);
            fread(scratch, 1, KV_BLOCK, f);
            if (memcmp(scratch, blk_data[b], KV_BLOCK) != 0) bad++;
            fseek(f, (long)cls_off[kv_n_blocks + b], SEEK_SET);
            fread(scratch, 1, KV_BLOCK, f);
            if (memcmp(scratch, blk_data[b], KV_BLOCK) != 0) bad++;
        }
    double r_cls = now_ns() - t0;
    fclose(f);
    printf("  DECODE read: CLASSIC %.2f GB/s (%8.0f ns/block)  %s (%d bad)\n",
           (double)kv_logical * 2 / 1e9 / (r_cls / 1e9), r_cls / (kv_n_blocks * 2),
           bad == 0 ? "PASS" : "FAIL", bad);

    printf("\n  write : MAP %.2fx vs CLASSIC\n", w_cls / w_map);
    printf("  read  : MAP %.2fx vs CLASSIC\n", r_cls / r_map);

    free(scratch);
    for (size_t b = 0; b < kv_n_blocks; b++) free(blk_data[b]);
    free(blk_data); free(blk_sz); free(blk_layer); free(cls_off);
    gguf_close(&gr);
    return 0;
}