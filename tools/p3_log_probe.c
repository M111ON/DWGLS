/* P3 log format A/B probe. Run identical workload against fixed-32B (option A) and
 * variable-packed (option B) event log formats. Compare on:
 *   - bytes per event
 *   - append throughput (events/sec)
 *   - replay throughput (events/sec reading)
 *   - file-open latency (warm cache)
 *
 * The probe uses simple stdio fread/fwrite + rewind/noinseeking, no NAS, no threading.
 * Real workloads use mmap + Windows file ops; the ratio is what matters here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define N_EVENTS       50000      /* event count to benchmark */
#define N_REPLAY_WARM  1          /* replay runs to do per measurement */

/* ──── A: fixed 32 B record per event ──────────────────────── */
#define A_RECORD_SIZE  32
typedef struct {
    uint32_t slot;       /* address slot in [0, 20736) */
    uint64_t name_hash;  /* FNV-1a 64 of name */
    uint32_t walk_round;
    uint32_t walk_tick;
    uint32_t action;     /* 0=write, 1=clear, 2=move, 3=anchor */
    uint32_t reserved;
    uint32_t crc32;      /* crc32 over first 28 B */
} __attribute__((packed)) A_record_t;

static uint32_t crc32_simple(const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (-(c & 1u)));
    }
    return ~c;
}

static void A_pack(A_record_t* r,
                   uint32_t slot, uint64_t name_hash,
                   uint32_t walk_round, uint32_t walk_tick, uint32_t action) {
    r->slot = slot; r->name_hash = name_hash;
    r->walk_round = walk_round; r->walk_tick = walk_tick;
    r->action = action; r->reserved = 0;
    r->crc32 = 0;
    r->crc32 = crc32_simple(r, 28);
}

static int A_verify(const A_record_t* r) {
    uint32_t c = crc32_simple(r, 28);
    return c == r->crc32;
}

/* ──── B: variable packed record ──────────────────────────── *
 * Layout (21 B with CRC32):
 *   [0..4)  slot        (uint32 LE)
 *   [4..12) name_hash   (uint64 LE)
 *   [12..15) walk_round (uint24 LE, capped at 16M)
 *   [15]    walk_tick   (uint8, capped at 255)
 *   [16]    action      (uint8)
 *   [17..21) crc32      (4 B LE, over first 17 B)
 * Total: 21 B.
 */
#define B_RECORD_SIZE  21
typedef struct {
    uint8_t bytes[B_RECORD_SIZE];
} B_record_t;

static void B_pack(B_record_t* r,
                   uint32_t slot, uint64_t name_hash,
                   uint32_t walk_round, uint32_t walk_tick, uint32_t action) {
    /* slot: 4 B LE */
    r->bytes[0] = (uint8_t)(slot & 0xFFu);
    r->bytes[1] = (uint8_t)((slot >> 8) & 0xFFu);
    r->bytes[2] = (uint8_t)((slot >> 16) & 0xFFu);
    r->bytes[3] = (uint8_t)((slot >> 24) & 0xFFu);
    /* name_hash: 8 B LE */
    for (int i = 0; i < 8; i++) r->bytes[4 + i] = (uint8_t)((name_hash >> (i * 8)) & 0xFFu);
    /* walk_round: 24-bit LE (capped at 2^24 = 16M) */
    uint32_t wr_cap = walk_round & 0xFFFFFFu;
    r->bytes[12] = (uint8_t)(wr_cap & 0xFFu);
    r->bytes[13] = (uint8_t)((wr_cap >> 8) & 0xFFu);
    r->bytes[14] = (uint8_t)((wr_cap >> 16) & 0xFFu);
    /* walk_tick: 8 B */
    r->bytes[15] = (uint8_t)(walk_tick & 0xFFu);
    /* action: 8 B */
    r->bytes[16] = (uint8_t)(action & 0xFFu);
    /* crc32 over first 17 B, stored as 4 B LE */
    uint32_t c = crc32_simple(r->bytes, 17);
    r->bytes[17] = (uint8_t)(c & 0xFFu);
    r->bytes[18] = (uint8_t)((c >> 8) & 0xFFu);
    r->bytes[19] = (uint8_t)((c >> 16) & 0xFFu);
    r->bytes[20] = (uint8_t)((c >> 24) & 0xFFu);
}

static int B_verify(const B_record_t* r) {
    uint32_t c = crc32_simple(r->bytes, 17);
    uint32_t stored = ((uint32_t)r->bytes[17])
                    | ((uint32_t)r->bytes[18] << 8)
                    | ((uint32_t)r->bytes[19] << 16)
                    | ((uint32_t)r->bytes[20] << 24);
    return c == stored;
}

static void B_unpack(const B_record_t* r,
                     uint32_t* slot, uint64_t* name_hash,
                     uint32_t* walk_round, uint32_t* walk_tick, uint32_t* action) {
    *slot = ((uint32_t)r->bytes[0])
         | ((uint32_t)r->bytes[1] << 8)
         | ((uint32_t)r->bytes[2] << 16)
         | ((uint32_t)r->bytes[3] << 24);
    uint64_t nh = 0;
    for (int i = 0; i < 8; i++) nh |= ((uint64_t)r->bytes[4 + i]) << (i * 8);
    *name_hash = nh;
    *walk_round = ((uint32_t)r->bytes[12])
                | ((uint32_t)r->bytes[13] << 8)
                | ((uint32_t)r->bytes[14] << 16);
    *walk_tick = r->bytes[15];
    *action = r->bytes[16];
}

/* ──── workload generator ─────────────────────────────────── */
static uint64_t fnv1a_64_of_name(uint32_t idx) {
    uint64_t h = 0xCBF29CE484222325ULL;
    uint8_t  buf[16];
    memcpy(buf, "blk.", 4);
    snprintf((char*)buf, sizeof(buf), "blk.%u", idx);
    size_t n = strlen((char*)buf);
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)buf[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

static void gen_event(uint32_t i,
                      uint32_t* slot, uint64_t* name_hash,
                      uint32_t* walk_round, uint32_t* walk_tick, uint32_t* action) {
    *slot = (i * 37u) % 20736u;
    *name_hash = fnv1a_64_of_name(i);
    *walk_round = i;
    *walk_tick = (i * 7u) % 12u;
    *action = (i * 11u) % 4u;
}

/* ──── benchmark helpers ─────────────────────────────────── */
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void) {
    printf("=== P3 log format A/B probe ===\n");
    printf("workload: %d events, %d replay run(s)\n\n", N_EVENTS, N_REPLAY_WARM);

    /* ──── A: build, append, replay, measure ──────────────── */
    const char* pa = "I:\\DWGLS-native-fs\\build\\p3_log_a.bin";
    const char* pb = "I:\\DWGLS-native-fs\\build\\p3_log_b.bin";

    /* A: append throughput */
    {
        FILE* f = fopen(pa, "wb");
        if (!f) { perror("A open"); return 1; }
        A_record_t rec;
        double t0 = now_sec();
        for (uint32_t i = 0; i < N_EVENTS; i++) {
            uint32_t slot, walk_round, walk_tick, action;
            uint64_t name_hash;
            gen_event(i, &slot, &name_hash, &walk_round, &walk_tick, &action);
            A_pack(&rec, slot, name_hash, walk_round, walk_tick, action);
            fwrite(&rec, A_RECORD_SIZE, 1, f);
        }
        fclose(f);
        double dt = now_sec() - t0;
        printf("A append: %.3f s, %.0f events/s, %llu bytes total\n",
               dt, (double)N_EVENTS / dt, (unsigned long long)((size_t)N_EVENTS * A_RECORD_SIZE));
    }

    /* A: replay throughput + verify */
    {
        FILE* f = fopen(pa, "rb");
        if (!f) { perror("A reopen"); return 1; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        double t0 = now_sec();
        A_record_t rec;
        uint32_t ok = 0, fail = 0;
        for (uint32_t i = 0; i < N_EVENTS; i++) {
            if (fread(&rec, A_RECORD_SIZE, 1, f) != 1) break;
            if (A_verify(&rec)) ok++; else fail++;
        }
        double dt = now_sec() - t0;
        fclose(f);
        printf("A replay: %.3f s, %.0f events/s, %u ok / %u fail, file sz=%ld B\n",
               dt, (double)N_EVENTS / dt, ok, fail, sz);
    }

    /* ──── B: build, append, replay, measure ──────────────── */
    {
        FILE* f = fopen(pb, "wb");
        if (!f) { perror("B open"); return 1; }
        B_record_t rec;
        double t0 = now_sec();
        for (uint32_t i = 0; i < N_EVENTS; i++) {
            uint32_t slot, walk_round, walk_tick, action;
            uint64_t name_hash;
            gen_event(i, &slot, &name_hash, &walk_round, &walk_tick, &action);
            B_pack(&rec, slot, name_hash, walk_round, walk_tick, action);
            fwrite(&rec.bytes[0], B_RECORD_SIZE, 1, f);
        }
        fclose(f);
        double dt = now_sec() - t0;
        printf("B append: %.3f s, %.0f events/s, %llu bytes total\n",
               dt, (double)N_EVENTS / dt, (unsigned long long)((size_t)N_EVENTS * B_RECORD_SIZE));
    }

    {
        FILE* f = fopen(pb, "rb");
        if (!f) { perror("B reopen"); return 1; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        double t0 = now_sec();
        B_record_t rec;
        uint32_t ok = 0, fail = 0;
        for (uint32_t i = 0; i < N_EVENTS; i++) {
            if (fread(&rec.bytes[0], B_RECORD_SIZE, 1, f) != 1) break;
            if (B_verify(&rec)) ok++; else fail++;
        }
        double dt = now_sec() - t0;
        fclose(f);
        printf("B replay: %.3f s, %.0f events/s, %u ok / %u fail, file sz=%ld B\n",
               dt, (double)N_EVENTS / dt, ok, fail, sz);
    }

    /* ──── size comparison + recommendation ───────────────── */
    long size_a = (long)N_EVENTS * A_RECORD_SIZE;
    long size_b = (long)N_EVENTS * B_RECORD_SIZE;
    double saving_pct = 100.0 * (1.0 - (double)size_b / (double)size_a);
    printf("\nsize deltas: A=%ld B, B=%ld B, B saves %.1f%%\n", size_a, size_b, saving_pct);
    printf("per-event:  A=%d B, B=%d B\n", A_RECORD_SIZE, B_RECORD_SIZE);

    /* disk usage on the build dir */
    {
        FILE* fa = fopen(pa, "rb");
        FILE* fb = fopen(pb, "rb");
        if (fa && fb) {
            fseek(fa, 0, SEEK_END); long sa = ftell(fa); fclose(fa);
            fseek(fb, 0, SEEK_END); long sb = ftell(fb); fclose(fb);
            printf("on-disk:    A=%ld B, B=%ld B\n", sa, sb);
        }
    }

    printf("\n=== probe done ===\n");
    return 0;
}