/* kv_lease_cli.c — kv_lease across PROCESS boundaries (the subagent binding).
 *
 * core/kv_lease.h has no consumer inside the KV path, and that is correct: its
 * consumer is a *delegated worker*, a different process. This tool is that
 * binding — a shared lease table on disk, edited under one OS lock (the same
 * style the producer claim uses), so:
 *
 *   parent:  init -> (spawn children) -> reap -> status
 *   child:   reserve -> write/beat ... -> close        (or gets killed)
 *
 * What real processes prove that the unit test cannot:
 *   - a child that stops beating loses its block to the parent's reap, and its
 *     payload stays recorded (close, not free);
 *   - a child that is killed gets no special treatment: the OS drops its file
 *     lock, lease expiry does the rest, and the reap is a normal reap.
 *
 * On disk: [magic|version|sizeof(kvb_table)][kvb_table]. A size mismatch is
 * refused rather than reinterpreted — the struct is the contract.
 */
#include "kv_lease.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/locking.h>
#define L_OPEN(p)   _open((p), _O_CREAT | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE)
#define L_LOCK(fd)  (_lseek((fd), 0, SEEK_SET), _locking((fd), _LK_LOCK, 1))
#define L_UNLOCK(fd) (_lseek((fd), 0, SEEK_SET), _locking((fd), _LK_UNLCK, 1))
#define L_CLOSE(fd) _close(fd)
#define L_SLEEP(ms) Sleep(ms)
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#define L_OPEN(p)   open((p), O_CREAT | O_RDWR, 0644)
#define L_LOCK(fd)  flock((fd), LOCK_EX)
#define L_UNLOCK(fd) flock((fd), LOCK_UN)
#define L_CLOSE(fd) close(fd)
#define L_SLEEP(ms) usleep((ms) * 1000)
#endif

#define LEASE_MAGIC "KVB1TABLE"
typedef struct { char magic[10]; uint32_t version; uint32_t table_size; } lease_hdr;

typedef struct {
    int       lfd;
    int       create;
    int       created;
    char      path[480];
    char      lock[512];
    kvb_table t;
} lease_ctx;

static int64_t now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e3 / (double)f.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void store(lease_ctx *c) {
    FILE *w = fopen(c->path, "wb");
    if (!w) { perror("table write"); return; }
    lease_hdr h; memset(&h, 0, sizeof h);
    memcpy(h.magic, LEASE_MAGIC, 10);
    h.version = 1;
    h.table_size = (uint32_t)sizeof(kvb_table);
    if (fwrite(&h, sizeof h, 1, w) != 1 || fwrite(&c->t, sizeof c->t, 1, w) != 1)
        fprintf(stderr, "lease: short write to %s\n", c->path);
    fclose(w);
}

static void release(lease_ctx *c) {
    if (c->lfd >= 0) { L_UNLOCK(c->lfd); L_CLOSE(c->lfd); c->lfd = -1; }
}

/* Load under lock. On success the caller mutates c->t and then either calls
 * commit() (store + unlock) or abort() (unlock, discard). */
static int open_table(lease_ctx *c, const char *path, int create,
                      int32_t total, int32_t quota, int64_t lease_ms) {
    memset(c, 0, sizeof *c);
    c->lfd = -1;
    c->create = create;
    snprintf(c->path, sizeof c->path, "%s", path);
    snprintf(c->lock, sizeof c->lock, "%s.lock", path);
    c->lfd = L_OPEN(c->lock);
    if (c->lfd < 0) { perror("lock file"); return -1; }
    if (L_LOCK(c->lfd) != 0) { fprintf(stderr, "lease: cannot take the table lock\n"); return -1; }

    int have = 0;
    FILE *f = fopen(c->path, "rb");
    if (f) {
        lease_hdr h;
        if (fread(&h, sizeof h, 1, f) == 1 && memcmp(h.magic, LEASE_MAGIC, 10) == 0) {
            if (h.table_size != (uint32_t)sizeof(kvb_table)) {
                fprintf(stderr, "lease: table layout is %u bytes, this build expects %u — "
                                "refusing to reinterpret it\n",
                        h.table_size, (unsigned)sizeof(kvb_table));
                fclose(f); release(c); return -1;
            }
            if (fread(&c->t, sizeof c->t, 1, f) == 1) have = 1;
        }
        fclose(f);
    }
    if (!have) {
        if (!create) {
            fprintf(stderr, "lease: no table at %s (run `init` first)\n", c->path);
            release(c); return -1;
        }
        kvb_init(&c->t, total, quota, 64, 4096, 1024, lease_ms);
        c->created = 1;
    }
    return 0;
}

static void commit(lease_ctx *c) { store(c); release(c); }
static void abort_tx(lease_ctx *c) { release(c); }

static void print_status(const kvb_table *t) {
    printf("lease: %d/%d slots held, quota %d [%d..%d], floor %d, lease %" PRId64 " ms, "
           "borrows %d, reaps %d, quota steps %d, round %d\n",
           t->held, t->total, t->quota, t->quota_min, t->quota_max, t->main_floor,
           t->lease_ms, t->borrows, t->reap_events, t->quota_steps, t->round);
    for (int i = 0; i < KVB_MAX_BLOCKS; i++) {
        const kvb_block *b = &t->b[i];
        if (b->state == KVB_FREE) continue;
        printf("  [%2d] owner %-3d %-8s slots %-5d written %-5d gen %-3u\n",
               i, b->owner,
               b->state == KVB_RESERVED ? "RESERVED" :
               b->state == KVB_ACTIVE   ? "ACTIVE"   : "CLOSED",
               b->slots, b->written_slots, b->generation);
    }
}

static int usage(void) {
    fprintf(stderr,
        "usage: kv_lease_cli <cmd> <table> [args]\n"
        "  init <total> <quota> <lease_ms>\n"
        "  reserve <owner> <slots> [--borrow]   -> prints \"idx gen\"\n"
        "  write <idx> <gen> <slots>\n"
        "  beat  <idx> <gen>\n"
        "  close <idx> <gen>\n"
        "  reap\n"
        "  status\n"
        "  hold <owner> <slots> <ms> <beat_every_ms>\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 3) return usage();
    const char *cmd = argv[1], *table = argv[2];
    lease_ctx c;

    if (strcmp(cmd, "init") == 0) {
        if (argc < 6) return usage();
        remove(table);
        if (open_table(&c, table, 1, atoi(argv[3]), atoi(argv[4]), (int64_t)atoll(argv[5])) < 0)
            return 1;
        commit(&c);
        print_status(&c.t);
        return 0;
    }

    if (strcmp(cmd, "status") == 0) {
        if (open_table(&c, table, 0, 0, 0, 0) < 0) return 1;
        print_status(&c.t);
        abort_tx(&c);
        return 0;
    }

    if (strcmp(cmd, "reap") == 0) {
        if (open_table(&c, table, 0, 0, 0, 0) < 0) return 1;
        int64_t now = now_ms();
        int64_t oldest = kvb_oldest_live_ms(&c.t, now);
        int n = kvb_reap(&c.t, now);
        commit(&c);
        printf("reaped %d (lease %" PRId64 " ms, oldest live before reap %" PRId64 " ms)\n",
               n, c.t.lease_ms, oldest);
        print_status(&c.t);
        return 0;
    }

    if (strcmp(cmd, "reserve") == 0) {
        if (argc < 5) return usage();
        int32_t owner = atoi(argv[3]), slots = atoi(argv[4]);
        int borrow = (argc > 5 && strcmp(argv[5], "--borrow") == 0);
        if (open_table(&c, table, 0, 0, 0, 0) < 0) return 1;
        kvb_ref r;
        int rc = kvb_reserve(&c.t, owner, slots, now_ms(), borrow, &r);
        if (rc != KVB_OK) { abort_tx(&c); printf("refused rc=%d\n", rc); return 1; }
        commit(&c);
        printf("%d %u\n", r.idx, r.generation);
        return 0;
    }

    if (strcmp(cmd, "write") == 0 || strcmp(cmd, "beat") == 0 || strcmp(cmd, "close") == 0) {
        if (argc < 5) return usage();
        kvb_ref r; r.idx = atoi(argv[3]); r.generation = (uint32_t)strtoul(argv[4], NULL, 10);
        if (open_table(&c, table, 0, 0, 0, 0) < 0) return 1;
        int64_t now = now_ms(); int rc;
        if (strcmp(cmd, "write") == 0)      rc = kvb_write(&c.t, r, argc > 5 ? atoi(argv[5]) : 0, now);
        else if (strcmp(cmd, "beat") == 0)  rc = kvb_beat(&c.t, r, now);
        else                                rc = kvb_close(&c.t, r, now);
        if (rc != KVB_OK) {
            uint32_t seen = 0; kvb_check(&c.t, r, &seen);
            abort_tx(&c);
            printf("refused rc=%d (handle gen %u, block gen %u)\n", rc, r.generation, seen);
            return 1;
        }
        commit(&c);
        printf("ok %s\n", cmd);
        return 0;
    }

    if (strcmp(cmd, "hold") == 0) {
        if (argc < 7) return usage();
        int32_t owner = atoi(argv[3]), slots = atoi(argv[4]);
        int64_t run_ms = (int64_t)atoll(argv[5]), every = (int64_t)atoll(argv[6]);
        if (open_table(&c, table, 0, 0, 0, 0) < 0) return 1;
        kvb_ref r;
        int rc = kvb_reserve(&c.t, owner, slots, now_ms(), 1, &r);
        if (rc != KVB_OK) { abort_tx(&c); printf("hold: reserve refused rc=%d\n", rc); return 1; }
        kvb_write(&c.t, r, slots / 2, now_ms());
        commit(&c);
        printf("hold: idx %d gen %u slots %d for %" PRId64 " ms (beat every %" PRId64 " ms)\n",
               r.idx, r.generation, slots, run_ms, every);
        fflush(stdout);

        int64_t t0 = now_ms();
        while (now_ms() - t0 < run_ms) {
            L_SLEEP((unsigned)every);
            lease_ctx b;
            if (open_table(&b, table, 0, 0, 0, 0) < 0) break;
            int brc = kvb_beat(&b.t, r, now_ms());
            if (brc == KVB_OK) { commit(&b); continue; }
            uint32_t seen = 0; kvb_check(&b.t, r, &seen);
            abort_tx(&b);
            printf("hold: lease lost rc=%d (handle gen %u, block gen %u)\n", brc, r.generation, seen);
            return 1;
        }
        lease_ctx d;
        if (open_table(&d, table, 0, 0, 0, 0) == 0) { kvb_close(&d.t, r, now_ms()); commit(&d); }
        printf("hold: closed\n");
        return 0;
    }

    return usage();
}
