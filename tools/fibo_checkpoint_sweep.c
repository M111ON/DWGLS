/* tools/fibo_checkpoint_sweep.c — custom-config checkpoint-replay sweep
 * ═══════════════════════════════════════════════════════════════════════
 * สนาม deterministic + checkpoint + tick — วนกี่รอบก็ได้ (user principle):
 *   state = (seed, round, tick) — เก็บแค่วิธีการสร้างกับ seed
 *
 * ต่างจาก tests/test_fibo_checkpoint.c (ตายตัว: 1728×12, 144 รอบ, 64×4KB):
 * รับ config เองได้ทุกมิติ แล้วรัน checkpoint-replay พิสูจน์ lossless:
 *
 *   ตาราง (table)  : pipes × ticks ต่อรอบ      --pipes N --ticks M
 *   สนาม (field)   : cycles รอบ (scale axis)   --cycles C      (≤ 255)
 *   ระยะ (distance): max offset from→to        --dist D
 *   ข้อมูล (volume): chunks × size bytes        --chunks N --size S (≤ 64KB)
 *   หมุนวน (pattern): scatter|cluster|allone|wrap|random  --pattern P
 *   checkpoint      : รอบกลาง (หรือ --ckpt R)
 *   --sweep         : รัน matrix หลาย config (default) แล้วสรุป
 *
 * PERSIST (durable restore — fresh process จากดิสก์):
 *   --sweep เขียน image ลง build/ckpt/<tag>.img + manifest <tag>.cfg โดยอัตโนมัติ
 *   แล้ว spawn ตัวมันเองเป็น process ใหม่: --verify-img <img> <cfg>
 *   → reload จากไฟล์จริงใน process ที่ memory ว่าง → พิสูจน์ lossless
 *   ตรวจเองทีหลังได้: --verify-all [DIR] (สแกน *.cfg → verify ทุกตัว)
 *   single config: เพิ่ม --persist[=DIR] (default build/ckpt)
 *
 * ECONOMY verdict (auto-detect "ไม่คุ้ม"):
 *   ทุก config พิมพ์ verdict: EXCELLENT (≤ thr/2) / WORTH (≤ thr) /
 *   MARGINAL (≤ thr×2.5) / NOT WORTH — เปรียบเทียบ overhead% กับ
 *   threshold ขนาดข้อมูล (default 2.0% — ปรับด้วย --economy X.X)
 *
 * พิสูจน์ต่อ config:
 *   lossless หลัง checkpoint/reload + เดินต่อข้ามรอบ (byte-for-byte)
 *   เสาเข็มห้ามขยับ: round ผิด → bond แตก / route ผิด → NULL
 *   log = 5B/route ∝ events (ไม่ขึ้นกับข้อมูล/ระยะ)
 *   รายงาน: overhead% จริง, routes, wraps (route วนข้าม 0), rounds ใช้จริง
 *
 * BUILD: gcc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format \
 *        -I. -Icore -Icore/infra -o build/fibo_checkpoint_sweep \
 *        tools/fibo_checkpoint_sweep.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <process.h>
#include <dirent.h>
#include "../core/infra/fibo_spine.h"
#include "../core/geo_ghost_lift.h"
#include "../core/fibo_walk.h"

#define CKPT_HEADER 28u  /* seed(8)+round(8)+tick(4)+ver/res(8) */
#define PERSIST_MAGIC "FCKP"
#define DEFAULT_CKPT_DIR "build/ckpt"

/* ── patterns ─────────────────────────────────────────── */
#define PAT_SCATTER  0
#define PAT_CLUSTER  1
#define PAT_ALLONE   2
#define PAT_WRAP     3
#define PAT_RANDOM   4

typedef struct {
    uint32_t pipes;    /* ตาราง: pipes ต่อรอบ              */
    uint32_t ticks;    /* ตาราง: ticks ต่อ cycle           */
    uint32_t cycles;   /* สนาม: จำนวนรอบ (scale axis)      */
    uint32_t chunks;   /* ปริมาณ: จำนวน chunk              */
    uint32_t size;     /* ปริมาณ: bytes ต่อ chunk (≤ 64KB) */
    uint32_t dist;     /* ระยะ: offset from→to             */
    uint32_t ckpt;     /* รอบ checkpoint (0 = cycles/2)    */
    int      pattern;  /* PAT_*                            */
    uint32_t seed;     /* random seed                      */
} Cfg;

typedef struct {
    uint16_t block;
    uint8_t  r0, rq;
    uint32_t len;
    uint8_t  data[RS_MAX_DATA_SIZE];
} Chunk;

static double g_econ_thr = 2.0;   /* economy threshold (% overhead) */

static uint32_t g_rand_state;
static uint32_t xrand(void) {
    g_rand_state ^= g_rand_state << 13;
    g_rand_state ^= g_rand_state >> 17;
    g_rand_state ^= g_rand_state << 5;
    return g_rand_state;
}

static const char *pat_name(int p) {
    switch (p) {
        case PAT_SCATTER: return "scatter";
        case PAT_CLUSTER: return "cluster";
        case PAT_ALLONE:  return "allone";
        case PAT_WRAP:    return "wrap";
        case PAT_RANDOM:  return "random";
        default:          return "?";
    }
}

static void fill_pattern(uint8_t *b, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++)
        b[i] = (uint8_t)((seed + i * 131u) ^ (i >> 3) ^ (seed >> 7));
}

/* birth round ตาม pattern + route (rq) ตามระยะ — หมุนวนได้หลายรูปแบบ
   DETERMINISTIC จาก cfg (รวม seed) — manifest เก็บแค่ cfg แล้ว regenerate ได้ */
static void gen_chunks(Chunk *c, uint32_t n, const Cfg *cfg) {
    g_rand_state = cfg->seed ? cfg->seed : 0xC0FFEEu;
    uint32_t half = cfg->cycles / 2u;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r0;
        switch (cfg->pattern) {
            case PAT_SCATTER:
                r0 = (uint32_t)(((uint64_t)i * 2654435761u) % cfg->cycles);
                break;
            case PAT_CLUSTER:                       /* กองที่ 4 รอบ */
                r0 = (cfg->cycles >= 4u) ? (i % 4u) * (cfg->cycles / 4u)
                                         : (i % cfg->cycles);
                break;
            case PAT_ALLONE:                        /* ทุกก้อนรอบเดียว */
                r0 = half;
                break;
            case PAT_WRAP:                          /* จาก + ระยะ → วนข้าม 0 */
                r0 = (uint32_t)(((uint64_t)i * 2654435761u) % cfg->cycles);
                break;
            default: /* PAT_RANDOM */
                r0 = xrand() % cfg->cycles;
                break;
        }
        uint32_t rq;
        if (cfg->pattern == PAT_WRAP)
            rq = (r0 + cfg->dist) % cfg->cycles;    /* ระยะเดียวคงที่ */
        else
            rq = (r0 + 1u + (i % (cfg->dist ? cfg->dist : 1u))) % cfg->cycles;
        c[i].block = (uint16_t)i;
        c[i].r0    = (uint8_t)r0;
        c[i].rq    = (uint8_t)rq;
        c[i].len   = cfg->size;
        fill_pattern(c[i].data, cfg->size, 1000u + i);
    }
}

static int cmp_r0(const void *a, const void *b) {
    const Chunk *x = (const Chunk *)a, *y = (const Chunk *)b;
    return (x->r0 > y->r0) - (x->r0 < y->r0);
}

/* round capacity ให้เป็น power of 2 (rs ใช้ mask = capacity-1) */
static uint32_t round_pow2(uint32_t v) {
    uint32_t c = 64;
    while (c < v) c <<= 1;
    return c;
}

/* ═══ §15.76 walk-based access — เดิน spine tick-by-tick หา route ที่ live ═══
   state = (seed, round, tick) พอสำหรับทุกตำแหน่งทุกตาราง:
   ที่ตำแหน่ง (r, t) route ที่ live = {i : rq_i == r และ rq_i % ticks == t}
   — คำนวณสนามใหม่จาก (method + seed) (chunks array = field ที่ recompute)
   → ไม่ต้อง index: เดินนาฬิกาจาก state ใดก็ถึงทุกตำแหน่ง อ่าน lossless */
typedef struct { const Chunk *chunks; } SweepCtx;
static void sweep_chunk_route(void *ctx, uint32_t i, FiboWalkRoute *out) {
    const SweepCtx *sc = (const SweepCtx *)ctx;
    out->block = sc->chunks[i].block;
    out->r0    = sc->chunks[i].r0;
    out->rq    = sc->chunks[i].rq;
}

/* เดินจาก start ไป target ของทุก chunk → อ่านผ่าน route → เทียบ data */
static int walk_read_all(const Cfg *cfg, GhostLog *log, ResidualSpace *rs,
                         const Chunk *chunks, uint32_t n,
                         FiboWalkPos start, uint32_t *max_steps) {
    int ok = 1;
    for (uint32_t i = 0; i < n && ok; i++) {
        FiboWalkPos target = { chunks[i].rq, (uint32_t)(chunks[i].rq % cfg->ticks), 0 };
        FiboWalkPos pos;
        if (!fibo_walk_to(NULL, NULL, cfg->ticks, cfg->cycles, start,
                          target.round, target.tick, &pos)) { ok = 0; break; }
        uint64_t expect = fibo_walk_dist(&start, &target, cfg->ticks, cfg->cycles);
        if (pos.steps - start.steps != expect) { ok = 0; break; }
        uint32_t d = (uint32_t)(pos.steps - start.steps);
        if (d > *max_steps) *max_steps = d;
        uint32_t sz = 0;
        const void *got = ghost_read(log, rs, chunks[i].block, chunks[i].r0,
                                     chunks[i].rq, &sz);
        if (!got || sz != chunks[i].len ||
            memcmp(got, chunks[i].data, chunks[i].len) != 0) ok = 0;
    }
    return ok;
}

/* พิสูจน์ walk-based access ทั้งชุด → 1 = ครบ (coverage + enter-anywhere +
   ว่าง + field ต่างจับได้) — ใช้กับ state ใดก็ได้ (live / reload จาก disk) */
static int walk_proof(const Cfg *cfg, GhostLog *log, ResidualSpace *rs,
                      const Chunk *chunks, uint32_t n) {
    SweepCtx ctx = { chunks };
    uint32_t *counts = (uint32_t *)calloc((size_t)cfg->cycles * cfg->ticks,
                                          sizeof(uint32_t));
    if (!counts) return 0;
    uint64_t total = fibo_walk_coverage(sweep_chunk_route, &ctx, n, cfg->ticks,
                                        cfg->cycles, counts);
    int ok = (total == n);

    /* enter-anywhere: 3 start states → ทุก chunk lossless */
    FiboWalkPos sA = { 0u, 0u, 0 };
    FiboWalkPos sB = { cfg->cycles / 2u, 2u, 0 };
    FiboWalkPos sC = { cfg->cycles - 1u, cfg->ticks - 1u, 0 };
    uint32_t max_steps = 0;
    int okA = walk_read_all(cfg, log, rs, chunks, n, sA, &max_steps);
    int okB = walk_read_all(cfg, log, rs, chunks, n, sB, &max_steps);
    int okC = walk_read_all(cfg, log, rs, chunks, n, sC, &max_steps);
    int ok3 = okA && okB && okC;
    ok = ok && ok3;

    /* ตำแหน่งว่าง (tick ≠ รอบ%ticks) → 0 live */
    uint32_t r_empty = cfg->cycles - 1u;
    uint32_t t_empty = (r_empty % cfg->ticks + 1u) % cfg->ticks;
    FiboWalkPos pe = { r_empty, t_empty, 0 };
    FiboWalkRoute live[64];
    uint32_t lc = fibo_walk_live(sweep_chunk_route, &ctx, n, cfg->ticks, &pe, live, 64);
    ok = ok && lc == 0;

    /* seed/method ต่าง = field ต่าง → route ไม่มีใน log → NULL (จับได้)
       เปรียบเทียบ per-block (chunks ที่รับเข้ามาอาจถูก qsort แล้ว — ต้อง
       regenerate field ใหม่เป็น array ตรงๆ ให้ index ตรงกัน) */
    Cfg bad = *cfg;
    if (cfg->pattern == PAT_RANDOM) bad.seed += 1u;
    else                            bad.dist = cfg->dist + 3u;
    Chunk *gchunks = (Chunk *)calloc(n, sizeof(Chunk));
    Chunk *bchunks = (Chunk *)calloc(n, sizeof(Chunk));
    uint32_t differs = 0, detected = 0;
    if (gchunks && bchunks) {
        gen_chunks(gchunks, n, cfg);
        gen_chunks(bchunks, n, &bad);
        for (uint32_t i = 0; i < n; i++) {
            if (bchunks[i].r0 != gchunks[i].r0 || bchunks[i].rq != gchunks[i].rq)
                differs++;
            uint32_t sz = 0;
            if (ghost_read(log, rs, bchunks[i].block, bchunks[i].r0,
                           bchunks[i].rq, &sz) == NULL) detected++;
        }
        ok = ok && differs >= 1 && detected == differs && detected >= 1;
        free(gchunks); free(bchunks);
    } else { free(gchunks); free(bchunks); ok = 0; }

    printf("       walk: coverage %llu/%u · enter-anywhere %s · lossless %s · "
           "field-other %u/%u NULL · max %u steps%s\n",
           (unsigned long long)total, n,
           ok3 ? "3/3" : "FAIL", ok3 ? "3/3" : "FAIL",
           detected, differs, max_steps,
           ok ? "  ✓" : "  ✗");
    free(counts);
    return ok;
}


/* ═══ economy verdict — auto-detect "ไม่คุ้ม" ═══════════ */
static const char *economy(double pct, double thr) {
    if (pct <= thr * 0.5)  return "EXCELLENT";
    if (pct <= thr)        return "WORTH";
    if (pct <= thr * 2.5)  return "MARGINAL";
    return "NOT WORTH";
}

/* ═══ file helpers ═════════════════════════════════════ */
static int write_file_all(const char *path, const void *buf, uint64_t sz) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, (size_t)sz, f);
    fclose(f);
    return (uint64_t)w == sz ? 0 : -1;
}

static int read_file_all(const char *path, void **out, uint64_t *out_sz) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return -1; }
    void *b = malloc((size_t)n);
    if (!b) { fclose(f); return -1; }
    size_t r = fread(b, 1, (size_t)n, f);
    fclose(f);
    if (r != (size_t)n) { free(b); return -1; }
    *out = b;
    *out_sz = (uint64_t)n;
    return 0;
}

static void ensure_dir(const char *dir) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *s = tmp + 1; *s; s++) {
        if (*s == '/' || *s == '\\') { *s = '\0'; mkdir(tmp); *s = '/'; }
    }
    mkdir(tmp);
}

/* ═══ manifest: เก็บ cfg + image size (เก็บแค่ "วิธีสร้างกับ seed") ═══ */
typedef struct {  /* 9 × u32 = 36B */
    uint32_t pipes, ticks, cycles, chunks, size, dist, ckpt, pattern, seed;
} ManiCfg;

static int manifest_write(const char *path, const Cfg *cfg, uint64_t img_sz) {
    uint8_t buf[50];
    memcpy(buf, PERSIST_MAGIC, 4);
    buf[4] = 1; buf[5] = 0;                       /* version */
    ManiCfg m = { cfg->pipes, cfg->ticks, cfg->cycles, cfg->chunks, cfg->size,
                  cfg->dist, cfg->ckpt ? cfg->ckpt : cfg->cycles / 2u,
                  (uint32_t)cfg->pattern, cfg->seed };
    memcpy(buf + 6, &m, sizeof(m));
    memcpy(buf + 6 + sizeof(m), &img_sz, 8);
    return write_file_all(path, buf, sizeof(buf));
}

static int manifest_read(const char *path, Cfg *cfg, uint64_t *img_sz) {
    void *b = NULL;
    uint64_t n = 0;
    if (read_file_all(path, &b, &n) != 0 || n < 50) { free(b); return -1; }
    const uint8_t *p = (const uint8_t *)b;
    if (memcmp(p, PERSIST_MAGIC, 4) != 0 || p[4] != 1) { free(b); return -1; }
    ManiCfg m;
    memcpy(&m, p + 6, sizeof(m));
    memcpy(img_sz, p + 6 + sizeof(m), 8);
    cfg->pipes   = m.pipes;
    cfg->ticks   = m.ticks;
    cfg->cycles  = m.cycles;
    cfg->chunks  = m.chunks;
    cfg->size    = m.size;
    cfg->dist    = m.dist;
    cfg->ckpt    = m.ckpt;
    cfg->pattern = (int)m.pattern;
    cfg->seed    = m.seed;
    free(b);
    return 0;
}

/* ═══ verify จากไฟล์ (FRESH PROCESS — memory ใหม่ทั้งหมด) ═══
   อ่าน image + manifest จากดิสก์ → reload → regenerate chunks จาก cfg
   → พิสูจน์ lossless + counts + header round */
static int verify_img_mode(const char *img_path, const char *cfg_path) {
    Cfg cfg;
    uint64_t img_sz = 0;
    if (manifest_read(cfg_path, &cfg, &img_sz) != 0) {
        printf("  [RESTORE FAIL] %s — manifest corrupt\n", cfg_path);
        return 1;
    }
    void *buf = NULL;
    uint64_t n = 0;
    if (read_file_all(img_path, &buf, &n) != 0 || n != img_sz || n < CKPT_HEADER) {
        printf("  [RESTORE FAIL] %s — image unreadable/size mismatch\n", img_path);
        free(buf);
        return 1;
    }
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t seed, round;
    memcpy(&seed, p, 8);
    memcpy(&round, p + 8, 8);
    if (seed != GHOST_SEED_MAGIC || round != cfg.ckpt) {
        printf("  [RESTORE FAIL] %s — header mismatch (round %llu != %u)\n",
               img_path, (unsigned long long)round, cfg.ckpt);
        free(buf);
        return 1;
    }
    /* log อยู่หลัง header: อ่าน magic+count → หา rs ที่ตามมา */
    GhostLog log;   ghost_log_init(&log);
    ResidualSpace rs;
    if (rs_init(&rs, round_pow2(cfg.chunks)) != 0) { free(buf); return 1; }
    const uint8_t *q = p + CKPT_HEADER;
    uint32_t lcount = 0;
    memcpy(&lcount, q + 8, 4);
    uint64_t lsz = 12u + (uint64_t)lcount * sizeof(GhostLogEntry);
    int ok = (memcmp(q, "GHST", 4) == 0 && lcount <= GHOST_LOG_MAX &&
              q + lsz <= p + n);
    if (ok) ok = ghost_log_load(&log, q, lsz) == 0;
    if (ok) ok = rs_load(&rs, q + lsz, n - (uint64_t)(q + lsz - p)) == 0;
    if (!ok || rs.count != cfg.chunks || log.count != cfg.chunks) {
        printf("  [RESTORE FAIL] %s — reload corrupt (log %u/%u, rs %u/%u)\n",
               img_path, log.count, cfg.chunks, rs.count, cfg.chunks);
        rs_free(&rs);
        free(buf);
        return 1;
    }
    /* regenerate expected chunks จาก cfg (เก็บแค่ seed+method) */
    Chunk *chunks = (Chunk *)calloc(cfg.chunks, sizeof(Chunk));
    if (!chunks) { rs_free(&rs); free(buf); return 1; }
    gen_chunks(chunks, cfg.chunks, &cfg);
    ok = 1;
    for (uint32_t i = 0; i < cfg.chunks && ok; i++) {
        uint32_t sz = 0;
        const void *got = ghost_read(&log, &rs, chunks[i].block, chunks[i].r0,
                                     chunks[i].rq, &sz);
        if (!got || sz != chunks[i].len ||
            memcmp(got, chunks[i].data, chunks[i].len) != 0) ok = 0;
    }
    /* เสาเข็มห้ามขยับ (ใน process ใหม่ด้วย) */
    if (ok) {
        uint32_t sz = 0;
        uint8_t wr0 = (uint8_t)((chunks[0].r0 + 1u) % cfg.cycles);
        uint8_t wrq = (uint8_t)((chunks[0].rq + 1u) % cfg.cycles);    if (ghost_read(&log, &rs, chunks[0].block, wr0, chunks[0].rq, &sz) != NULL) ok = 0;
    if (ghost_read(&log, &rs, chunks[0].block, chunks[0].r0, wrq, &sz) != NULL) ok = 0;
    }
    /* ── §15.76 walk-based access หลัง restore จากดิสก์ (fresh process) ── */
    if (ok) ok = walk_proof(&cfg, &log, &rs, chunks, cfg.chunks);
    double overhead_pct = 100.0 * (double)((int64_t)n - (int64_t)cfg.chunks * cfg.size)
                                        / (double)((int64_t)cfg.chunks * cfg.size);
    printf("  [RESTORE %s] %s — %u chunks × %u B จาก %s (disk %llu B, overhead %+.2f%%, economy=%s)\n",
           ok ? "PASS" : "FAIL", img_path, cfg.chunks, cfg.size, cfg_path,
           (unsigned long long)n, overhead_pct,
           economy(overhead_pct, g_econ_thr));
    free(chunks);
    rs_free(&rs);
    free(buf);
    return ok ? 0 : 1;
}

static int verify_all_mode(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { printf("  (cannot open dir %s)\n", dir); return 1; }
    int pass = 0, fail = 0, n = 0;
    struct dirent *e;
    char cfg_path[1024];
    while ((e = readdir(d)) != NULL) {
        size_t L = strlen(e->d_name);
        if (L < 5 || strcmp(e->d_name + L - 4, ".cfg") != 0) continue;
        snprintf(cfg_path, sizeof(cfg_path), "%s/%s", dir, e->d_name);
        char img_path[1024];
        char base[512];
        memcpy(base, e->d_name, L - 4);
        base[L - 4] = '\0';
        snprintf(img_path, sizeof(img_path), "%s/%s.img", dir, base);
        n++;
        if (verify_img_mode(img_path, cfg_path) == 0) pass++; else fail++;
    }
    closedir(d);
    printf("── verify-all %s: %d/%d RESTORE PASS (fresh processes, จากดิสก์) ──\n",
           dir, pass, n);
    return fail == 0 ? 0 : 1;
}

/* ── spawn ตัวมันเองเป็น fresh process เพื่อ verify จากไฟล์ ── */
static int spawn_verify(const char *self, const char *img, const char *cfg) {
    int r = _spawnl(P_WAIT, self, self, "--verify-img", img, cfg, NULL);
    if (r == -1) {
        char exe[1024];
        snprintf(exe, sizeof(exe), "%s.exe", self);
        r = _spawnl(P_WAIT, exe, exe, "--verify-img", img, cfg, NULL);
    }
    return r;
}

/* ── หนึ่ง config: วาง → checkpoint → restart/reload → เดินต่อ → พิสูจน์ ──
   persist_dir != NULL → เขียน image+manifest ลงดิสก์ + spawn fresh process
   returns 1 = lossless ครบทุกข้อ, 0 = พัง */
static int run_config(const Cfg *cfg, uint64_t *out_image, uint64_t *out_data,
                      const char *persist_dir, const char *tag,
                      const char *self) {
    Chunk *chunks = (Chunk *)calloc(cfg->chunks, sizeof(Chunk));
    if (!chunks) return 0;
    gen_chunks(chunks, cfg->chunks, cfg);
    qsort(chunks, cfg->chunks, sizeof(Chunk), cmp_r0);

    uint32_t ckpt = cfg->ckpt ? cfg->ckpt : cfg->cycles / 2u;
    uint64_t data_bytes = (uint64_t)cfg->chunks * cfg->size;

    GhostLog log;      ghost_log_init(&log);
    ResidualSpace rs;
    if (rs_init(&rs, round_pow2(cfg->chunks)) != 0) { free(chunks); return 0; }
    P5HRibcage rc;     p5h_ribcage_init(&rc, NULL);

    uint32_t placed = 0, wraps = 0, rounds_used = 0;
    uint8_t  seen[256]; memset(seen, 0, sizeof(seen));
    for (uint32_t i = 0; i < cfg->chunks; i++) {
        if (chunks[i].r0 > ckpt) continue;
        uint64_t bk = ghost_lift(&log, &rs, chunks[i].block, chunks[i].r0,
                                 chunks[i].rq, chunks[i].data, chunks[i].len);
        if (bk == RS_BOND_KEY_RESERVED) { printf("  (place failed @chunk %u)\n", i); goto out_fail; }
        p5h_ribcage_step(&rc, (uint16_t)(chunks[i].block % cfg->pipes),
                         (uint8_t)(chunks[i].rq % cfg->ticks), bk);
        if (chunks[i].rq < chunks[i].r0) wraps++;   /* route วนข้าม 0 */
        if (!seen[chunks[i].r0]) { seen[chunks[i].r0] = 1; rounds_used++; }
        placed++;
    }

    int ok = 1;
    for (uint32_t i = 0; i < cfg->chunks && ok; i++) {
        if (chunks[i].r0 > ckpt) continue;
        uint32_t sz = 0;
        const void *got = ghost_read(&log, &rs, chunks[i].block, chunks[i].r0,
                                     chunks[i].rq, &sz);
        if (!got || sz != chunks[i].len || memcmp(got, chunks[i].data, chunks[i].len) != 0) ok = 0;
    }
    if (!ok) { printf("  (live read mismatch)\n"); goto out_fail; }

    /* ── checkpoint image (ใน memory) ── */
    uint64_t rsz = rs_serialize_size(&rs);
    uint64_t lsz = ghost_log_serialize_size(&log);
    uint64_t image_sz = CKPT_HEADER + rsz + lsz;
    uint8_t *img = (uint8_t *)malloc((size_t)image_sz);
    if (!img) goto out_fail;
    uint8_t *p = img;
    uint64_t seed = GHOST_SEED_MAGIC;
    memcpy(p, &seed, 8); p += 8;
    uint64_t ck = ckpt;          memcpy(p, &ck, 8); p += 8;
    uint32_t tk = 2;             memcpy(p, &tk, 4); p += 4;
    p[0] = 1; p[1] = 0; p[2] = 0; p[3] = 0;
    p[4] = 0; p[5] = 0; p[6] = 0; p[7] = 0; p += 8;
    if (ghost_log_serialize(&log, p, lsz) != lsz) { free(img); goto out_fail; }
    p += lsz;
    if (rs_serialize(&rs, p, rsz) != rsz) { free(img); goto out_fail; }
    p += rsz;

    /* ── restart (ทุกอย่างใหม่) + reload จาก memory image ── */
    GhostLog log2;      ghost_log_init(&log2);
    ResidualSpace rs2;
    if (rs_init(&rs2, round_pow2(cfg->chunks)) != 0) { free(img); goto out_fail; }
    P5HRibcage rc2;     p5h_ribcage_init(&rc2, NULL);
    const uint8_t *q = img + 8;
    uint64_t r2; memcpy(&r2, q, 8); q += 8;
    q += 12;                                    /* tick(4) + ver/res(8) */
    if (r2 != ckpt) { printf("  (ckpt round restore fail)\n"); goto out_fail2; }
    if (ghost_log_load(&log2, q, lsz) != 0) { printf("  (log load fail)\n"); goto out_fail2; }
    q += lsz;
    if (rs_load(&rs2, q, rsz) != 0) { printf("  (space load fail)\n"); goto out_fail2; }
    if (rs2.count != rs.count || log2.count != log.count) { printf("  (count mismatch)\n"); goto out_fail2; }

    ok = 1;
    for (uint32_t i = 0; i < cfg->chunks && ok; i++) {
        if (chunks[i].r0 > ckpt) continue;
        uint32_t sz = 0;
        const void *got = ghost_read(&log2, &rs2, chunks[i].block, chunks[i].r0,
                                     chunks[i].rq, &sz);
        if (!got || sz != chunks[i].len || memcmp(got, chunks[i].data, chunks[i].len) != 0) ok = 0;
    }
    if (!ok) { printf("  (post-reload mismatch)\n"); goto out_fail2; }

    /* ── เดินต่อ วาง birth round > ckpt ── */
    for (uint32_t i = 0; i < cfg->chunks; i++) {
        if (chunks[i].r0 <= ckpt) continue;
        uint64_t bk = ghost_lift(&log2, &rs2, chunks[i].block, chunks[i].r0,
                                 chunks[i].rq, chunks[i].data, chunks[i].len);
        if (bk == RS_BOND_KEY_RESERVED) { printf("  (post place failed)\n"); goto out_fail2; }
        p5h_ribcage_step(&rc2, (uint16_t)(chunks[i].block % cfg->pipes),
                         (uint8_t)(chunks[i].rq % cfg->ticks), bk);
        if (chunks[i].rq < chunks[i].r0) wraps++;
        if (!seen[chunks[i].r0]) { seen[chunks[i].r0] = 1; rounds_used++; }
    }

    ok = 1;
    for (uint32_t i = 0; i < cfg->chunks && ok; i++) {
        uint32_t sz = 0;
        const void *got = ghost_read(&log2, &rs2, chunks[i].block, chunks[i].r0,
                                     chunks[i].rq, &sz);
        if (!got || sz != chunks[i].len || memcmp(got, chunks[i].data, chunks[i].len) != 0) ok = 0;
    }
    if (!ok) { printf("  (final mismatch)\n"); goto out_fail2; }

    /* ── §15.76 walk-based access: เดินนาฬิกาจาก state → หา route ที่ live ── */
    int walk_ok = walk_proof(cfg, &log2, &rs2, chunks, cfg->chunks);

    int secure = 1;
    uint32_t sz = 0;
    uint8_t wr0 = (uint8_t)((chunks[0].r0 + 1u) % cfg->cycles);
    uint8_t wrq = (uint8_t)((chunks[0].rq + 1u) % cfg->cycles);
    if (ghost_read(&log2, &rs2, chunks[0].block, wr0, chunks[0].rq, &sz) != NULL) secure = 0;
    if (ghost_read(&log2, &rs2, chunks[0].block, chunks[0].r0, wrq, &sz) != NULL) secure = 0;
    if (!secure) { printf("  (bond/route breach)\n"); goto out_fail2; }

    int log_ok = ((uint64_t)(lsz - 12u) == 5ull * log.count);
    int rs_ok  = (rs2.count == cfg->chunks);

    uint64_t total_img = CKPT_HEADER + rs_serialize_size(&rs2) + ghost_log_serialize_size(&log2);
    double overhead_pct = 100.0 * (double)(total_img - data_bytes) / (double)data_bytes;

    *out_image = total_img;
    *out_data  = data_bytes;

    int all_ok = ok && secure && log_ok && rs_ok && walk_ok;
    printf("  [%s] pipes=%u ticks=%u cycles=%u chunks=%u size=%u pat=%-8s dist=%u ckpt=%u\n",
           all_ok ? "PASS" : "FAIL",
           cfg->pipes, cfg->ticks, cfg->cycles, cfg->chunks, cfg->size,
           pat_name(cfg->pattern), cfg->dist, ckpt);
    printf("       data=%lluB image=%lluB overhead=%+5.2f%%  routes=%u wraps=%u rounds=%u/%u log=%lluB\n",
           (unsigned long long)data_bytes, (unsigned long long)total_img, overhead_pct,
           log.count, wraps, rounds_used, cfg->cycles,
           (unsigned long long)(ghost_log_serialize_size(&log2) - 12u));
    printf("       lossless=YES bond/route=SECURE log=5B/route=YES placed=%u/%u  economy=%s (thr %.1f%%)\n",
           placed, cfg->chunks, economy(overhead_pct, g_econ_thr), g_econ_thr);

    /* ── PERSIST: เขียน image + manifest ลงดิสก์ → spawn fresh process verify ── */
    if (persist_dir && all_ok) {
        uint64_t frsz = rs_serialize_size(&rs2);
        uint64_t flsz = ghost_log_serialize_size(&log2);
        uint64_t fsz  = CKPT_HEADER + frsz + flsz;
        uint8_t *fimg = (uint8_t *)malloc((size_t)fsz);
        if (fimg) {
            uint8_t *fp = fimg;
            memcpy(fp, &seed, 8); fp += 8;
            memcpy(fp, &ck, 8);   fp += 8;
            memcpy(fp, &tk, 4);   fp += 4;
            fp[0] = 1; fp[1] = 0; fp[2] = 0; fp[3] = 0;
            fp[4] = 0; fp[5] = 0; fp[6] = 0; fp[7] = 0; fp += 8;
            ghost_log_serialize(&log2, fp, flsz); fp += flsz;
            rs_serialize(&rs2, fp, frsz);         fp += frsz;
            char img_path[1024], cfg_path[1024];
            snprintf(img_path, sizeof(img_path), "%s/%s.img", persist_dir, tag);
            snprintf(cfg_path, sizeof(cfg_path), "%s/%s.cfg", persist_dir, tag);
            int w_ok = write_file_all(img_path, fimg, fsz) == 0 &&
                       manifest_write(cfg_path, cfg, fsz) == 0;
            free(fimg);
            if (w_ok) {
                int r = spawn_verify(self, img_path, cfg_path);
                if (r == 0)
                    printf("       disk=%s restore=FRESH-PROCESS-PASS\n", img_path);
                else {
                    printf("       disk=%s restore=MANUAL → run: %s --verify-img %s %s\n",
                           img_path, self, img_path, cfg_path);
                    all_ok = 0;
                }
            } else {
                printf("       disk write FAIL — %s\n", img_path);
                all_ok = 0;
            }
        }
    }

    p5h_ribcage_free(&rc); p5h_ribcage_free(&rc2);
    free(img); free(chunks);
    rs_free(&rs); rs_free(&rs2);
    return all_ok ? 1 : 0;

out_fail2:
    rs_free(&rs2);
out_fail:
    p5h_ribcage_free(&rc); p5h_ribcage_free(&rc2);
    free(chunks);
    rs_free(&rs);
    return 0;
}

/* ── sweep matrix: หลาย config ในรันเดียว + persist ทุกตัว ── */
static int run_sweep(const char *persist_dir, const char *self) {
    static const uint32_t cycles_vals[] = {16u, 72u, 144u};
    static const struct { uint32_t chunks, size; } data_vals[] = {
        {16u, 4096u}, {64u, 4096u}, {64u, 65536u}, {256u, 1024u} };
    static const int pats[] = {PAT_SCATTER, PAT_WRAP};

    if (persist_dir) ensure_dir(persist_dir);
    int pass = 0, fail = 0, n = 0;
    printf("── sweep: ตาราง×สนาม×ปริมาณ×รูปแบบ (+disk persist/restore) ──\n");
    for (size_t c = 0; c < sizeof(cycles_vals)/sizeof(cycles_vals[0]); c++)
    for (size_t d = 0; d < sizeof(data_vals)/sizeof(data_vals[0]); d++)
    for (size_t p = 0; p < sizeof(pats)/sizeof(pats[0]); p++) {
        Cfg cfg = { 1728u, 12u, cycles_vals[c], data_vals[d].chunks,
                    data_vals[d].size, 0u, 0u, pats[p], 0xC0FFEEu };
        if (pats[p] == PAT_WRAP) cfg.dist = cfg.cycles * 3u / 4u;
        else                     cfg.dist = 5u;
        uint64_t img = 0, data = 0;
        char tag[32];
        snprintf(tag, sizeof(tag), "ckpt_%02d", n);
        n++;
        if (run_config(&cfg, &img, &data, persist_dir, tag, self)) pass++; else fail++;
    }
    Cfg tv[3] = {
        { 512u, 12u, 144u, 64u, 4096u, 5u, 0u, PAT_SCATTER, 7u },
        { 1728u,  4u, 72u,  64u, 4096u, 5u, 0u, PAT_SCATTER, 7u },
        {  256u,  3u, 255u, 64u, 4096u, 5u, 0u, PAT_WRAP,    7u },
    };
    for (int i = 0; i < 3; i++) {
        uint64_t img = 0, data = 0;
        char tag[32];
        snprintf(tag, sizeof(tag), "ckpt_%02d", n);
        n++;
        if (run_config(&tv[i], &img, &data, persist_dir, tag, self)) pass++; else fail++;
    }
    printf("── sweep summary: %d/%d PASS%s ────────────────────────────\n",
           pass, n, persist_dir ? " (+ fresh-process disk restore)" : "");
    return fail == 0;
}

static int parse_cfg(Cfg *cfg, int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) continue;
        *eq = '\0';
        uint32_t v = (uint32_t)strtoul(eq + 1, NULL, 10);
        if      (strcmp(argv[i], "pipes")   == 0) cfg->pipes   = v;
        else if (strcmp(argv[i], "ticks")   == 0) cfg->ticks   = v;
        else if (strcmp(argv[i], "cycles")  == 0) cfg->cycles  = v;
        else if (strcmp(argv[i], "chunks")  == 0) cfg->chunks  = v;
        else if (strcmp(argv[i], "size")    == 0) cfg->size    = v;
        else if (strcmp(argv[i], "dist")    == 0) cfg->dist    = v;
        else if (strcmp(argv[i], "ckpt")    == 0) cfg->ckpt    = v;
        else if (strcmp(argv[i], "seed")    == 0) cfg->seed    = v;
        else if (strcmp(argv[i], "pattern") == 0) {
            if      (strcmp(eq + 1, "scatter") == 0) cfg->pattern = PAT_SCATTER;
            else if (strcmp(eq + 1, "cluster") == 0) cfg->pattern = PAT_CLUSTER;
            else if (strcmp(eq + 1, "allone")  == 0) cfg->pattern = PAT_ALLONE;
            else if (strcmp(eq + 1, "wrap")    == 0) cfg->pattern = PAT_WRAP;
            else if (strcmp(eq + 1, "random")  == 0) cfg->pattern = PAT_RANDOM;
            else { printf("unknown pattern '%s'\n", eq + 1); *eq = '='; return -1; }
        }
        *eq = '=';
    }
    if (cfg->pipes == 0 || cfg->ticks == 0 || cfg->cycles < 2 ||
        cfg->cycles > 255 || cfg->chunks == 0 || cfg->chunks > GHOST_LOG_MAX ||
        cfg->size == 0 || cfg->size > RS_MAX_DATA_SIZE) {
        printf("bad config: pipes>0 ticks>0 cycles∈[2,255] chunks∈[1,4096] size∈[1,65536]\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int sweep = 0;
    const char *persist_dir = NULL;
    const char *v_img = NULL, *v_cfg = NULL, *v_all = NULL;
    char econ_arg[64] = "";

    /* แยก flags ออกก่อน (key=value ที่เหลือเข้าสู่ config) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sweep") == 0) sweep = 1;
        else if (strncmp(argv[i], "--persist", 9) == 0) {
            if (argv[i][9] == '=') persist_dir = argv[i] + 10;
            else if (argv[i][9] == '\0') persist_dir = DEFAULT_CKPT_DIR;
        }
        else if (strcmp(argv[i], "--verify-img") == 0 && i + 2 < argc) {
            v_img = argv[i + 1];              /* แบบแยก args (spawn ใช้แบบนี้) */
            v_cfg = argv[i + 2];
            i += 2;
        }
        else if (strncmp(argv[i], "--verify-img=", 13) == 0) {
            char tmp[1024];
            snprintf(tmp, sizeof(tmp), "%s", argv[i] + 13);
            char *comma = strchr(tmp, ',');
            if (comma) {
                *comma = '\0';
                v_img = _strdup(tmp);
                v_cfg = _strdup(comma + 1);
            }
        }
        else if (strncmp(argv[i], "--verify-all", 12) == 0) {
            v_all = argv[i][12] == '=' ? argv[i] + 13 : DEFAULT_CKPT_DIR;
        }
        else if (strncmp(argv[i], "--economy", 9) == 0) {
            snprintf(econ_arg, sizeof(econ_arg), "%s",
                     argv[i][9] == '=' ? argv[i] + 10 : "2.0");
        }
    }
    if (econ_arg[0]) {
        double t = strtod(econ_arg, NULL);
        if (t > 0) g_econ_thr = t;
    }

    printf("fibo checkpoint-replay sweep — custom table / field / distance / volume / pattern\n");
    printf("══════════════════════════════════════════════════════════════════════\n");

    if (v_img && v_cfg) {
        int r = verify_img_mode(v_img, v_cfg);
        free((void *)v_img); free((void *)v_cfg);
        return r;
    }
    if (v_all) return verify_all_mode(v_all);

    Cfg cfg = { 1728u, 12u, 144u, 64u, 4096u, 5u, 0u, PAT_SCATTER, 0xC0FFEEu };
    if (parse_cfg(&cfg, argc, argv) != 0) return 2;

    if (sweep)   /* sweep เขียน disk + fresh-process restore เสมอ (default build/ckpt) */
        return run_sweep(persist_dir ? persist_dir : DEFAULT_CKPT_DIR, argv[0]) ? 0 : 1;

    if (persist_dir) ensure_dir(persist_dir);
    printf("── single config: pipes=%u ticks=%u cycles=%u chunks=%u size=%u pat=%s dist=%u ckpt=%u%s\n",
           cfg.pipes, cfg.ticks, cfg.cycles, cfg.chunks, cfg.size,
           pat_name(cfg.pattern), cfg.dist, cfg.ckpt ? cfg.ckpt : cfg.cycles / 2u,
           persist_dir ? "  [persist]" : "");
    uint64_t img = 0, data = 0;
    int ok = run_config(&cfg, &img, &data, persist_dir, "ckpt_00", argv[0]);
    printf("%s — config %s\n", ok ? "✅ PASS" : "❌ FAIL",
           ok ? (persist_dir ? "lossless + secure + disk-restore verified"
                             : "lossless + secure") : "FAILED");
    return ok ? 0 : 1;
}
