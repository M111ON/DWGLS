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
    const char *base_dir = h.base_dir[0] ? h.base_dir : NULL;

    /* reopen GGUF ใน process ใหม่ (memory ว่าง — ข้อมูลมาจากดิสก์เท่านั้น) */
    GGUFBox box;
    memset(&box, 0, sizeof box);
    if (gguf_box_open(&box, model) != 0) {
        printf("  [REPLAY FAIL] cannot open model: %s\n", model);
        free(entries);
        return 1;
    }
    const GGUFBoxEntry **by_idx = (const GGUFBoxEntry **)calloc(h.n, sizeof(void *));
    for (uint32_t i = 0; i < box.n_tensors && i < h.n; i++)
        by_idx[i] = &box.entries[i];
    CmpCtx ctx = { &box, by_idx };

    double t0 = now_sec();
    uint64_t bytes = 0;
    uint32_t ok = 0, fail = 0, skip = 0;
    /* delta: replay merge — SAME → base_dir · STORED → dir (ภายใน header) */
    int rc = ggf_ckpt_replay(&h, entries, dir, base_dir, cmp_vs_gguf, &ctx,
                             &bytes, &ok, &fail, &skip);
    double dt = now_sec() - t0;

    if (base_dir) {
        uint32_t n_same = 0, n_stored = 0;
        for (uint32_t i = 0; i < h.n; i++) {
            if (entries[i].status == GGF_CKPT_SAME) n_same++;
            else if (entries[i].home_of == (int32_t)i) n_stored++;
        }
        printf("  [REPLAY %s] %s — %u/%u tensors lossless (delta merge จาก '%s': "
               "same %u · stored %u) · %llu B · %.1f MB/s\n",
               rc == 0 ? "PASS" : "FAIL", manifest, ok, h.n, base_dir,
               n_same, n_stored, (unsigned long long)bytes,
               bytes / (1024.0 * 1024.0) / (dt ? dt : 1e-9));
    } else if (h.ckpt_round || h.ckpt_tick)
        printf("  [REPLAY %s] %s — %u/%u tensors lossless ตั้งแต่ checkpoint (round=%u tick=%u) · "
               "skip (ก่อน checkpoint): %u · %llu B · %.1f MB/s\n",
               rc == 0 ? "PASS" : "FAIL", manifest, ok,
               h.n - skip, h.ckpt_round, h.ckpt_tick, skip,
               (unsigned long long)bytes,
               bytes / (1024.0 * 1024.0) / (dt ? dt : 1e-9));
    else
        printf("  [REPLAY %s] %s — %u/%u tensors lossless (fresh process จากดิสก์) · "
               "%llu B · %.1f MB/s · dedup saved %llu B (%.1f MB)\n",
               rc == 0 ? "PASS" : "FAIL", manifest, ok, h.n,
               (unsigned long long)bytes,
               bytes / (1024.0 * 1024.0) / (dt ? dt : 1e-9),
               (unsigned long long)h.dup_bytes, h.dup_bytes / (1024.0 * 1024.0));

    gguf_box_close(&box);
    free(by_idx); free(entries);
    return rc == 0 ? 0 : 1;
}

/* ═══ verify — สแกน .ggf + manifest ทั้งชุด (ไม่มีโมเดลต้นทาง) ═══ */
static int verify_mode(const char *dir)
{
    char mfp[1024];
    snprintf(mfp, sizeof mfp, "%s/manifest.mfp", dir);
    GgfCkptHeader h;
    GgfCkptEntry *e = NULL;
    if (ggf_ckpt_read(mfp, &h, &e) != 0) {
        printf("  [VERIFY FAIL] manifest unreadable/corrupt: %s\n", mfp);
        free(e);
        return 1;
    }
    printf("  [VERIFY] %s — provenance: note='%s' model='%s' created=%llu · "
           "tensors=%u ckpt=(%u,%u) dup_bytes=%llu%s%s\n",
           mfp, h.note, h.model, (unsigned long long)h.created_utc, h.n,
           h.ckpt_round, h.ckpt_tick, (unsigned long long)h.dup_bytes,
           h.base_dir[0] ? " · delta base='" : "",
           h.base_dir[0] ? h.base_dir : "");
    uint32_t files = 0, fail = 0;
    int rc = ggf_ckpt_verify(dir, &files, &fail);
    printf("  [VERIFY %s] %s — home .ggf CRC32 ผ่าน %u/%u (delta: base+delta merge) · "
           "ไม่ต้องมีโมเดลต้นทาง\n",
           rc == 0 ? "PASS" : "FAIL", dir, files, files + fail);
    free(e);
    return rc == 0 ? 0 : 1;
}

/* ═══ GC — รวม delta chain เป็น snapshot ใหม่ (base ที่เลิกอ้างแล้ว) ═══ */
static int gc_mode(const char *head_dir, const char *new_dir)
{
    ensure_dir(new_dir);
    GgfCkptChain ch;
    int cr = ggf_ckpt_chain_open(head_dir, &ch);
    if (cr != 0) {
        printf("  [GC FAIL] chain unreadable from %s (rc=%d)\n", head_dir, cr);
        return 1;
    }
    printf("  [GC] chain %d ระดับ (head → base):\n", ch.depth);
    for (int d = 0; d < ch.depth; d++)
        printf("    [%d] %s — tensors=%u base_dir='%s'%s\n", d,
               ch.links[d].dir, ch.links[d].h.n, ch.links[d].h.base_dir,
               d == ch.depth - 1 ? " (tail)" : "");
    char note[GGF_CKPT_NOTE_LEN];
    snprintf(note, sizeof note, "gc: %s", ch.links[0].h.note);
    ggf_ckpt_chain_close(&ch);

    uint32_t n_home = 0, n_fail = 0;
    uint64_t bytes = 0;
    int rc = ggf_ckpt_gc(head_dir, new_dir, note, &n_home, &bytes, &n_fail);
    if (rc != 0) {
        printf("  [GC FAIL] consolidate rc=%d (fail=%u)\n", rc, n_fail);
        return 1;
    }
    printf("  [GC OK] %s ← %s — home %u (.ggf คัดลอก + CRC ผ่าน) · %llu B · "
           "manifest เต็ม (base ว่าง — self-contained)\n",
           new_dir, head_dir, n_home, (unsigned long long)bytes);

    /* รายชื่อ dir ที่ปลอดภัยที่จะลบ (snapshot ใหม่ไม่อ้างอีก) */
    if (ggf_ckpt_chain_open(head_dir, &ch) == 0) {
        printf("  [GC] ปลอดภัยที่จะลบ chain เดิม:\n");
        for (int d = 0; d < ch.depth; d++)
            printf("    rm -rf %s\n", ch.links[d].dir);
        ggf_ckpt_chain_close(&ch);
    }
    return 0;
}

/* ═══ checkpoint — save home .ggf + manifest → spawn fresh process ═══ */
static int checkpoint_mode(const char *model, const char *dir,
                           const char *delta_base,
                           uint32_t ckpt_round, uint32_t ckpt_tick,
                           int max_chain)
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

    /* AUTO-GC: chain ของ base ลึกเกิน --max-chain → รวมเป็น snapshot ใหม่
     * ก่อนเขียน delta ระดับถัดไป (chain ใหม่ลึก 2 — ไม่ยาวเกิน threshold) */
    const char *base_used = delta_base;
    char gc_base[1024];
    if (delta_base && max_chain > 1) {
        snprintf(gc_base, sizeof gc_base, "%s_base", dir);
        uint32_t ag_home = 0;
        uint64_t ag_bytes = 0;
        int ag = ggf_ckpt_auto_gc(delta_base, gc_base, max_chain, "auto-gc",
                                  gc_base, sizeof gc_base, &ag_home, &ag_bytes);
        if (ag < 0) {
            printf("  [CKPT FAIL] auto-gc (chain ลึกเกิน --max-chain %d): rc=%d\n",
                   max_chain, ag);
            return 1;
        }
        if (ag > 0) {
            base_used = gc_base;
            printf("  [CKPT AUTO-GC] base chain ลึกเกิน %d → รวมเป็น %s "
                   "(home %u · %llu B) — chain เดิมปลอดภัยที่จะลบ:",
                   max_chain, gc_base, ag_home,
                   (unsigned long long)ag_bytes);
            GgfCkptChain ch;
            if (ggf_ckpt_chain_open(delta_base, &ch) == 0) {
                for (int d = 0; d < ch.depth; d++)
                    printf(" %s", ch.links[d].dir);
                ggf_ckpt_chain_close(&ch);
            }
            printf("\n");
        }
    }

    /* delta: status ต่อ home — SAME (เท่ากับ base) ไม่เก็บไฟล์ใหม่ */
    uint8_t *status = (uint8_t *)malloc(n);
    memset(status, GGF_CKPT_STORED, n);
    uint32_t n_same = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (base_used && home_of[i] == (int32_t)i && sizes[i] > 0) {
            status[i] = ggf_ckpt_cmp_base(base_used, i, names[i],
                                          sizes[i], ptrs[i]);
            if (status[i] == GGF_CKPT_SAME) n_same++;
        }
    }
    for (uint32_t i = 0; i < n; i++)          /* dup สถานะตาม home */
        if (home_of[i] != (int32_t)i) status[i] = status[home_of[i]];

    /* save home .ggf ผ่าน ggf_save_map (dedup ระดับไฟล์ — เก็บเฉพาะ STORED) */
    char *path_buf = (char *)malloc(n * 256);
    uint32_t n_save = 0, n_save_fail = 0;
    double t0 = now_sec();
    for (uint32_t i = 0; i < n; i++) {
        if (home_of[i] != (int32_t)i) continue;   /* dup — ไม่มีไฟล์ */
        if (sizes[i] == 0) continue;
        if (base_used && status[i] == GGF_CKPT_SAME) continue; /* เก็บไว้ที่ base */
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
                       status, base_used,
                       dup_bytes, data_bytes, ckpt_round, ckpt_tick,
                       "ggf_checkpoint_replay v1", model) != 0) {
        printf("  [CKPT FAIL] manifest write: %s\n", mfp);
        return 1;
    }

    printf("  [CKPT OK] %s — %u tensors · home %u (.ggf ผ่าน mmap view, %.2f s) · "
           "dup %u (%.1f MB saved) · %s\n",
           model, n, base_used ? n_same + n_save : n_save,
           dt, n - n_save, dup_bytes / (1024.0 * 1024.0), mfp);
    if (base_used)
        printf("  [CKPT DELTA] base='%s' · same %u (อ้าง base — ไม่เก็บ) · "
               "stored %u · fail %u\n",
               base_used, n_same, n_save, n_save_fail);

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
    free(status);
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
    if (argc >= 3 && strcmp(argv[1], "--verify") == 0)
        return verify_mode(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "--gc") == 0) {
        const char *new_dir = "build/ckpt_gc";
        for (int i = 3; i + 1 < argc; i++)
            if (strcmp(argv[i], "--ckpt-dir") == 0) new_dir = argv[i + 1];
        return gc_mode(argv[2], new_dir);
    }

    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        printf("usage:\n"
               "  %s <model.gguf> --ckpt-dir <dir> [--ckpt-round R --ckpt-tick T]\n"
               "  %s <model.gguf> --ckpt-dir <dir> --delta <base_dir>   (เก็บเฉพาะที่เปลี่ยน)\n"
               "  %s --replay <manifest.mfp> <model.gguf> [dir]\n"
               "  %s --verify <dir>     สแกน .ggf+manifest (CRC, ไม่ต้องมีโมเดล)\n"
               "  %s --gc <head_dir> [--ckpt-dir <new>]   รวม delta chain → snapshot ใหม่\n"
               "  --delta เพิ่ม --max-chain N: auto-GC เมื่อ chain ของ base ลึกเกิน N\n",
               argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 0;
    }
    if (argc < 2) { printf("usage: see --help\n"); return 1; }

    const char *model = argv[1];
    const char *dir = "build/ckpt_ggf";
    const char *delta_base = NULL;
    uint32_t ckpt_round = 0, ckpt_tick = 0;
    int max_chain = 4;   /* chain ไม่ลึกเกิน 4 (auto-GC ก่อนระดับถัดไป) */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--ckpt-dir") == 0 && i + 1 < argc) dir = argv[++i];
        else if (strcmp(argv[i], "--delta") == 0 && i + 1 < argc) delta_base = argv[++i];
        else if (strcmp(argv[i], "--ckpt-round") == 0 && i + 1 < argc) ckpt_round = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--ckpt-tick") == 0 && i + 1 < argc) ckpt_tick = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--max-chain") == 0 && i + 1 < argc) max_chain = (int)strtol(argv[++i], NULL, 10);
    }
    return checkpoint_mode(model, dir, delta_base, ckpt_round, ckpt_tick, max_chain);
}
