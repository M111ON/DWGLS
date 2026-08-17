/* tools/geo_speed_bench.c — 3-way speed comparison
 *   MAP (geometric)  : dram_addr -> mmap offset, coordinate = address, no hash
 *   Classic file I/O : contiguous file + index array (traditional FS floor)
 *   RAM floor        : heap memcpy only (physical speed ceiling)
 * Constraint: MAP storage overhead must be <= 50% of logical bytes.
 * Integrity: every read pass is memcmp'd against source (rule: ratio<1.0
 *            must be proven by decode; here overhead% measured on real bytes).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
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
  #include <fcntl.h>
  #include <unistd.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  static double now_ns(void) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
  }
#endif

#define N_TENSORS   500
#define CHUNK_SZ    (512u * 1024u)
#define BIG_SZ      (512u * 1024u)
#define MID_SZ      (256u * 1024u)
#define BENCH_MAP   "build/geo_bench_map.bin"
#define BENCH_CLS   "build/geo_bench_classic.bin"

static size_t      tsz[N_TENSORS];
static uint8_t    *src[N_TENSORS];
static uint32_t    slot_addr[N_TENSORS];
static size_t      cls_off[N_TENSORS];
static size_t      ram_off[N_TENSORS];
static int         perm[N_TENSORS];
static size_t      total_logical;

static uint8_t    *g_map_base;
static size_t      g_window;
static size_t      g_used_slots;
#if defined(_WIN32)
static HANDLE      g_mf = INVALID_HANDLE_VALUE;
static HANDLE      g_mm = NULL;
#else
static int         g_fd = -1;
#endif

static void gen_slots(void) {
    int k = 0;
    uint32_t max_addr = 0;
    for (uint32_t a = 0; a < DRAM_ANCHORS && k < N_TENSORS; a++)
        for (uint32_t y = 0; y < DRAM_GRID_Y && k < N_TENSORS; y++)
            for (uint32_t x = 0; x < DRAM_GRID_X && k < N_TENSORS; x++)
                for (uint32_t l = 0; l < DRAM_LAYERS && k < N_TENSORS; l++) {
                    slot_addr[k] = dram_addr(a, x, y, l);
                    if (slot_addr[k] > max_addr) max_addr = slot_addr[k];
                    k++;
                }
    g_used_slots = (size_t)max_addr + 1;
}

static void gen_perm(void) {
    for (int i = 0; i < N_TENSORS; i++) perm[i] = i;
    uint32_t s = 12345u;
    for (int i = N_TENSORS - 1; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        int j = (int)((s >> 8) % (uint32_t)(i + 1));
        int t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
}

static void gen_workload(void) {
    total_logical = 0;
    for (int i = 0; i < N_TENSORS; i++) {
        tsz[i] = (i < 400) ? BIG_SZ : MID_SZ;
        total_logical += tsz[i];
    }
    for (int i = 0; i < N_TENSORS; i++) {
        src[i] = (uint8_t*)malloc(tsz[i]);
        uint32_t kx = (uint32_t)(i * 2654435761u);
        uint8_t *p = src[i];
        for (size_t j = 0; j < tsz[i]; j++)
            p[j] = (uint8_t)((j * 2654435761u) >> 24) ^ (uint8_t)kx;
    }
}

static int map_create(const char *path) {
    g_window = g_used_slots * CHUNK_SZ;
#if defined(_WIN32)
    g_mf = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_mf == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz; sz.QuadPart = (LONGLONG)g_window;
    SetFilePointerEx(g_mf, sz, NULL, FILE_BEGIN);
    SetEndOfFile(g_mf);
    g_mm = CreateFileMappingA(g_mf, NULL, PAGE_READWRITE,
                              (DWORD)(g_window >> 32), (DWORD)g_window, NULL);
    if (!g_mm) return -1;
    g_map_base = (uint8_t*)MapViewOfFile(g_mm, FILE_MAP_ALL_ACCESS, 0, 0, g_window);
    return g_map_base ? 0 : -1;
#else
    g_fd = open(path, O_RDWR | O_CREAT, 0644);
    if (g_fd < 0) return -1;
    if (ftruncate(g_fd, (off_t)g_window) != 0) return -1;
    g_map_base = (uint8_t*)mmap(NULL, g_window, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    return (g_map_base == MAP_FAILED) ? -1 : 0;
#endif
}

static void map_close(void) {
#if defined(_WIN32)
    if (g_map_base) UnmapViewOfFile(g_map_base);
    if (g_mm) CloseHandle(g_mm);
    if (g_mf != INVALID_HANDLE_VALUE) CloseHandle(g_mf);
    g_map_base = NULL; g_mm = NULL; g_mf = INVALID_HANDLE_VALUE;
#else
    if (g_map_base) munmap(g_map_base, g_window);
    if (g_fd >= 0) close(g_fd);
    g_map_base = NULL; g_fd = -1;
#endif
}

static void map_flush_core(void) {
#if defined(_WIN32)
    FlushViewOfFile(g_map_base, g_window);
#else
    msync(g_map_base, g_window, MS_SYNC);
#endif
}

static double map_write(void) {
    double t0 = now_ns();
    for (int i = 0; i < N_TENSORS; i++)
        memcpy(g_map_base + (size_t)slot_addr[i] * CHUNK_SZ, src[i], tsz[i]);
    return now_ns() - t0;
}

static double map_flush(void) {
    double t0 = now_ns();
    map_flush_core();
    return now_ns() - t0;
}

static double map_read(int use_perm, uint8_t *scratch, int *bad) {
    double t0 = now_ns(); *bad = 0;
    for (int k = 0; k < N_TENSORS; k++) {
        int i = use_perm ? perm[k] : k;
        const uint8_t *p = g_map_base + (size_t)slot_addr[i] * CHUNK_SZ;
        memcpy(scratch, p, tsz[i]);
        if (memcmp(scratch, src[i], tsz[i]) != 0) (*bad)++;
    }
    return now_ns() - t0;
}

static double cls_read(int use_perm, uint8_t *scratch, int *bad) {
    FILE *f = fopen(BENCH_CLS, "rb");
    if (!f) return -1;
    double t0 = now_ns(); *bad = 0;
    for (int k = 0; k < N_TENSORS; k++) {
        int i = use_perm ? perm[k] : k;
        fseek(f, (long)cls_off[i], SEEK_SET);
        fread(scratch, 1, tsz[i], f);
        if (memcmp(scratch, src[i], tsz[i]) != 0) (*bad)++;
    }
    double dt = now_ns() - t0;
    fclose(f);
    return dt;
}

static void file_size_info(const char *path, uint64_t *logical, uint64_t *alloc) {
    *logical = 0; *alloc = 0;
#if defined(_WIN32)
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz)) *logical = (uint64_t)sz.QuadPart;
    DWORD hi = 0;
    DWORD lo = GetCompressedFileSizeA(path, &hi);
    if (lo != INVALID_FILE_SIZE) *alloc = ((uint64_t)hi << 32) | lo;
    CloseHandle(h);
#else
    struct stat st;
    if (stat(path, &st) == 0) { *logical = (uint64_t)st.st_size; *alloc = (uint64_t)st.st_blocks * 512; }
#endif
}

static void print_row(const char *path, const char *pattern, const char *temp,
                      double ns, int bad) {
    double gb = (double)total_logical / 1e9;
    double sec = ns / 1e9;
    printf("  %-12s %-7s %-6s %10.2f GB/s  %8.0f ns/op  %s (%d bad)\n",
           path, pattern, temp, sec > 0 ? gb / sec : 0.0,
           ns / N_TENSORS, bad == 0 ? "PASS" : "FAIL", bad);
}

/* ── TINY random-access workload (small objects, the per-op-overhead regime) ── */
#define TINY_N      15000
#define TINY_SLOT   160u
#define TINY_FILE   "build/geo_bench_tiny.bin"
#define TINY_CLS    "build/geo_bench_tiny_cls.bin"

static uint16_t tsz_tiny[TINY_N];
static uint8_t  *src_tiny[TINY_N];
static uint32_t  slot_tiny[TINY_N];
static size_t    cls_tiny[TINY_N];
static int       perm_tiny[TINY_N];
static uint8_t  *g_tiny_base;
static size_t    g_tiny_window;
static size_t    tiny_logical;
#if defined(_WIN32)
static HANDLE g_tf = INVALID_HANDLE_VALUE, g_tm = NULL;
#else
static int    g_tfd = -1;
#endif

static void gen_tiny(void) {
    tiny_logical = 0;
    for (int i = 0; i < TINY_N; i++) {
        tsz_tiny[i] = (uint16_t)(96 + (i * 37) % 65);   /* 96..160, avg ~128 */
        tiny_logical += tsz_tiny[i];
        src_tiny[i] = (uint8_t*)malloc(tsz_tiny[i]);
        uint8_t *p = src_tiny[i];
        uint32_t kx = (uint32_t)(i * 2654435761u);
        for (size_t j = 0; j < tsz_tiny[i]; j++)
            p[j] = (uint8_t)((j * 2654435761u) >> 24) ^ (uint8_t)kx;
    }
    int k = 0;
    uint32_t tiny_max = 0;
    for (uint32_t a = 0; a < DRAM_ANCHORS && k < TINY_N; a++)
        for (uint32_t y = 0; y < DRAM_GRID_Y && k < TINY_N; y++)
            for (uint32_t x = 0; x < DRAM_GRID_X && k < TINY_N; x++)
                for (uint32_t l = 0; l < DRAM_LAYERS && k < TINY_N; l++) {
                    slot_tiny[k] = dram_addr(a, x, y, l);
                    if (slot_tiny[k] > tiny_max) tiny_max = slot_tiny[k];
                    k++;
                }
    g_tiny_window = ((size_t)tiny_max + 1) * TINY_SLOT;
    size_t cum = 0;
    for (int i = 0; i < TINY_N; i++) { cls_tiny[i] = cum; cum += tsz_tiny[i]; }
    for (int i = 0; i < TINY_N; i++) perm_tiny[i] = i;
    uint32_t s = 987654321u;
    for (int i = TINY_N - 1; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        int j = (int)((s >> 8) % (uint32_t)(i + 1));
        int t = perm_tiny[i]; perm_tiny[i] = perm_tiny[j]; perm_tiny[j] = t;
    }
}

static int tiny_map_create(void) {
#if defined(_WIN32)
    g_tf = CreateFileA(TINY_FILE, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_tf == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz; sz.QuadPart = (LONGLONG)g_tiny_window;
    SetFilePointerEx(g_tf, sz, NULL, FILE_BEGIN);
    SetEndOfFile(g_tf);
    g_tm = CreateFileMappingA(g_tf, NULL, PAGE_READWRITE,
                              (DWORD)(g_tiny_window >> 32), (DWORD)g_tiny_window, NULL);
    if (!g_tm) return -1;
    g_tiny_base = (uint8_t*)MapViewOfFile(g_tm, FILE_MAP_ALL_ACCESS, 0, 0, g_tiny_window);
    return g_tiny_base ? 0 : -1;
#else
    g_tfd = open(TINY_FILE, O_RDWR | O_CREAT, 0644);
    if (g_tfd < 0) return -1;
    if (ftruncate(g_tfd, (off_t)g_tiny_window) != 0) return -1;
    g_tiny_base = (uint8_t*)mmap(NULL, g_tiny_window, PROT_READ | PROT_WRITE, MAP_SHARED, g_tfd, 0);
    return (g_tiny_base == MAP_FAILED) ? -1 : 0;
#endif
}

static void tiny_map_close(void) {
#if defined(_WIN32)
    if (g_tiny_base) UnmapViewOfFile(g_tiny_base);
    if (g_tm) CloseHandle(g_tm);
    if (g_tf != INVALID_HANDLE_VALUE) CloseHandle(g_tf);
    g_tiny_base = NULL; g_tm = NULL; g_tf = INVALID_HANDLE_VALUE;
#else
    if (g_tiny_base) munmap(g_tiny_base, g_tiny_window);
    if (g_tfd >= 0) close(g_tfd);
    g_tiny_base = NULL; g_tfd = -1;
#endif
}

/* zero-copy read: in-place verify on mapped memory (no intermediate copy) */
static double tiny_read_map_zc(int use_perm, int *bad) {
    double t0 = now_ns(); *bad = 0;
    for (int k = 0; k < TINY_N; k++) {
        int i = use_perm ? perm_tiny[k] : k;
        const uint8_t *p = g_tiny_base + (size_t)slot_tiny[i] * TINY_SLOT;
        if (memcmp(p, src_tiny[i], tsz_tiny[i]) != 0) (*bad)++;
    }
    return now_ns() - t0;
}

/* copy read: MAP with intermediate copy (like CLASSIC must do) */
static double tiny_read_map_cp(int use_perm, uint8_t *scratch, int *bad) {
    double t0 = now_ns(); *bad = 0;
    for (int k = 0; k < TINY_N; k++) {
        int i = use_perm ? perm_tiny[k] : k;
        const uint8_t *p = g_tiny_base + (size_t)slot_tiny[i] * TINY_SLOT;
        memcpy(scratch, p, tsz_tiny[i]);
        if (memcmp(scratch, src_tiny[i], tsz_tiny[i]) != 0) (*bad)++;
    }
    return now_ns() - t0;
}

static double tiny_read_classic(int use_perm, uint8_t *scratch, int *bad) {
    FILE *f = fopen(TINY_CLS, "rb");
    if (!f) return -1;
    double t0 = now_ns(); *bad = 0;
    for (int k = 0; k < TINY_N; k++) {
        int i = use_perm ? perm_tiny[k] : k;
        fseek(f, (long)cls_tiny[i], SEEK_SET);
        fread(scratch, 1, tsz_tiny[i], f);
        if (memcmp(scratch, src_tiny[i], tsz_tiny[i]) != 0) (*bad)++;
    }
    double dt = now_ns() - t0;
    fclose(f);
    return dt;
}

static void bench_tiny(void) {
    gen_tiny();
    printf("\nTINY RANDOM-ACCESS (%d objects, %d..%d B, avg ~%.0f B, logical %.2f MB)\n",
           TINY_N, 96, 160, (double)tiny_logical / TINY_N, (double)tiny_logical / 1e6);
    printf("MAP window = %.2f MB (%.0f%% of logical, budget <= 50%%)\n",
           (double)g_tiny_window / 1e6, 100.0 * (double)g_tiny_window / (double)tiny_logical);

    uint8_t *scratch = (uint8_t*)malloc(TINY_SLOT);
    if (tiny_map_create() != 0) { printf("tiny_map_create failed\n"); fflush(stdout); return; }

    double t0 = now_ns();
    for (int i = 0; i < TINY_N; i++)
        memcpy(g_tiny_base + (size_t)slot_tiny[i] * TINY_SLOT, src_tiny[i], tsz_tiny[i]);
    double w_map = now_ns() - t0;
    printf("  write: MAP %.2f GB/s (%8.0f ns/op)\n",
           (double)tiny_logical / 1e9 / (w_map / 1e9), w_map / TINY_N);

    tiny_map_close();
    FILE *f = fopen(TINY_CLS, "wb");
    t0 = now_ns();
    for (int i = 0; i < TINY_N; i++)
        fwrite(src_tiny[i], 1, tsz_tiny[i], f);
    double w_cls = now_ns() - t0;
    fclose(f);
    printf("  write: CLASSIC %.2f GB/s (%8.0f ns/op)\n",
           (double)tiny_logical / 1e9 / (w_cls / 1e9), w_cls / TINY_N);

    printf("  read (random order):\n");
    tiny_map_create();
    {
        int b;
        double ns = tiny_read_map_zc(1, &b);
        printf("    MAP  zero-copy : %.2f GB/s  %6.0f ns/op  %s (%d bad)\n",
               (double)tiny_logical / 1e9 / (ns / 1e9), ns / TINY_N,
               b == 0 ? "PASS" : "FAIL", b);
        ns = tiny_read_map_cp(1, scratch, &b);
        printf("    MAP  copy      : %.2f GB/s  %6.0f ns/op  %s (%d bad)\n",
               (double)tiny_logical / 1e9 / (ns / 1e9), ns / TINY_N,
               b == 0 ? "PASS" : "FAIL", b);
    }
    tiny_map_close();
    {
        int b = 0;
        double ns = tiny_read_classic(1, scratch, &b);
        printf("    CLASSIC        : %.2f GB/s  %6.0f ns/op  %s (%d bad)\n",
               (double)tiny_logical / 1e9 / (ns / 1e9), ns / TINY_N,
               b == 0 ? "PASS" : "FAIL", b);
    }

    uint64_t clog, call;
    file_size_info(TINY_CLS, &clog, &call);
    printf("  CLASSIC overhead = %.1f%%\n",
           tiny_logical ? 100.0 * ((double)clog / (double)tiny_logical - 1.0) : 0.0);
    free(scratch);
    for (int i = 0; i < TINY_N; i++) free(src_tiny[i]);
}

/* ── KV-cache-like workload: mixed sizes, size-classed slots, random access ── */
#define KV_N       100000
#define KV_CLASSES 7
static const size_t kv_class_sz[KV_CLASSES] = { 64, 128, 256, 512, 1024, 2048, 4096 };

static size_t kv_tsz[KV_N];
static uint8_t *kv_src[KV_N];
static int    kv_class[KV_N];
static size_t kv_slot[KV_N];      /* slot index within its class */
static size_t kv_cls_off[KV_N];
static int    kv_perm[KV_N];
static uint8_t *kv_base;
static size_t kv_window;
static size_t kv_class_base[KV_CLASSES];
static size_t kv_class_count[KV_CLASSES];
static size_t kv_logical;
#if defined(_WIN32)
static HANDLE g_kf = INVALID_HANDLE_VALUE, g_km = NULL;
#else
static int    g_kfd = -1;
#endif
#define KV_FILE "build/geo_bench_kv.bin"
#define KV_CLS  "build/geo_bench_kv_cls.bin"

static void gen_kv(void) {
    kv_logical = 0;
    for (int i = 0; i < KV_N; i++) {
        uint32_t s = (uint32_t)(i * 2654435761u) % 100;
        if      (s < 30) kv_tsz[i] = 64;
        else if (s < 55) kv_tsz[i] = 128;
        else if (s < 75) kv_tsz[i] = 256;
        else if (s < 85) kv_tsz[i] = 512;
        else if (s < 93) kv_tsz[i] = 1024;
        else if (s < 98) kv_tsz[i] = 2048;
        else             kv_tsz[i] = 4096;
        kv_logical += kv_tsz[i];
        kv_src[i] = (uint8_t*)malloc(kv_tsz[i]);
        uint8_t *p = kv_src[i];
        uint32_t kx = (uint32_t)(i * 2654435761u);
        for (size_t j = 0; j < kv_tsz[i]; j++)
            p[j] = (uint8_t)((j * 2654435761u) >> 24) ^ (uint8_t)kx;
    }
    for (int c = 0; c < KV_CLASSES; c++) kv_class_count[c] = 0;
    for (int i = 0; i < KV_N; i++) {
        int c = 0;
        while (c < KV_CLASSES - 1 && kv_tsz[i] > kv_class_sz[c]) c++;
        kv_class[i] = c;
        kv_slot[i] = kv_class_count[c]++;
    }
    kv_window = 0;
    for (int c = 0; c < KV_CLASSES; c++) {
        kv_class_base[c] = kv_window;
        kv_window += kv_class_count[c] * kv_class_sz[c];
    }
    size_t cum = 0;
    for (int i = 0; i < KV_N; i++) { kv_cls_off[i] = cum; cum += kv_tsz[i]; }
    for (int i = 0; i < KV_N; i++) kv_perm[i] = i;
    uint32_t s2 = 555u;
    for (int i = KV_N - 1; i > 0; i--) {
        s2 = s2 * 1664525u + 1013904223u;
        int j = (int)((s2 >> 8) % (uint32_t)(i + 1));
        int t = kv_perm[i]; kv_perm[i] = kv_perm[j]; kv_perm[j] = t;
    }
}

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

static uint8_t *kv_ptr(int i) {
    return kv_base + kv_class_base[kv_class[i]] + kv_slot[i] * kv_class_sz[kv_class[i]];
}

static void bench_kv(void) {
    gen_kv();
    printf("\nKV-CACHE-LIKE (%d objects, 64B..4KB mixed, logical %.2f MB)\n",
           KV_N, (double)kv_logical / 1e6);
    printf("MAP size-classed window = %.2f MB (%.0f%% of logical, budget <= 50%%)\n",
           (double)kv_window / 1e6, 100.0 * (double)kv_window / (double)kv_logical);

    uint8_t *scratch = (uint8_t*)malloc(4096);
    if (kv_map_create() != 0) { printf("kv_map_create failed\n"); fflush(stdout); return; }

    double t0 = now_ns();
    for (int i = 0; i < KV_N; i++)
        memcpy(kv_ptr(i), kv_src[i], kv_tsz[i]);
    double w_map = now_ns() - t0;
    printf("  write: MAP %.2f GB/s (%8.0f ns/op)\n",
           (double)kv_logical / 1e9 / (w_map / 1e9), w_map / KV_N);

    kv_map_close();
    FILE *f = fopen(KV_CLS, "wb");
    t0 = now_ns();
    for (int i = 0; i < KV_N; i++)
        fwrite(kv_src[i], 1, kv_tsz[i], f);
    double w_cls = now_ns() - t0;
    fclose(f);
    printf("  write: CLASSIC %.2f GB/s (%8.0f ns/op)\n",
           (double)kv_logical / 1e9 / (w_cls / 1e9), w_cls / KV_N);

    printf("  read (random order):\n");
    kv_map_create();
    {
        int b;
        double ns;
        t0 = now_ns(); b = 0;
        for (int k = 0; k < KV_N; k++) {
            int i = kv_perm[k];
            if (memcmp(kv_ptr(i), kv_src[i], kv_tsz[i]) != 0) b++;
        }
        ns = now_ns() - t0;
        printf("    MAP  zero-copy : %.2f GB/s  %6.0f ns/op  %s (%d bad)\n",
               (double)kv_logical / 1e9 / (ns / 1e9), ns / KV_N,
               b == 0 ? "PASS" : "FAIL", b);
        t0 = now_ns(); b = 0;
        for (int k = 0; k < KV_N; k++) {
            int i = kv_perm[k];
            memcpy(scratch, kv_ptr(i), kv_tsz[i]);
            if (memcmp(scratch, kv_src[i], kv_tsz[i]) != 0) b++;
        }
        ns = now_ns() - t0;
        printf("    MAP  copy      : %.2f GB/s  %6.0f ns/op  %s (%d bad)\n",
               (double)kv_logical / 1e9 / (ns / 1e9), ns / KV_N,
               b == 0 ? "PASS" : "FAIL", b);
    }
    kv_map_close();
    {
        FILE *fc = fopen(KV_CLS, "rb");
        int b = 0;
        double t0 = now_ns();
        for (int k = 0; k < KV_N; k++) {
            int i = kv_perm[k];
            fseek(fc, (long)kv_cls_off[i], SEEK_SET);
            fread(scratch, 1, kv_tsz[i], fc);
            if (memcmp(scratch, kv_src[i], kv_tsz[i]) != 0) b++;
        }
        double ns = now_ns() - t0;
        fclose(fc);
        printf("    CLASSIC        : %.2f GB/s  %6.0f ns/op  %s (%d bad)\n",
               (double)kv_logical / 1e9 / (ns / 1e9), ns / KV_N,
               b == 0 ? "PASS" : "FAIL", b);
    }
    uint64_t clog, call;
    file_size_info(KV_CLS, &clog, &call);
    printf("  CLASSIC overhead = %.1f%%\n",
           kv_logical ? 100.0 * ((double)clog / (double)kv_logical - 1.0) : 0.0);
    free(scratch);
    for (int i = 0; i < KV_N; i++) free(kv_src[i]);
}

int main(void) {
    gen_workload();
    gen_slots();
    gen_perm();

    size_t cls_cum = 0, ram_cum = 0;
    for (int i = 0; i < N_TENSORS; i++) {
        cls_off[i] = cls_cum; cls_cum += tsz[i];
        ram_off[i] = ram_cum; ram_cum += tsz[i];
    }
    uint8_t *ram_base = (uint8_t*)malloc(total_logical);
    uint8_t *scratch  = (uint8_t*)malloc(BIG_SZ);

    printf("DWGLS geometric FS speed benchmark — 3 paths, %d tensors, %.2f MB logical\n",
           N_TENSORS, (double)total_logical / 1e6);
    printf("MAP slot = CHUNK_SZ %u, window = %.2f MB (%.0f%% of logical, budget <= 50%%)\n\n",
           CHUNK_SZ, (double)(g_used_slots * CHUNK_SZ) / 1e6,
           100.0 * (double)(g_used_slots * CHUNK_SZ) / (double)total_logical);

    printf("WRITE (cache = เข้าหน้า cache / flush = sync ลงดิสก์)\n");
    printf("  %-12s %-7s %-6s %10s  %8s  %s\n", "path", "pattern", "temp", "GB/s", "ns/op", "integrity");
    double w_map_c, w_map_f, w_cls_c, w_cls_f, w_ram_c;
    int bad;

    if (map_create(BENCH_MAP) != 0) { printf("map_create failed\n"); fflush(stdout); return 1; }
    w_map_c = map_write();
    w_map_f = map_flush();
    map_close();
    print_row("MAP", "seq", "cache", w_map_c, 0);
    print_row("MAP", "seq", "flush", w_map_f, 0);

    FILE *f = fopen(BENCH_CLS, "wb");
    if (!f) return 1;
    double t0 = now_ns();
    for (int i = 0; i < N_TENSORS; i++)
        fwrite(src[i], 1, tsz[i], f);
    w_cls_c = now_ns() - t0;
    t0 = now_ns();
    fflush(f);
    fclose(f);
#if defined(_WIN32)
    {
        HANDLE h = CreateFileA(BENCH_CLS, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { FlushFileBuffers(h); CloseHandle(h); }
    }
#else
    {
        int fd = open(BENCH_CLS, O_RDWR);
        if (fd >= 0) { fsync(fd); close(fd); }
    }
#endif
    w_cls_f = now_ns() - t0;
    print_row("CLASSIC", "seq", "cache", w_cls_c, 0);
    print_row("CLASSIC", "seq", "flush", w_cls_f, 0);

    t0 = now_ns();
    for (int i = 0; i < N_TENSORS; i++)
        memcpy(ram_base + ram_off[i], src[i], tsz[i]);
    w_ram_c = now_ns() - t0;
    print_row("RAM", "seq", "cache", w_ram_c, 0);

    printf("\nREAD (cold = fresh mapping/open, warm = second pass)\n");
    printf("  %-12s %-7s %-6s %10s  %8s  %s\n", "path", "pattern", "temp", "GB/s", "ns/op", "integrity");

    map_create(BENCH_MAP);
    {
        double ns; int b;
        ns = map_read(0, scratch, &b); print_row("MAP", "seq", "cold", ns, b);
        ns = map_read(0, scratch, &b); print_row("MAP", "seq", "warm", ns, b);
    }
    map_close();
    map_create(BENCH_MAP);
    {
        double ns; int b;
        ns = map_read(1, scratch, &b); print_row("MAP", "rand", "cold", ns, b);
        ns = map_read(1, scratch, &b); print_row("MAP", "rand", "warm", ns, b);
    }
    map_close();

    {
        double ns; int b;
        ns = cls_read(0, scratch, &b); print_row("CLASSIC", "seq", "cold", ns, b);
        ns = cls_read(0, scratch, &b); print_row("CLASSIC", "seq", "warm", ns, b);
        ns = cls_read(1, scratch, &b); print_row("CLASSIC", "rand", "cold", ns, b);
        ns = cls_read(1, scratch, &b); print_row("CLASSIC", "rand", "warm", ns, b);
    }

    t0 = now_ns();
    bad = 0;
    for (int k = 0; k < N_TENSORS; k++) {
        int i = perm[k];
        memcpy(scratch, ram_base + ram_off[i], tsz[i]);
        if (memcmp(scratch, src[i], tsz[i]) != 0) bad++;
    }
    print_row("RAM", "rand", "warm", now_ns() - t0, bad);

    uint64_t mlog, mall, clog, call;
    file_size_info(BENCH_MAP, &mlog, &mall);
    file_size_info(BENCH_CLS, &clog, &call);

    printf("\nSTORAGE OVERHEAD (measured on real bytes)\n");
    printf("  MAP     : file=%.2f MB  alloc=%.2f MB  logical=%.2f MB  overhead=%.1f%%  (budget 50%%)\n",
           (double)mlog / 1e6, (double)mall / 1e6, (double)total_logical / 1e6,
           total_logical ? 100.0 * ((double)mlog / (double)total_logical - 1.0) : 0.0);
    printf("  CLASSIC : file=%.2f MB  alloc=%.2f MB  overhead=%.1f%%\n",
           (double)clog / 1e6, (double)call / 1e6,
           total_logical ? 100.0 * ((double)clog / (double)total_logical - 1.0) : 0.0);

    printf("\nVERDICT\n");
    double mapw = (w_map_c + w_map_f) / N_TENSORS, clsw = (w_cls_c + w_cls_f) / N_TENSORS;
    printf("  write total (cache+flush): MAP %.0f ns/t vs CLASSIC %.0f ns/t -> %.2fx\n", mapw, clsw,
           clsw > 0 ? mapw / clsw : 0);
    map_create(BENCH_MAP);
    double m_cold = map_read(1, scratch, &bad) / N_TENSORS;
    double m_warm = map_read(1, scratch, &bad) / N_TENSORS;
    map_close();
    double c_cold = cls_read(1, scratch, &bad) / N_TENSORS;
    double c_warm = cls_read(1, scratch, &bad) / N_TENSORS;

    printf("  rand read cold: MAP %.0f ns/t vs CLASSIC %.0f ns/t -> %.2fx\n",
           m_cold, c_cold, c_cold > 0 ? m_cold / c_cold : 0);
    printf("  rand read warm: MAP %.0f ns/t vs CLASSIC %.0f ns/t -> %.2fx\n",
           m_warm, c_warm, c_warm > 0 ? m_warm / c_warm : 0);

    free(ram_base); free(scratch);
    for (int i = 0; i < N_TENSORS; i++) free(src[i]);

    bench_tiny();
    bench_kv();
    return 0;
}