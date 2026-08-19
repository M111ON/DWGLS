/*
 * ggf_fs_probe.c — GGFS: mount checkpoint dir (.ggf + manifest) เป็น FS
 * ═══════════════════════════════════════════════════════════════════════
 *
 * T1.2t — อ่าน tensor ด้วย STATE (seed, round, tick) — enter-anywhere
 * (core/geo_ggf_fs.h — §15.96) — ไม่ต้องมีโมเดลต้นทาง:
 *
 *   --mount <dir>             mount + สรุป (count/seed/provenance)
 *   --read <name> [r t]       อ่าน tensor 1 ตัวจาก state (r, t) — พิมพ์
 *                             size + steps + CRC + 4 bytes แรก
 *   --sweep [r t] [r2 t2]     อ่านทุก tensor จาก start states (r,t) และ
 *                             (r2,t2) → เปรียบเทียบ byte-for-byte (สอง state
 *                             ต้องให้ bytes เดียวกัน) + steps ต่อ state
 *
 * BUILD: make ggf_fs
 * RUN:   ./build/ggf_fs_probe --mount build/ckpt_ggf
 *        ./build/ggf_fs_probe --mount build/ckpt_ggf --sweep 7 3 0 0
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../core/geo_ggf_fs.h"

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int mount_mode(const char *dir, GgfsMount *fs)
{
    int rc = ggfs_mount(dir, fs);
    if (rc != 0) {
        printf("  [GGFS FAIL] mount %s (rc=%d) — manifest/chain เปิดไม่ได้\n", dir, rc);
        return 1;
    }
    const GgfCkptHeader *h = &fs->chain.links[0].h;
    printf("  [GGFS MOUNT] %s — %u tensors · seed=%u ticks=%u cycles=%u · "
           "chain %d ระดับ%s%s\n",
           dir, fs->n, fs->seed, fs->ticks, fs->cycles, fs->chain.depth,
           h->base_dir[0] ? " · base='" : "", h->base_dir[0] ? h->base_dir : "");
    printf("  [GGFS MOUNT] provenance: note='%s' model='%s' created=%llu\n",
           h->note, h->model, (unsigned long long)h->created_utc);
    printf("  [GGFS MOUNT] ckpt=(%u,%u)%s · dup bytes saved %llu (%.1f MB)\n",
           h->ckpt_round, h->ckpt_tick,
           (h->ckpt_round || h->ckpt_tick) ? " (mid-round — อ่านเฉพาะ pending)" : "",
           (unsigned long long)h->dup_bytes, h->dup_bytes / (1024.0 * 1024.0));
    return 0;
}

static int read_one(GgfsMount *fs, const char *name, uint32_t r, uint32_t t)
{
    int32_t idx = ggfs_find(fs, name);
    if (idx < 0) {
        printf("  [READ FAIL] ไม่พบ tensor: %s\n", name);
        return 1;
    }
    uint8_t *buf = (uint8_t *)malloc(fs->walk.sizes[idx] ? fs->walk.sizes[idx] : 1);
    uint64_t got = 0;
    uint64_t steps_before = fs->walk_steps;
    int rc = ggfs_read(fs, (uint32_t)idx, r, t, buf, fs->walk.sizes[idx], &got);
    uint64_t steps = fs->walk_steps - steps_before;
    if (rc != 0) {
        printf("  [READ %s] state=(%u,%u) → rc=%d (fail)\n", name, r, t, rc);
        free(buf);
        return 1;
    }
    uint32_t crc = ggf_crc32(buf, got, 0);
    GgfsStat st;
    ggfs_stat(fs, (uint32_t)idx, &st);
    printf("  [READ %s] state=(%u,%u) → %llu B · steps=%llu · crc32=%08x · "
           "rq=%u tick=%u%s%s\n",
           name, r, t, (unsigned long long)got, (unsigned long long)steps, crc,
           st.rq, st.tick, st.dup ? " · dup" : "",
           st.pending ? " · PENDING (ก่อน checkpoint)" : "");
    printf("           first 16 B: ");
    for (int i = 0; i < 16 && i < (int)got; i++) printf("%02x ", buf[i]);
    printf("\n");
    free(buf);
    return 0;
}

static int sweep_mode(GgfsMount *fs, uint32_t r1, uint32_t t1,
                      uint32_t r2, uint32_t t2)
{
    uint64_t max_sz = 0;
    for (uint32_t i = 0; i < fs->n; i++)
        if (fs->walk.sizes[i] > max_sz) max_sz = fs->walk.sizes[i];
    if (max_sz < 1) max_sz = 1;
    uint8_t *a = (uint8_t *)malloc(max_sz);
    uint8_t *b = (uint8_t *)malloc(max_sz);
    if (!a || !b) { free(a); free(b); return 1; }
    uint64_t ok = 0, fail = 0, pending = 0, empty = 0;
    uint64_t steps1 = 0, steps2 = 0;
    double t0 = now_sec();
    for (uint32_t i = 0; i < fs->n; i++) {
        if (fs->walk.home_of[i] < 0) { empty++; continue; }
        uint64_t got1 = 0, got2 = 0;
        uint64_t s1 = fs->walk_steps;
        int rc1 = ggfs_read(fs, i, r1, t1, a, max_sz, &got1);
        steps1 += fs->walk_steps - s1;
        if (rc1 == -2) { pending++; continue; }
        if (rc1 != 0) { fail++; continue; }
        uint64_t s2 = fs->walk_steps;
        int rc2 = ggfs_read(fs, i, r2, t2, b, max_sz, &got2);
        steps2 += fs->walk_steps - s2;
        if (rc2 == 0 && got1 == got2 && memcmp(a, b, got1) == 0 &&
            got1 == fs->walk.sizes[i])
            ok++;
        else {
            fail++;
            if (fail <= 3)
                printf("    [sweep-fail] idx=%u rc1=%d rc2=%d got1=%llu got2=%llu\n",
                       i, rc1, rc2, (unsigned long long)got1, (unsigned long long)got2);
        }
    }
    double dt = now_sec() - t0;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < fs->n; i++)
        if (fs->walk.home_of[i] >= 0) bytes += fs->walk.sizes[i];
    printf("  [SWEEP] state A=(%u,%u) · state B=(%u,%u) — %llu/%llu tensor "
           "byte-for-byte เหมือนกัน · fail %llu · pending %llu · ว่าง %llu\n",
           r1, t1, r2, t2, (unsigned long long)ok,
           (unsigned long long)(fs->n - pending - empty),
           (unsigned long long)fail, (unsigned long long)pending,
           (unsigned long long)empty);
    printf("  [SWEEP] steps: A=%llu · B=%llu (state ต่าง → เส้นทางต่าง ข้อมูลเดียวกัน) · "
           "%llu B · %.1f MB/s\n",
           (unsigned long long)steps1, (unsigned long long)steps2,
           (unsigned long long)bytes,
           bytes / (1024.0 * 1024.0) / (dt ? dt : 1e-9));
    free(a); free(b);
    return fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        printf("usage:\n"
               "  %s --mount <ckpt-dir> [--read <name> [r t]] [--sweep r1 t1 r2 t2]\n",
               argv[0]);
        return 1;
    }
    static GgfsMount fs;
    const char *dir = argv[2];
    if (mount_mode(dir, &fs) != 0) return 1;

    const char *read_name = NULL;
    uint32_t rr = 7, tt = 3, r1 = 7, t1 = 3, r2 = 0, t2 = 0;
    int do_sweep = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--read") == 0 && i + 1 < argc) {
            read_name = argv[++i];
            if (i + 2 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                rr = (uint32_t)strtoul(argv[++i], NULL, 10);
                tt = (uint32_t)strtoul(argv[++i], NULL, 10);
            }
        } else if (strcmp(argv[i], "--sweep") == 0 && i + 4 < argc) {
            r1 = (uint32_t)strtoul(argv[++i], NULL, 10);
            t1 = (uint32_t)strtoul(argv[++i], NULL, 10);
            r2 = (uint32_t)strtoul(argv[++i], NULL, 10);
            t2 = (uint32_t)strtoul(argv[++i], NULL, 10);
            do_sweep = 1;
        }
    }

    int rc = 0;
    if (read_name) rc |= read_one(&fs, read_name, rr, tt);
    if (do_sweep) rc |= sweep_mode(&fs, r1, t1, r2, t2);
    if (!read_name && !do_sweep) {
        /* สรุป stat เบาๆ: นับ STORED/SAME/dup/pending */
        uint32_t n_stored = 0, n_same = 0, n_dup = 0, n_pending = 0;
        for (uint32_t i = 0; i < fs.n; i++) {
            GgfsStat st;
            ggfs_stat(&fs, i, &st);
            if (st.status == GGF_CKPT_STORED) n_stored++;
            if (st.status == GGF_CKPT_SAME) n_same++;
            if (st.dup) n_dup++;
            if (st.pending) n_pending++;
        }
        printf("  [GGFS] สรุป: STORED %u · SAME %u (อ้าง chain) · dup %u · "
               "pending %u (mid-round)\n",
               n_stored, n_same, n_dup, n_pending);
        printf("  ใช้ --sweep <r1> <t1> <r2> <t2> เพื่ออ่านทุก tensor จาก 2 states\n");
    }
    ggfs_unmount(&fs);
    return rc;
}
