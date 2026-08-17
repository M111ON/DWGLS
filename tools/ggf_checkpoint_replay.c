/*
 * ggf_checkpoint_replay.c — checkpoint/replay ของ .ggf storage
 * ═══════════════════════════════════════════════════════════════════════
 *
 * T1.2n — เชื่อม write/read path ใหม่ (ggf_save_map + walk clock + mmap)
 * เข้ากับ checkpoint/replay (เหมือน fibo_checkpoint_sweep — §15.76):
 *
 *   checkpoint: GGUF จริง → dedup registry (tied_dedup_scan) → save เฉพาะ
 *               home .ggf ผ่าน ggf_save_map (mmap view) + manifest (.mfp —
 *               เก็บแค่ seed+method: seed/ticks/cycles/n/sizes/home_of/names)
 *               → spawn ตัวเองเป็น FRESH PROCESS
 *   replay    : process ใหม่ (memory ว่าง) อ่าน manifest จากดิสก์ →
 *               rebuild walk clock (state = seed/round/tick) → resolve ทุก
 *               tensor → อ่านผ่าน GGFMap (zero-copy) → เทียบกับ GGUF จริง
 *               (reopen จากไฟล์) → lossless byte-for-byte
 *
 * BUILD: make ggf_ckpt
 * RUN:   ./build/ggf_checkpoint_replay <model.gguf> --ckpt-dir build/ckpt_ggf
 *        (spawn --replay <manifest> <model> <dir> เอง)
 *        ./build/ggf_checkpoint_replay --replay <manifest> <model> <dir>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <process.h>
#include <sys/stat.h>
#include "../core/gguf_box.h"
#include "../core/geo_ggf_ckpt.h"
#include "../core/tied_dedup.h"

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void ensure_dir(const char *dir)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *s = tmp + 1; *s; s++) {
        if (*s == '/' || *s == '\\') { *s = '\0'; mkdir(tmp); *s = '/'; }
    }
    mkdir(tmp);
}

/* ── replay callback: เทียบ bytes จาก .ggf กับ tensor จริงใน GGUF (reopen) ── */
typedef struct {
    GGUFBox *box;
    const GGUFBoxEntry **by_idx;   /* [n] — entry per tensor index */
} CmpCtx;

static int cmp_vs_gguf(void *ctx, uint32_t idx, const uint8_t *got, uint64_t got_n)
{
    CmpCtx *c = (CmpCtx *)ctx;
    const GGUFBoxEntry *e = c->by_idx[idx];
    if (!e || !e->data || e->size != got_n) return 0;
    if (memcmp(got, e->data, got_n) == 0) return 1;
    (void)c;
    return 0;
}

/* ═══ replay — fresh process: manifest + .ggf + GGUF จากดิสก์ ═══ */
static int replay_mode(const char *manifest, const char *model, const char *dir)
{
    GgfCkptHeader h;
    GgfCkptEntry *entries = NULL;
    if (ggf_ckpt_read(manifest, &h, &entries) != 0) {
        printf("  [REPLAY FAIL] manifest unreadable: %s\n", manifest);
        return 1;
    }
    /* paths derive จาก names (deterministic) */
    const char **paths = (const char **)malloc(h.n * sizeof(char *));
    char *path_buf = (char *)malloc(h.n * 256);
    for (uint32_t i = 0; i < h.n; i++) {
        ggf_ckpt_path(path_buf + i * 256, 256, dir, i, entries[i].name);
        paths[i] = path_buf + i * 256;
    }

    /* reopen GGUF ใน process ใหม่ (memory ว่าง — ข้อมูลมาจากดิสก์เท่านั้น) */
    GGUFBox box;
    memset(&box, 0, sizeof box);
    if (gguf_box_open(&box, model) != 0) {
        printf("  [REPLAY FAIL] cannot open model: %s\n", model);
        free(entries); free(paths); free(path_buf);
        return 1;
    }
    const GGUFBoxEntry **by_idx = (const GGUFBoxEntry **)calloc(h.n, sizeof(void *));
    for (uint32_t i = 0; i < box.n_tensors && i < h.n; i++)
        by_idx[i] = &box.entries[i];
    CmpCtx ctx = { &box, by_idx };

    double t0 = now_sec();
    uint64_t bytes = 0;
    uint32_t ok = 0, fail = 0;
    int rc = ggf_ckpt_replay(&h, entries, paths, cmp_vs_gguf, &ctx,
                             &bytes, &ok, &fail);
    double dt = now_sec() - t0;

    printf("  [REPLAY %s] %s — %u/%u tensors lossless (fresh process จากดิสก์) · "
           "%llu B · %.1f MB/s · dedup saved %llu B (%.1f MB)\n",
           rc == 0 ? "PASS" : "FAIL", manifest, ok, h.n,
           (unsigned long long)bytes,
           bytes / (1024.0 * 1024.0) / (dt ? dt : 1e-9),
           (unsigned long long)h.dup_bytes, h.dup_bytes / (1024.0 * 1024.0));

    gguf_box_close(&box);
    free(by_idx); free(entries); free(paths); free(path_buf);
    return rc == 0 ? 0 : 1;
}

/* ═══ checkpoint — save home .ggf + manifest → spawn fresh process ═══ */
static int checkpoint_mode(const char *model, const char *dir)
{
    ensure_dir(dir);
    GGUFBox box;
    memset(&box, 0, sizeof box);
    if (gguf_box_open(&box, model) != 0) {
        printf("  [CKPT FAIL] cannot open model: %s\n", model);
        return 1;
    }
    uint32_t n = box.n_tensors;
    if (n > GGF_CKPT_MAX_T) n = GGF_CKPT_MAX_T;

    /* registry + arrays */
    const uint8_t **ptrs = (const uint8_t **)malloc(n * sizeof(void *));
    uint32_t *sizes = (uint32_t *)malloc(n * sizeof(uint32_t));
    int32_t  *home_of = (int32_t *)malloc(n * sizeof(int32_t));
    const char **names = (const char **)malloc(n * sizeof(char *));
    for (uint32_t i = 0; i < n; i++) {
        ptrs[i] = box.entries[i].data;
        sizes[i] = box.entries[i].size;
        names[i] = box.entries[i].name;
    }
    uint64_t dup_bytes = tied_dedup_scan(ptrs, sizes, n, home_of);

    /* save home .ggf ผ่าน ggf_save_map (dedup ระดับไฟล์) */
    char *path_buf = (char *)malloc(n * 256);
    uint32_t n_save = 0, n_save_fail = 0;
    double t0 = now_sec();
    for (uint32_t i = 0; i < n; i++) {
        if (home_of[i] != (int32_t)i) continue;   /* dup — ไม่มีไฟล์ */
        if (sizes[i] == 0) continue;
        ggf_ckpt_path(path_buf + i * 256, 256, dir, i, names[i]);
        if (ggf_save_map(ptrs[i], sizes[i], 8, path_buf + i * 256) != 0)
            n_save_fail++;
        else
            n_save++;
    }
    double dt = now_sec() - t0;

    /* manifest */
    uint64_t data_bytes = 0;
    for (uint32_t i = 0; i < n; i++) data_bytes += sizes[i];
    char mfp[1024];
    snprintf(mfp, sizeof(mfp), "%s/manifest.mfp", dir);
    if (ggf_ckpt_write(mfp, 42u, 12u, 144u, n, names, sizes, home_of,
                       dup_bytes, data_bytes, "ggf_checkpoint_replay v1",
                       model) != 0) {
        printf("  [CKPT FAIL] manifest write: %s\n", mfp);
        return 1;
    }

    printf("  [CKPT OK] %s — %u tensors · home %u (.ggf ผ่าน mmap view, %.2f s) · "
           "dup %u (%.1f MB saved) · manifest %s\n",
           model, n, n_save, dt, n - n_save, dup_bytes / (1024.0 * 1024.0), mfp);

    /* spawn fresh process → replay */
    int r = _spawnl(P_WAIT, "build/ggf_checkpoint_replay",
                    "build/ggf_checkpoint_replay", "--replay", mfp, model, dir, NULL);
    if (r == -1)
        r = _spawnl(P_WAIT, "build/ggf_checkpoint_replay.exe",
                    "build/ggf_checkpoint_replay.exe", "--replay", mfp, model, dir, NULL);
    if (r == -1) {
        printf("  [CKPT FAIL] cannot spawn replay process (run manually: "
               "--replay %s %s %s)\n", mfp, model, dir);
        return 1;
    }

    free(ptrs); free(sizes); free(home_of); free(names); free(path_buf);
    gguf_box_close(&box);
    return r;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 5 && strcmp(argv[1], "--replay") == 0)
        return replay_mode(argv[2], argv[3], argv[4]);
    if (argc >= 4 && strcmp(argv[1], "--replay") == 0)
        return replay_mode(argv[2], argv[3], "build/ckpt_ggf");

    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        printf("usage:\n"
               "  %s <model.gguf> --ckpt-dir <dir>   checkpoint + spawn fresh-process replay\n"
               "  %s --replay <manifest.mfp> <model.gguf> [dir]\n",
               argv[0], argv[0]);
        return 0;
    }
    if (argc < 2) { printf("usage: see --help\n"); return 1; }

    const char *model = argv[1];
    const char *dir = "build/ckpt_ggf";
    for (int i = 2; i + 1 < argc; i++)
        if (strcmp(argv[i], "--ckpt-dir") == 0) dir = argv[i + 1];
    return checkpoint_mode(model, dir);
}
