/* ═══════════════════════════════════════════════════════════════════════════
 * geofs_mdim.h — GeoFS Multidimensional Native Volume
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * ONE address space (20736 slots × 64 B), FOUR views, ONE timeline journal.
 *
 *   "Coordinate = address. No hash. No lookup table."
 *
 * ── THE VOLUME ──────────────────────────────────────────────────────────
 *   bytes[20736 × 64] — the whole filesystem is one contiguous buffer,
 *   memory-mappable (CreateFileMappingA / mmap). Slot N lives at byte
 *   N×64. Nothing is stored twice: every table is derived on open.
 *
 * ── THE FOUR VIEWS (pure arithmetic over the same bytes) ────────────────
 *   flat   : slot 0..20735
 *   cube   : (gen, face, slot)  — 3-bit/3-bit/8-bit pack (14-bit subspace)
 *   rail   : (tick, pipe)       — tick×1728 + pipe   (bijective)
 *   time   : frame              — frame×37 % 20736   (bijective, stride-37)
 *   cell   : (cell, slot)       — cell×144 + slot    (6ico: 144×144)
 *
 *   The SAME bytes are reachable through every view — reading through one
 *   axis is a coordinate transform, never a copy.
 *
 * ── NAME BONDING (no hash, no LUT) ──────────────────────────────────────
 *   bond(name)  = base-3 trit fold of the name bytes, mod 144.
 *   probe(k)    = DATA_START + ((bond + k×37) % span)
 *   A file's NAME ENTRY lives at the first free probe position. Lookup
 *   walks the same stride — the space itself is the table; collisions are
 *   structural; tombstones keep probe chains alive.
 *
 * ── RUN-SPAN CHAINS (multi-frame files) ─────────────────────────────────
 *   A file = chain of runs: [LINK][DATA...] → [LINK][DATA...] → ... .
 *   Each run is one contiguous first-fit span; its LINK slot (at the run's
 *   start) holds {size = this run's bytes, prev = next run's LINK (0 = last)}.
 *   The FILE entry points at the first LINK and carries a run-span header
 *   (n_runs, n_data_slots) — coordinate = address, the chain is the extent.
 *
 *   CHUNKED COMMITS: files larger than one journal frame are committed one
 *   run per frame (each ≤ 62 changes). The entry is written in the LAST
 *   frame — a crash mid-chain leaves no visible file (orphaned runs are
 *   swept by rebuild on the next open). Rewrites re-layout the whole chain:
 *   new chain written first, entry switched in one atomic frame, old chain
 *   freed last — a crash at any point leaves the file fully old or fully
 *   new, never torn. The free bitmap is DERIVED from entry chains, so
 *   orphaned blocks after a crash self-heal on load.
 *
 * ── TIMELINE JOURNAL (write-ahead undo log) ─────────────────────────────
 *   Every mutation is one atomic op (multi-frame ops commit one frame per
 *   run):
 *     1. capture before-images of every slot to be touched (in memory)
 *     2. write the whole frame {slot_idx, before[64]} to the journal ring
 *        at the tail — BEFORE any base byte changes (write-ahead)
 *     3. mutate the base
 *     4. stamp the frame COMMITTED (frame_no + CRC)
 *
 *   When the ring can't fit the next frame, it compacts: every retained
 *   frame is checkpointed away and the ring restarts. History depth = one
 *   ring's worth — the timeline is a ring, not an archive.
 *
 *     recover()   : undo the uncommitted frame at the tail (crash between
 *                   steps 2–4); a CRC failure on a committed frame OR a
 *                   torn uncommitted frame is fail-loud MDIM_ERR_CORRUPT
 *                   (never a partial rollback).
 *     state_at(F) : base − undo(frames > F) → the volume exactly as of
 *                   commit F. Versions are free; the timeline IS history.
 *                   F ≤ checkpoint_frame = evicted (MDIM_ERR_EVICTED).
 *
 * ── SLOT LAYOUT (64 B) ──────────────────────────────────────────────────
 *   type: 0 EMPTY · 1 FILE(name entry) · 2 DIR(reserved) · 3 TOMB ·
 *         4 SUPER · 5 JRNL · 6 DATA · 7 LINK(run header)
 *
 *   FILE entry : [type|flags|pad|size|crc32|name(24)|home_frame|prev|
 *                 n_runs|n_data_slots]  prev = first run's LINK slot.
 *   LINK slot  : [type|size(this run's bytes)|prev(next run's LINK)].
 *   DATA slot  : [type(1) | data[63]] → 63 B of file bytes per slot.
 *
 *   Journal frame: 1 header slot + 2 slots per change {idx, before[64]};
 *   max 62 changes per frame (1 + 2×62 = 125 ≤ 128 ring slots).
 *
 * Self-contained: stdint/string/stdio/stdlib only. No malloc in hot path.
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef GEOFS_MDIM_H
#define GEOFS_MDIM_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <unistd.h>
#endif

/* ═══════════════ SACRED CONSTANTS ═══════════════ */
#define MDIM_SLOTS          20736u
#define MDIM_SLOT_SZ        64u
#define MDIM_VOL_BYTES      (MDIM_SLOTS * MDIM_SLOT_SZ)          /* 1,327,104 */
#define MDIM_STRIDE         37u            /* coprime with 20736 */
#define MDIM_PIPES          1728u
#define MDIM_TICKS          12u
#define MDIM_CELLS          144u           /* 6ico macro cells */
#define MDIM_CELL_SLOTS     144u           /* 144 × 144 = 20736 */
#define MDIM_MAX_NAME       24u
#define MDIM_MAX_PROBE      4096u          /* fail-loud probe cap */

/* super region: slot 0 · journal ring: slots 1..128 · data: slots 129.. */
#define MDIM_JRNL_START     1u
#define MDIM_JRNL_SLOTS     128u
#define MDIM_JRNL_END       (MDIM_JRNL_START + MDIM_JRNL_SLOTS)
#define MDIM_DATA_START     129u
#define MDIM_DATA_SPAN      (MDIM_SLOTS - MDIM_DATA_START)
#define MDIM_FRAME_NONE     0xFFFFFFFFu
#define MDIM_MAX_CHANGES    62u            /* 1 + 2×62 = 125 ≤ 128 */
#define MDIM_FRAME_CAP      (1u + 2u * MDIM_MAX_CHANGES)
#define MDIM_DATA_SLOT_BYTES 63u           /* type byte + 63 data */

/* super field offsets (raw bytes in slot 0) */
#define MDIM_SUPER_MAGIC    0u
#define MDIM_SUPER_VERSION  4u
#define MDIM_SUPER_FLAGS    6u
#define MDIM_SUPER_N_FILES  8u
#define MDIM_SUPER_N_DIRS   12u
#define MDIM_SUPER_N_USED   16u
#define MDIM_SUPER_JRNL_HEAD 20u
#define MDIM_SUPER_CKPT     24u
#define MDIM_SUPER_FRAME    28u
#define MDIM_SUPER_MAXPROBE 32u

/* journal frame header offsets (inside its header slot) */
#define MDIM_FH_MAGIC       0u
#define MDIM_FH_FLAGS       1u
#define MDIM_FH_FRAME_NO    4u
#define MDIM_FH_PREV        8u
#define MDIM_FH_CRC         12u
#define MDIM_FH_N_CHANGES   16u

#define MDIM_JMAGIC         0x4Au
#define MDIM_JFLAG_COMMITTED 0x01u

/* Test-only fault injection (empty by default — zero behavior change). A
 * test may define MDIM_CRASH_HOOK(v, stage) before including this header
 * to snapshot the volume at every journal stage boundary of a mutation:
 *   stage 1 = frame materialized (write-ahead), base not yet touched
 *   stage 2 = base mutated, frame still uncommitted (rollback window)
 *   stage 3 = frame committed
 * The snapshot is the on-disk state after a power loss at that exact
 * instruction boundary — RAM state (bitmap, counters, staged changes) is
 * gone, and a reload must derive everything from the bytes alone. */
#ifndef MDIM_CRASH_HOOK
#define MDIM_CRASH_HOOK(v, stage) ((void)0)
#endif

/* slot types */
#define MDIM_T_EMPTY        0u
#define MDIM_T_FILE         1u
#define MDIM_T_DIR          2u   /* reserved */
#define MDIM_T_TOMB         3u
#define MDIM_T_SUPER        4u
#define MDIM_T_JRNL         5u
#define MDIM_T_DATA         6u
#define MDIM_T_LINK         7u   /* run-link: {size=this run's bytes, prev=next run start} */

/* FILE entry flags */
#define MDIM_F_CHAIN        0x01u  /* multi-run file (n_runs > 1) */

/* run-span geometry: a file = chain of runs, each run = [LINK][DATA...] */
#define MDIM_RUN_CHUNK      60u    /* max data slots per run (fits one frame w/ link+entry) */
#define MDIM_RUN_BYTES      (MDIM_RUN_CHUNK * MDIM_DATA_SLOT_BYTES)  /* 3780 B */
#define MDIM_MAX_FILE_BYTES (1024u * 1024u)  /* hard cap: 1 MiB (volume-bounded) */
#define MDIM_CAP_ONE_FRAME  ((MDIM_MAX_CHANGES - 2u) * MDIM_DATA_SLOT_BYTES) /* 3780 = one-frame */
#define MDIM_MAX_RUNS       (MDIM_MAX_FILE_BYTES / MDIM_RUN_BYTES + 2u)      /* walk guard */

/* error codes */
#define MDIM_OK             0
#define MDIM_ERR_EXISTS    -1
#define MDIM_ERR_NOENT     -2
#define MDIM_ERR_NOSPC     -3
#define MDIM_ERR_FULL      -4   /* journal frame full — commit first */
#define MDIM_ERR_CORRUPT   -5   /* fail-loud: committed frame CRC failed */
#define MDIM_ERR_EVICTED   -6   /* frame evicted below checkpoint */
#define MDIM_ERR_SIZE      -7
#define MDIM_ERR_ARG       -8
#define MDIM_ERR_IO        -9
#define MDIM_ERR_PROBE     -10  /* probe chain exhausted */

/* ═══════════════ STRUCTS ═══════════════ */

typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint16_t pad;
    uint32_t size;        /* file bytes (FILE entry) / this run's bytes (LINK) */
    uint32_t crc32;       /* crc of the file bytes (FILE entry) */
    char     name[MDIM_MAX_NAME];
    uint32_t home_frame;
    uint32_t prev;        /* FILE entry → first run's LINK slot · LINK → next run's LINK (0 = last) */
    uint32_t n_runs;      /* run-span header: number of runs in the chain */
    uint32_t n_data_slots;/* run-span header: total data slots across all runs */
    uint8_t  payload[12];
} MdimSlot;               /* 1+1+2+4+4+24+4+4+4+4+12 = 64 ✓ */

typedef struct {
    uint8_t *bytes;       /* whole volume (owned or mmap'd) */
    int      is_mapped;
    int      owns_bytes;
    void    *h_file;      /* platform handles */
    void    *h_map;
    /* derived state (rebuilt on open / recover — nothing stored twice) */
    uint32_t n_files;
    uint32_t n_blocks_used;
    uint32_t journal_head;    /* header slot of last committed frame */
    uint32_t checkpoint_frame; /* oldest retained frame_no (evicted below) */
    uint32_t frame_counter;   /* next frame_no to assign */
    uint32_t jrnl_cursor;     /* next free ring slot (tail) */
    uint32_t jrnl_n_changes;  /* changes staged for the current op */
    uint32_t staged_slot[MDIM_MAX_CHANGES];
    uint8_t  staged_before[MDIM_MAX_CHANGES][MDIM_SLOT_SZ];
    uint32_t max_probe;
    uint8_t  bitmap[MDIM_SLOTS / 8];
} MdimVolume;

typedef struct {
    uint32_t entry;       /* name-entry slot (FILE) */
    uint32_t run_start;   /* first data slot */
    uint32_t size;
} MdimFile;

typedef enum {
    MDIM_VIEW_FLAT = 0,
    MDIM_VIEW_CUBE = 1,
    MDIM_VIEW_RAIL = 2,
    MDIM_VIEW_TIME = 3,
    MDIM_VIEW_CELL = 4
} MdimView;

/* ═══════════════ SUPER ACCESSORS ═══════════════ */

static inline uint32_t mdim_u32(const uint8_t *m, uint32_t off) {
    return (uint32_t)m[off] | ((uint32_t)m[off+1] << 8) |
           ((uint32_t)m[off+2] << 16) | ((uint32_t)m[off+3] << 24);
}
static inline void mdim_wu32(uint8_t *m, uint32_t off, uint32_t v) {
    m[off] = (uint8_t)v; m[off+1] = (uint8_t)(v >> 8);
    m[off+2] = (uint8_t)(v >> 16); m[off+3] = (uint8_t)(v >> 24);
}
static inline uint16_t mdim_u16(const uint8_t *m, uint32_t off) {
    return (uint16_t)((uint16_t)m[off] | ((uint16_t)m[off+1] << 8));
}
static inline void mdim_wu16(uint8_t *m, uint32_t off, uint16_t v) {
    m[off] = (uint8_t)v; m[off+1] = (uint8_t)(v >> 8);
}
static inline MdimSlot *mdim_slot(MdimVolume *v, uint32_t flat) {
    return (MdimSlot *)&v->bytes[flat * MDIM_SLOT_SZ];
}
static inline uint8_t *mdim_slot_bytes(MdimVolume *v, uint32_t flat) {
    return &v->bytes[flat * MDIM_SLOT_SZ];
}

/* ═══════════════ CRC32 (bitwise, no table) ═══════════════ */

static inline uint32_t mdim_crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* ═══════════════ THE FOUR VIEWS — pure arithmetic ═══════════════ */

static inline const char *mdim_view_name(MdimView view) {
    switch (view) {
        case MDIM_VIEW_FLAT: return "flat";
        case MDIM_VIEW_CUBE: return "cube";
        case MDIM_VIEW_RAIL: return "rail";
        case MDIM_VIEW_TIME: return "time";
        case MDIM_VIEW_CELL: return "cell";
    }
    return "?";
}

/* inverse of 37 mod 20736 (extended Euclid; gcd(37,20736)=1 → exists) */
static inline uint32_t mdim_inv37(void) {
    int64_t t = 0, nt = 1, r = 20736, nr = 37;
    while (nr != 0) {
        int64_t q = r / nr;
        int64_t tmp = t; t = nt; nt = tmp - q * nt;
        tmp = r; r = nr; nr = tmp - q * nr;
    }
    if (t < 0) t += 20736;
    return (uint32_t)t;
}

/* view → flat slot index (a,b,c = view coordinates) */
static inline uint32_t mdim_view_flat(MdimView view, uint32_t a, uint32_t b, uint32_t c) {
    switch (view) {
        case MDIM_VIEW_FLAT: return a % MDIM_SLOTS;
        case MDIM_VIEW_CUBE:
            return ((a & 7u) | ((b & 7u) << 3) | ((c & 0xFFu) << 6)) % MDIM_SLOTS;
        case MDIM_VIEW_RAIL:
            return ((a % MDIM_TICKS) * MDIM_PIPES + (b % MDIM_PIPES)) % MDIM_SLOTS;
        case MDIM_VIEW_TIME:
            return (a * MDIM_STRIDE) % MDIM_SLOTS;
        case MDIM_VIEW_CELL:
            return ((a % MDIM_CELLS) * MDIM_CELL_SLOTS + (b % MDIM_CELL_SLOTS)) % MDIM_SLOTS;
    }
    return 0;
}

/* flat → view coordinates */
static inline void mdim_view_coords(MdimView view, uint32_t flat,
                                    uint32_t *a, uint32_t *b, uint32_t *c) {
    flat %= MDIM_SLOTS;
    switch (view) {
        case MDIM_VIEW_FLAT:
            *a = flat; *b = 0; *c = 0; break;
        case MDIM_VIEW_CUBE:
            *a = flat & 7u; *b = (flat >> 3) & 7u; *c = (flat >> 6) & 0xFFu; break;
        case MDIM_VIEW_RAIL:
            *a = flat / MDIM_PIPES;          /* tick */
            *b = flat % MDIM_PIPES;          /* pipe */
            *c = 0; break;
        case MDIM_VIEW_TIME:
            *a = (flat * mdim_inv37()) % MDIM_SLOTS;  /* frame */
            *b = 0; *c = 0; break;
        case MDIM_VIEW_CELL:
            *a = flat / MDIM_CELL_SLOTS;     /* cell */
            *b = flat % MDIM_CELL_SLOTS;     /* slot within cell */
            *c = 0; break;
    }
}

/* number of distinct addresses a view can express */
static inline uint32_t mdim_view_space(MdimView view) {
    return (view == MDIM_VIEW_CUBE) ? 16384u : MDIM_SLOTS;
}

/* ═══════════════ NAME BONDING (no hash, no LUT) ═══════════════ */

/* base-3 trit fold of the name bytes, mod 144 (cell granularity) */
static inline uint16_t mdim_bond(const char *name) {
    uint32_t h = 0;
    const uint8_t *p = (const uint8_t *)name;
    for (uint32_t i = 0; i < MDIM_MAX_NAME && p[i]; i++)
        h = (h * 3u + p[i]) & 0xFFFFu;
    return (uint16_t)(h % MDIM_CELLS);
}

/* probe position k for a name (stays inside the data region) */
static inline uint32_t mdim_probe_pos(uint16_t bond, uint32_t k) {
    return MDIM_DATA_START + ((bond + k * MDIM_STRIDE) % MDIM_DATA_SPAN);
}

/* find a name by walking its stride chain; returns entry slot or NONE */
static inline uint32_t mdim_find_slot(MdimVolume *v, const char *name, uint32_t *out_k) {
    uint16_t b = mdim_bond(name);
    for (uint32_t k = 0; k < MDIM_MAX_PROBE; k++) {
        uint32_t pos = mdim_probe_pos(b, k);
        MdimSlot *s = mdim_slot(v, pos);
        if (s->type == MDIM_T_EMPTY) break;                 /* never placed */
        if (s->type == MDIM_T_TOMB) continue;               /* freed — keep walking */
        if (s->type == MDIM_T_FILE && s->name[0] &&
            strncmp(s->name, name, MDIM_MAX_NAME) == 0) {
            if (out_k) *out_k = k;
            return pos;
        }
        /* DATA slot or another name → keep walking */
    }
    return MDIM_FRAME_NONE;
}

/* ═══════════════ BITMAP (derived on open; the only allocator) ═══════════════ */

static inline int mdim_bit_get(MdimVolume *v, uint32_t flat) {
    return (v->bitmap[flat >> 3] >> (flat & 7)) & 1u;
}
static inline void mdim_bit_set(MdimVolume *v, uint32_t flat, int used) {
    if (used) v->bitmap[flat >> 3] |= (uint8_t)(1u << (flat & 7));
    else      v->bitmap[flat >> 3] &= (uint8_t)~(1u << (flat & 7));
}

static inline int mdim_run_free(MdimVolume *v, uint32_t start, uint32_t n) {
    if (start + n > MDIM_SLOTS) return 0;
    for (uint32_t i = start; i < start + n; i++)
        if (mdim_bit_get(v, i)) return 0;
    return 1;
}

/* first-fit contiguous run in the data region */
static inline uint32_t mdim_alloc_run(MdimVolume *v, uint32_t n) {
    uint32_t run = 0;
    for (uint32_t i = MDIM_DATA_START; i + n <= MDIM_SLOTS; i++) {
        if (mdim_bit_get(v, i)) { run = 0; continue; }
        if (run == 0) run = i;
        if (i - run + 1 >= n) return run;
    }
    return MDIM_FRAME_NONE;
}

/* ═══════════════ JOURNAL — write-ahead undo log ═══════════════ */

static inline uint8_t *mdim_frame_hdr(MdimVolume *v, uint32_t slot) {
    return mdim_slot_bytes(v, slot);
}
static inline uint32_t mdim_frame_n_changes(const uint8_t *hdr) {
    return mdim_u16(hdr, MDIM_FH_N_CHANGES);
}
static inline uint32_t mdim_frame_span(uint32_t n_changes) {
    return 1u + 2u * n_changes;
}

/* last committed frame_no (or MDIM_FRAME_NONE) */
static inline uint32_t mdim_last_frame(MdimVolume *v) {
    if (v->journal_head == MDIM_FRAME_NONE) return MDIM_FRAME_NONE;
    return mdim_u32(mdim_frame_hdr(v, v->journal_head), MDIM_FH_FRAME_NO);
}

/*
 * mdim_frame_commit — stage 4: stamp the frame at the tail COMMITTED with
 * frame_no + CRC. Call AFTER the base mutation. frame_no = NONE → auto.
 * Returns the committed frame_no (or MDIM_FRAME_NONE when nothing staged).
 */
static inline uint32_t mdim_frame_commit(MdimVolume *v, uint32_t frame_no) {
    if (v->jrnl_n_changes == 0) return MDIM_FRAME_NONE;
    if (frame_no == MDIM_FRAME_NONE) frame_no = v->frame_counter;

    uint32_t span = mdim_frame_span(v->jrnl_n_changes);
    uint32_t hdr_slot = v->jrnl_cursor - span;
    uint8_t *hdr = mdim_frame_hdr(v, hdr_slot);

    uint8_t newhdr[MDIM_SLOT_SZ];
    memcpy(newhdr, hdr, MDIM_SLOT_SZ);
    mdim_wu32(newhdr, MDIM_FH_FRAME_NO, frame_no);
    mdim_wu32(newhdr, MDIM_FH_CRC, 0);
    newhdr[MDIM_FH_FLAGS] = MDIM_JFLAG_COMMITTED;
    memcpy(hdr, newhdr, MDIM_SLOT_SZ);
    uint32_t crc = mdim_crc32(mdim_slot_bytes(v, hdr_slot), span * MDIM_SLOT_SZ);
    mdim_wu32(hdr, MDIM_FH_CRC, crc);

    v->journal_head = hdr_slot;
    v->frame_counter = frame_no + 1;
    v->jrnl_n_changes = 0;

    mdim_wu32(v->bytes, MDIM_SUPER_JRNL_HEAD, v->journal_head);
    mdim_wu32(v->bytes, MDIM_SUPER_CKPT, v->checkpoint_frame);
    mdim_wu32(v->bytes, MDIM_SUPER_FRAME, v->frame_counter);
    return frame_no;
}

/* reset the pending change set for a new op. If a previous op left an
 * uncommitted frame (write-then-crash window), commit it first — at most
 * one uncommitted frame exists, always at the tail. */
static inline void mdim_pending_begin(MdimVolume *v) {
    if (v->jrnl_n_changes > 0)
        mdim_frame_commit(v, MDIM_FRAME_NONE);
    v->jrnl_n_changes = 0;
}

/* capture a before-image of slot_idx (write-ahead stage 1) */
static inline int mdim_pending_add(MdimVolume *v, uint32_t slot_idx) {
    if (slot_idx >= MDIM_SLOTS) return MDIM_ERR_ARG;
    for (uint32_t i = 0; i < v->jrnl_n_changes; i++)
        if (v->staged_slot[i] == slot_idx) return MDIM_OK;
    if (v->jrnl_n_changes >= MDIM_MAX_CHANGES) return MDIM_ERR_FULL;
    memcpy(v->staged_before[v->jrnl_n_changes], mdim_slot_bytes(v, slot_idx),
           MDIM_SLOT_SZ);
    v->staged_slot[v->jrnl_n_changes] = slot_idx;
    v->jrnl_n_changes++;
    return MDIM_OK;
}

/*
 * mdim_frame_write — write-ahead stage 2: materialize the pending frame in
 * the journal ring at the tail, BEFORE any base byte changes. Compacts the
 * ring (checkpoints all retained frames) when the tail can't fit the frame.
 */
static inline int mdim_frame_write(MdimVolume *v) {
    uint32_t n = v->jrnl_n_changes;
    if (n == 0) return MDIM_OK;
    uint32_t span = mdim_frame_span(n);

    if (v->jrnl_cursor + span > MDIM_JRNL_END) {
        /* compact: checkpoint everything, restart the ring. The whole ring
         * is wiped — a stale frame (e.g. an uncommitted frame left behind
         * by a previous crash-recovery) must NEVER survive into the dead
         * zone: recover() trusts that anything beyond the live tail is
         * clean, and a stale frame there would be mistaken for the crash
         * frame and its ancient before-images rolled into the base. */
        uint32_t last = mdim_last_frame(v);
        if (last != MDIM_FRAME_NONE) v->checkpoint_frame = last;
        v->journal_head = MDIM_FRAME_NONE;
        v->jrnl_cursor = MDIM_JRNL_START;
        for (uint32_t i = MDIM_JRNL_START; i < MDIM_JRNL_END; i++)
            memset(mdim_slot_bytes(v, i), 0, MDIM_SLOT_SZ);
        mdim_wu32(v->bytes, MDIM_SUPER_JRNL_HEAD, v->journal_head);
        mdim_wu32(v->bytes, MDIM_SUPER_CKPT, v->checkpoint_frame);
    }

    uint32_t hdr_slot = v->jrnl_cursor;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t ca = hdr_slot + 1 + 2 * k;
        uint8_t *a = mdim_slot_bytes(v, ca);
        uint8_t *b = mdim_slot_bytes(v, ca + 1);
        memset(a, 0, MDIM_SLOT_SZ);
        memset(b, 0, MDIM_SLOT_SZ);
        mdim_wu32(a, 0, v->staged_slot[k]);
        memcpy(a + 4, v->staged_before[k], 60);
        memcpy(b, v->staged_before[k] + 60, 4);
        mdim_wu32(b, 4, mdim_crc32(v->staged_before[k], MDIM_SLOT_SZ));
    }
    uint8_t *hdr = mdim_frame_hdr(v, hdr_slot);
    memset(hdr, 0, MDIM_SLOT_SZ);
    hdr[MDIM_FH_MAGIC] = MDIM_JMAGIC;
    hdr[MDIM_FH_FLAGS] = 0u;                                /* uncommitted */
    mdim_wu16(hdr, MDIM_FH_N_CHANGES, (uint16_t)n);
    mdim_wu32(hdr, MDIM_FH_PREV, v->journal_head);

    v->jrnl_cursor = hdr_slot + span;
    return MDIM_OK;
}

/*
 * mdim_frame_commit — stage 4: stamp the frame at the tail COMMITTED with
 * frame_no + CRC. Call AFTER the base mutation. frame_no = NONE → auto.
 * Returns the committed frame_no (or MDIM_FRAME_NONE when nothing staged).
 */
/* data slots needed for `size` bytes (each DATA slot carries 63 B) */
static inline uint32_t mdim_data_slots(uint32_t size) {
    return (size + MDIM_DATA_SLOT_BYTES - 1u) / MDIM_DATA_SLOT_BYTES;
}

/* ═══════════════ DERIVED STATE — nothing stored twice ═══════════════ */

static inline void mdim_rebuild(MdimVolume *v) {
    /* The bitmap is DERIVED from FILE entry chains — every block reachable
     * from an entry is used; everything else in the data region is free.
     * This is what makes a crash mid multi-frame op self-healing: orphaned
     * run blocks (committed frames, no entry yet) are swept on the next
     * open. TOMB slots stay (probe-chain markers) but count as free. */
    memset(v->bitmap, 0, sizeof(v->bitmap));
    v->n_blocks_used = 0;
    v->n_files = 0;

    /* pass 1: mark entry + reachable chain for every FILE entry */
    for (uint32_t i = MDIM_DATA_START; i < MDIM_SLOTS; i++) {
        MdimSlot *s = mdim_slot(v, i);
        if (s->type != MDIM_T_FILE || !s->name[0]) continue;
        if (mdim_bit_get(v, i)) continue;                  /* already claimed */
        mdim_bit_set(v, i, 1);
        v->n_blocks_used++;
        v->n_files++;
        uint32_t link = s->prev, guard = 0;
        while (link != 0 && guard < MDIM_MAX_RUNS) {
            MdimSlot *ls = mdim_slot(v, link);
            if (ls->type != MDIM_T_LINK) break;
            uint32_t d = mdim_data_slots(ls->size);
            if (link + 1 + d > MDIM_SLOTS) break;
            mdim_bit_set(v, link, 1);
            v->n_blocks_used++;
            for (uint32_t k = 0; k < d; k++) {
                mdim_bit_set(v, link + 1 + k, 1);
                v->n_blocks_used++;
            }
            link = ls->prev;
            guard++;
        }
    }

    /* pass 2: sweep — free blocks that belong to no entry's chain. Orphans
     * become TOMB (never EMPTY): an EMPTY slot terminates a stride probe
     * walk, so zeroing a freed block could hide entries placed past it. */
    for (uint32_t i = MDIM_DATA_START; i < MDIM_SLOTS; i++) {
        if (mdim_bit_get(v, i)) continue;
        MdimSlot *s = mdim_slot(v, i);
        if (s->type == MDIM_T_TOMB) continue;              /* keep probe markers */
        if (s->type != MDIM_T_EMPTY) {
            memset(s, 0, sizeof(*s));
            s->type = MDIM_T_TOMB;                         /* orphan → TOMB */
        }
    }

    v->max_probe = 0;
    mdim_wu32(v->bytes, MDIM_SUPER_N_FILES, v->n_files);
    mdim_wu32(v->bytes, MDIM_SUPER_N_USED, v->n_blocks_used);
    mdim_wu32(v->bytes, MDIM_SUPER_MAXPROBE, v->max_probe);
}

/* ═══════════════ RECOVERY ═══════════════ */

/*
 * mdim_recover — undo the uncommitted (crash) frame at the tail, verify
 * committed frames. Returns frames rolled back, or MDIM_ERR_CORRUPT
 * (fail-loud) when a committed frame fails its CRC.
 */
static inline int mdim_recover(MdimVolume *v, uint32_t *out_rolled) {
    uint32_t rolled = 0;

    /* 1) walk the committed chain newest → oldest; verify CRCs */
    uint32_t f = v->journal_head;
    uint32_t prev_fn = MDIM_FRAME_NONE;
    while (f != MDIM_FRAME_NONE) {
        uint8_t *hdr = mdim_frame_hdr(v, f);
        if (hdr[MDIM_FH_MAGIC] != MDIM_JMAGIC) break;
        uint32_t n = mdim_frame_n_changes(hdr);
        uint32_t span = mdim_frame_span(n);
        uint32_t fn = mdim_u32(hdr, MDIM_FH_FRAME_NO);
        uint32_t flags = hdr[MDIM_FH_FLAGS];
        uint32_t prev = mdim_u32(hdr, MDIM_FH_PREV);
        if (span > MDIM_JRNL_SLOTS || f + span > MDIM_JRNL_END) break;
        if (prev_fn != MDIM_FRAME_NONE && fn >= prev_fn) break; /* evicted region */
        prev_fn = fn;

        uint8_t saved[4];
        memcpy(saved, hdr + MDIM_FH_CRC, 4);
        memset(hdr + MDIM_FH_CRC, 0, 4);
        uint32_t crc = mdim_crc32(mdim_slot_bytes(v, f), span * MDIM_SLOT_SZ);
        memcpy(hdr + MDIM_FH_CRC, saved, 4);

        if (!(flags & MDIM_JFLAG_COMMITTED)) break;         /* chain anomaly */
        if (crc != mdim_u32(hdr, MDIM_FH_CRC)) return MDIM_ERR_CORRUPT; /* fail-loud */

        if (prev == MDIM_FRAME_NONE || fn <= v->checkpoint_frame) break;
        f = prev;
    }

    /* 2) uncommitted frame at the tail (crash between write and stamp) */
    uint32_t cursor = (v->journal_head == MDIM_FRAME_NONE)
                    ? MDIM_JRNL_START
                    : v->journal_head + mdim_frame_span(mdim_frame_n_changes(
                          mdim_frame_hdr(v, v->journal_head)));
    if (cursor < MDIM_JRNL_END) {
        uint8_t *hdr = mdim_frame_hdr(v, cursor);
        if (hdr[MDIM_FH_MAGIC] == MDIM_JMAGIC &&
            !(hdr[MDIM_FH_FLAGS] & MDIM_JFLAG_COMMITTED)) {
            uint32_t n = mdim_frame_n_changes(hdr);
            if (cursor + mdim_frame_span(n) > MDIM_JRNL_END)
                return MDIM_ERR_CORRUPT;  /* torn header — frame cannot be read */
            /* two-pass rollback: validate EVERY before-image first. A torn
             * change means the crash frame is untrustworthy — fail loud
             * rather than roll back a partial set (which would tear the
             * base between the old and new states). */
            for (uint32_t k = 0; k < n; k++) {
                uint32_t ca = cursor + 1 + 2 * k;
                uint8_t *a = mdim_slot_bytes(v, ca);
                uint8_t *b = mdim_slot_bytes(v, ca + 1);
                uint8_t before[MDIM_SLOT_SZ];
                memcpy(before, a + 4, 60);
                memcpy(before + 60, b, 4);
                if (mdim_u32(a, 0) >= MDIM_SLOTS ||
                    mdim_u32(b, 4) != mdim_crc32(before, MDIM_SLOT_SZ))
                    return MDIM_ERR_CORRUPT;   /* torn change — fail-loud */
            }
            for (uint32_t k = 0; k < n; k++) {
                uint32_t ca = cursor + 1 + 2 * k;
                uint8_t *a = mdim_slot_bytes(v, ca);
                uint8_t *b = mdim_slot_bytes(v, ca + 1);
                uint8_t before[MDIM_SLOT_SZ];
                memcpy(before, a + 4, 60);
                memcpy(before + 60, b, 4);
                memcpy(mdim_slot_bytes(v, mdim_u32(a, 0)), before, MDIM_SLOT_SZ);
            }
            rolled++;
        }
    }

    v->jrnl_cursor = (v->journal_head == MDIM_FRAME_NONE)
                   ? MDIM_JRNL_START
                   : v->journal_head + mdim_frame_span(mdim_frame_n_changes(
                         mdim_frame_hdr(v, v->journal_head)));
    /* wipe the dead zone — same invariant as compaction: a stale frame
     * beyond the live tail must never be mistaken for the crash frame on
     * a later load (crash → recover → operate → crash again). */
    for (uint32_t i = v->jrnl_cursor; i < MDIM_JRNL_END; i++)
        memset(mdim_slot_bytes(v, i), 0, MDIM_SLOT_SZ);
    v->jrnl_n_changes = 0;

    uint32_t last = mdim_last_frame(v);
    if (last != MDIM_FRAME_NONE && last >= v->frame_counter)
        v->frame_counter = last + 1;

    mdim_wu32(v->bytes, MDIM_SUPER_JRNL_HEAD, v->journal_head);
    mdim_wu32(v->bytes, MDIM_SUPER_FRAME, v->frame_counter);

    if (out_rolled) *out_rolled = rolled;
    return MDIM_OK;
}

/* ═══════════════ VOLUME LIFECYCLE ═══════════════ */

static inline void mdim_super_init(MdimVolume *v) {
    uint8_t *s = v->bytes;
    memset(s, 0, MDIM_SLOT_SZ);
    s[0] = 'M'; s[1] = 'D'; s[2] = 'I'; s[3] = 'M';
    mdim_wu16(s, MDIM_SUPER_VERSION, 2);
    s[MDIM_SUPER_FLAGS] = 0;
    mdim_wu32(s, MDIM_SUPER_N_FILES, 0);
    mdim_wu32(s, MDIM_SUPER_N_DIRS, 0);
    mdim_wu32(s, MDIM_SUPER_N_USED, 0);
    mdim_wu32(s, MDIM_SUPER_JRNL_HEAD, MDIM_FRAME_NONE);
    mdim_wu32(s, MDIM_SUPER_CKPT, 0);
    mdim_wu32(s, MDIM_SUPER_FRAME, 1);
    mdim_wu32(s, MDIM_SUPER_MAXPROBE, 0);
    for (uint32_t i = MDIM_JRNL_START; i < MDIM_JRNL_END; i++)
        mdim_slot(v, i)->type = MDIM_T_JRNL;
}

/* init over caller buffer (buf may be NULL → malloc) */
static inline int mdim_volume_init(MdimVolume *v, uint8_t *buf) {
    if (!v) return MDIM_ERR_ARG;
    memset(v, 0, sizeof(*v));
    if (buf) {
        v->bytes = buf;
        v->owns_bytes = 0;
    } else {
        v->bytes = (uint8_t *)calloc(1, MDIM_VOL_BYTES);
        v->owns_bytes = 1;
    }
    if (!v->bytes) return MDIM_ERR_IO;
    v->is_mapped = 0;
    mdim_super_init(v);
    v->journal_head = MDIM_FRAME_NONE;
    v->checkpoint_frame = 0;
    v->frame_counter = 1;
    v->jrnl_cursor = MDIM_JRNL_START;
    v->jrnl_n_changes = 0;
    mdim_rebuild(v);
    return MDIM_OK;
}

static inline void mdim_volume_free(MdimVolume *v) {
    if (!v) return;
#if defined(_WIN32)
    /* UnmapViewOfFile is REQUIRED — closing the mapping handle does not
     * unmap views; a leaked view keeps the file's section alive and locks
     * the file against later opens (fopen "wb" fails, remove() fails). */
    if (v->is_mapped && v->bytes) UnmapViewOfFile(v->bytes);
    if (v->h_map) CloseHandle((HANDLE)v->h_map);
    if (v->h_file) CloseHandle((HANDLE)v->h_file);
#else
    if (v->h_map) { munmap(v->bytes, MDIM_VOL_BYTES); v->bytes = NULL; }
    if (v->h_file) close((int)(intptr_t)v->h_file);
#endif
    if (v->owns_bytes && v->bytes) free(v->bytes);
    memset(v, 0, sizeof(*v));
}

/* ── save / load (whole-buffer snapshot; journal travels inside) ──── */

static inline int mdim_volume_save(MdimVolume *v, const char *path) {
    if (!v || !path) return MDIM_ERR_ARG;
    FILE *f = fopen(path, "wb");
    if (!f) return MDIM_ERR_IO;
    size_t w = fwrite(v->bytes, 1, MDIM_VOL_BYTES, f);
    fclose(f);
    return (w == MDIM_VOL_BYTES) ? MDIM_OK : MDIM_ERR_IO;
}

static inline int mdim_volume_load(MdimVolume *v, const char *path) {
    if (!v || !path) return MDIM_ERR_ARG;
    memset(v, 0, sizeof(*v));
    v->bytes = (uint8_t *)calloc(1, MDIM_VOL_BYTES);
    v->owns_bytes = 1;
    if (!v->bytes) return MDIM_ERR_IO;
    FILE *f = fopen(path, "rb");
    if (!f) { free(v->bytes); v->bytes = NULL; return MDIM_ERR_IO; }
    size_t r = fread(v->bytes, 1, MDIM_VOL_BYTES, f);
    fclose(f);
    if (r != MDIM_VOL_BYTES || memcmp(v->bytes, "MDIM", 4) != 0 ||
        mdim_u16(v->bytes, MDIM_SUPER_VERSION) != 2) {
        free(v->bytes); v->bytes = NULL;
        return MDIM_ERR_CORRUPT;
    }
    v->journal_head   = mdim_u32(v->bytes, MDIM_SUPER_JRNL_HEAD);
    v->checkpoint_frame = mdim_u32(v->bytes, MDIM_SUPER_CKPT);
    v->frame_counter  = mdim_u32(v->bytes, MDIM_SUPER_FRAME);
    int rc = mdim_recover(v, NULL);
    if (rc != MDIM_OK) { free(v->bytes); v->bytes = NULL; return rc; }
    mdim_rebuild(v);
    return MDIM_OK;
}

/* ── mmap open (native page-cache volume; flush = durability) ────── */

static inline int mdim_volume_mmap_open(const char *path, MdimVolume *v) {
    if (!path || !v) return MDIM_ERR_ARG;
    memset(v, 0, sizeof(*v));
#if defined(_WIN32)
    HANDLE hf = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return MDIM_ERR_IO;
    if (GetFileSize(hf, NULL) < MDIM_VOL_BYTES) { CloseHandle(hf); return MDIM_ERR_IO; }
    HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READWRITE, 0, 0, NULL);
    if (!hm) { CloseHandle(hf); return MDIM_ERR_IO; }
    uint8_t *base = (uint8_t *)MapViewOfFile(hm, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!base) { CloseHandle(hm); CloseHandle(hf); return MDIM_ERR_IO; }
    v->h_file = hf; v->h_map = hm;
#else
    int fd = open(path, O_RDWR);
    if (fd < 0) return MDIM_ERR_IO;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)MDIM_VOL_BYTES) {
        close(fd); return MDIM_ERR_IO;
    }
    uint8_t *base = (uint8_t *)mmap(NULL, MDIM_VOL_BYTES, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { close(fd); return MDIM_ERR_IO; }
    v->h_file = (void *)(intptr_t)fd; v->h_map = base;
#endif
    if (memcmp(base, "MDIM", 4) != 0 || mdim_u16(base, MDIM_SUPER_VERSION) != 2) {
        mdim_volume_free(v); return MDIM_ERR_CORRUPT;
    }
    v->bytes = base;
    v->is_mapped = 1;
    v->owns_bytes = 0;
    v->journal_head   = mdim_u32(base, MDIM_SUPER_JRNL_HEAD);
    v->checkpoint_frame = mdim_u32(base, MDIM_SUPER_CKPT);
    v->frame_counter  = mdim_u32(base, MDIM_SUPER_FRAME);
    int rc = mdim_recover(v, NULL);
    if (rc != MDIM_OK) { mdim_volume_free(v); return rc; }
    mdim_rebuild(v);
    return MDIM_OK;
}

static inline void mdim_volume_mmap_flush(MdimVolume *v) {
    if (!v || !v->is_mapped || !v->bytes) return;
#if defined(_WIN32)
    FlushViewOfFile(v->bytes, 0);
#else
    msync(v->bytes, MDIM_VOL_BYTES, MS_SYNC);
#endif
}

/* ═══════════════ FILE OPERATIONS ═══════════════ */

/* ── run-span geometry (a file = chain of runs) ────────────────────────
 *   File = [LINK run0][DATA...] → [LINK run1][DATA...] → ... → [LINK last][DATA...]
 *   Each run is ONE contiguous span; its LINK slot (at the run's start)
 *   holds {size = this run's bytes, prev = next run's LINK slot (0 = last)}.
 *   The FILE entry points at the first run's LINK slot, so a single run
 *   and a chain of many runs share one read path.
 *
 *   Chunking rule: each run is committed in its own journal frame
 *   (entry + LINK + ≤ 60 data slots = ≤ 62 changes per frame). */
static inline uint32_t mdim_n_runs(uint32_t size) {
    if (size == 0) return 1;
    return (size + MDIM_RUN_BYTES - 1u) / MDIM_RUN_BYTES;
}

/* bytes in run k of a file with `n` runs (runs 0..n-2 are full, last is tail) */
static inline uint32_t mdim_run_bytes(uint32_t size, uint32_t k, uint32_t n) {
    if (k + 1 < n) return MDIM_RUN_BYTES;
    return size - (n - 1u) * MDIM_RUN_BYTES;
}

/* write one run: [LINK][DATA...] at link_slot, `bytes` payload bytes */
static inline void mdim_file_store_run(MdimVolume *v, uint32_t link_slot,
                                       const uint8_t *data, uint32_t bytes,
                                       uint32_t next_link) {
    uint32_t d = mdim_data_slots(bytes);
    uint32_t rem = bytes;
    for (uint32_t k = 0; k < d; k++) {
        uint8_t *s = mdim_slot_bytes(v, link_slot + 1 + k);
        uint32_t chunk = rem < MDIM_DATA_SLOT_BYTES ? rem : MDIM_DATA_SLOT_BYTES;
        s[0] = MDIM_T_DATA;
        memcpy(s + 1, data + k * MDIM_DATA_SLOT_BYTES, chunk);
        if (chunk < MDIM_DATA_SLOT_BYTES)
            memset(s + 1 + chunk, 0, MDIM_DATA_SLOT_BYTES - chunk);
        rem -= chunk;
    }
    MdimSlot *ls = mdim_slot(v, link_slot);
    memset(ls, 0, sizeof(*ls));
    ls->type = MDIM_T_LINK;
    ls->size = bytes;
    ls->prev = next_link;
}

/* read `size` bytes from a run chain starting at first LINK slot */
static inline void mdim_file_load(MdimVolume *v, uint32_t link_slot,
                                  uint8_t *out, uint32_t size) {
    uint32_t rem = size, off = 0, guard = 0;
    while (link_slot != 0 && rem > 0 && guard < MDIM_MAX_RUNS) {
        MdimSlot *ls = mdim_slot(v, link_slot);
        if (ls->type != MDIM_T_LINK) break;
        uint32_t rb = ls->size < rem ? ls->size : rem;
        uint32_t d = mdim_data_slots(rb);
        for (uint32_t k = 0; k < d; k++) {
            const uint8_t *s = mdim_slot_bytes(v, link_slot + 1 + k);
            uint32_t chunk = rb - k * MDIM_DATA_SLOT_BYTES;
            if (chunk > MDIM_DATA_SLOT_BYTES) chunk = MDIM_DATA_SLOT_BYTES;
            memcpy(out + off + k * MDIM_DATA_SLOT_BYTES, s + 1, chunk);
        }
        off += rb;
        rem -= rb;
        link_slot = ls->prev;
        guard++;
    }
}

/*
 * mdim_summon — place a file. Entry lives at the name's probe address
 * (bond + k×37, first free); the file is stored as a chain of runs, each
 * run committed in its own journal frame (write-ahead → mutate → commit).
 * The FILE entry is written in the LAST frame, so a crash mid-chain
 * leaves no visible file — its orphaned runs are swept on the next
 * rebuild (load). Files ≤ one frame still land in a single frame.
 */
static inline uint32_t mdim_summon(MdimVolume *v, const char *name,
                                   const uint8_t *data, uint32_t size,
                                   int *err) {
    if (err) *err = MDIM_OK;
    if (!v || !name || !name[0]) { if (err) *err = MDIM_ERR_ARG; return MDIM_FRAME_NONE; }
    if (size > MDIM_MAX_FILE_BYTES) {
        if (err) *err = MDIM_ERR_SIZE;
        return MDIM_FRAME_NONE;
    }
    if (mdim_find_slot(v, name, NULL) != MDIM_FRAME_NONE) {
        if (err) *err = MDIM_ERR_EXISTS;
        return MDIM_FRAME_NONE;
    }

    uint32_t n_runs = mdim_n_runs(size);
    uint32_t n_data = mdim_data_slots(size);
    uint16_t b = mdim_bond(name);

    /* entry placement: first free probe position (EMPTY or TOMB) */
    uint32_t entry = MDIM_FRAME_NONE, entry_k = 0;
    for (uint32_t k = 0; k < MDIM_MAX_PROBE; k++) {
        uint32_t pos = mdim_probe_pos(b, k);
        MdimSlot *s = mdim_slot(v, pos);
        if (s->type == MDIM_T_EMPTY || s->type == MDIM_T_TOMB) {
            entry = pos; entry_k = k;
            break;
        }
    }
    if (entry == MDIM_FRAME_NONE) {
        if (err) *err = MDIM_ERR_PROBE;
        return MDIM_FRAME_NONE;
    }

    /* reserve the entry slot first — the run allocator must not swallow it */
    mdim_bit_set(v, entry, 1);

    /* 0) allocate ALL runs up front (extent reservation): every frame then
     *    knows its next link, and NOSPC can't leave a half-committed chain. */
    uint32_t run_links[MDIM_MAX_RUNS];
    for (uint32_t r = 0; r < n_runs; r++) {
        uint32_t d = mdim_data_slots(mdim_run_bytes(size, r, n_runs));
        uint32_t run = mdim_alloc_run(v, d + 1);          /* LINK + data, contiguous */
        if (run == MDIM_FRAME_NONE) {
            /* nothing was written yet — only release the reservations */
            for (uint32_t i = 0; i < r; i++) {
                uint32_t dd = mdim_data_slots(mdim_run_bytes(size, i, n_runs));
                for (uint32_t k = 0; k <= dd; k++) mdim_bit_set(v, run_links[i] + k, 0);
            }
            mdim_bit_set(v, entry, 0);
            if (err) *err = MDIM_ERR_NOSPC;
            return MDIM_FRAME_NONE;
        }
        run_links[r] = run;
        for (uint32_t i = 0; i <= d; i++) mdim_bit_set(v, run + i, 1);
    }

    /* 1..4) one frame per run: capture → frame-write → mutate → commit */
    for (uint32_t r = 0; r < n_runs; r++) {
        int is_last = (r + 1 == n_runs);
        uint32_t run_bytes = mdim_run_bytes(size, r, n_runs);
        uint32_t d = mdim_data_slots(run_bytes);
        uint32_t run = run_links[r];
        uint32_t next_link = is_last ? 0 : run_links[r + 1];

        mdim_pending_begin(v);
        int rc;
        if (is_last) {
            rc = mdim_pending_add(v, entry);
            if (rc != MDIM_OK) { if (err) *err = rc; return MDIM_FRAME_NONE; }
        }
        rc = mdim_pending_add(v, run);
        if (rc != MDIM_OK) { if (err) *err = rc; return MDIM_FRAME_NONE; }
        for (uint32_t i = 0; i < d; i++) {
            rc = mdim_pending_add(v, run + 1 + i);
            if (rc != MDIM_OK) { if (err) *err = rc; return MDIM_FRAME_NONE; }
        }
        rc = mdim_frame_write(v);
        if (rc != MDIM_OK) { if (err) *err = rc; return MDIM_FRAME_NONE; }
        MDIM_CRASH_HOOK(v, 1);

        const uint8_t *run_data = data ? data + r * MDIM_RUN_BYTES : NULL;
        mdim_file_store_run(v, run, run_data, run_bytes, next_link);
        v->n_blocks_used += d + 1;

        if (is_last) {
            MdimSlot *s = mdim_slot(v, entry);
            memset(s, 0, sizeof(*s));
            s->type = MDIM_T_FILE;
            s->flags = (n_runs > 1) ? MDIM_F_CHAIN : 0;
            s->size = size;
            s->crc32 = mdim_crc32(data, size);
            strncpy(s->name, name, MDIM_MAX_NAME - 1);
            s->name[MDIM_MAX_NAME - 1] = 0;
            s->home_frame = v->frame_counter;
            s->prev = run_links[0];
            s->n_runs = n_runs;
            s->n_data_slots = n_data;
            mdim_bit_set(v, entry, 1);
            v->n_blocks_used += 1;
            v->n_files++;
            if (entry_k > v->max_probe) v->max_probe = entry_k;
        }

        mdim_wu32(v->bytes, MDIM_SUPER_N_FILES, v->n_files);
        mdim_wu32(v->bytes, MDIM_SUPER_N_USED, v->n_blocks_used);
        mdim_wu32(v->bytes, MDIM_SUPER_MAXPROBE, v->max_probe);
        MDIM_CRASH_HOOK(v, 2);
        mdim_frame_commit(v, MDIM_FRAME_NONE);
        MDIM_CRASH_HOOK(v, 3);
    }
    return entry;
}

static inline int mdim_open(MdimVolume *v, const char *name, MdimFile *out) {
    uint32_t e = mdim_find_slot(v, name, NULL);
    if (e == MDIM_FRAME_NONE) return MDIM_ERR_NOENT;
    MdimSlot *s = mdim_slot(v, e);
    if (out) { out->entry = e; out->run_start = s->prev; out->size = s->size; }
    return MDIM_OK;
}

static inline int mdim_open_flat(MdimVolume *v, uint32_t flat, MdimFile *out) {
    if (flat >= MDIM_SLOTS) return MDIM_ERR_ARG;
    MdimSlot *s = mdim_slot(v, flat);
    if (s->type != MDIM_T_FILE || !s->name[0]) return MDIM_ERR_NOENT;
    if (out) { out->entry = flat; out->run_start = s->prev; out->size = s->size; }
    return MDIM_OK;
}

static inline int mdim_read(MdimVolume *v, const MdimFile *f,
                            uint8_t *buf, uint32_t buf_size, uint32_t *actual) {
    if (!v || !f || !buf) return MDIM_ERR_ARG;
    uint32_t n = buf_size < f->size ? buf_size : f->size;
    mdim_file_load(v, f->run_start, buf, n);
    if (actual) *actual = n;
    return MDIM_OK;
}

/* read size bytes starting at a raw slot (through any view's coordinates) */
static inline int mdim_view_read(MdimVolume *v, MdimView view,
                                 uint32_t a, uint32_t b, uint32_t c,
                                 uint8_t *buf, uint32_t size) {
    if (!v || !buf) return MDIM_ERR_ARG;
    uint32_t flat = mdim_view_flat(view, a, b, c);
    if (flat + size > MDIM_VOL_BYTES / MDIM_SLOT_SZ) return MDIM_ERR_SIZE;
    memcpy(buf, &v->bytes[flat * MDIM_SLOT_SZ], size);
    return MDIM_OK;
}

/*
 * mdim_write — overwrite an existing file with bytes of ANY size
 * (0 .. MDIM_MAX_FILE_BYTES, bounded by free volume space). Full chain
 * re-layout, crash-atomic WITHOUT any flag:
 *   1. allocate the NEW chain up front (reservations only — nothing committed)
 *   2. write the new runs, one frame per run — the entry is untouched, so
 *      the file keeps reading its OLD chain the whole time
 *   3. final new-run frame: switch the entry to the new chain (one atomic
 *      frame — the old chain is still intact below it)
 *   4. free the OLD chain, one frame per run — a crash here only leaks
 *      already-unreferenced blocks (swept by rebuild on the next open)
 * A crash at ANY point leaves the file fully old OR fully new — never torn.
 * Requires old+new chains to coexist briefly (grow needs free space for both).
 */
static inline int mdim_write(MdimVolume *v, const char *name,
                             const uint8_t *data, uint32_t size) {
    uint32_t e = mdim_find_slot(v, name, NULL);
    if (e == MDIM_FRAME_NONE) return MDIM_ERR_NOENT;
    if (size > MDIM_MAX_FILE_BYTES) return MDIM_ERR_SIZE;
    MdimSlot *s = mdim_slot(v, e);
    uint32_t old_first = s->prev;
    uint32_t n_runs = mdim_n_runs(size);
    uint32_t n_data = mdim_data_slots(size);

    /* 1) allocate the new chain up front (extent reservation) */
    uint32_t new_links[MDIM_MAX_RUNS];
    for (uint32_t r = 0; r < n_runs; r++) {
        uint32_t d = mdim_data_slots(mdim_run_bytes(size, r, n_runs));
        uint32_t run = mdim_alloc_run(v, d + 1);
        if (run == MDIM_FRAME_NONE) {
            for (uint32_t i = 0; i < r; i++) {
                uint32_t dd = mdim_data_slots(mdim_run_bytes(size, i, n_runs));
                for (uint32_t k = 0; k <= dd; k++) mdim_bit_set(v, new_links[i] + k, 0);
            }
            return MDIM_ERR_NOSPC;
        }
        new_links[r] = run;
        for (uint32_t i = 0; i <= d; i++) mdim_bit_set(v, run + i, 1);
    }

    /* 2) write the new runs, one frame per run; entry switched in the last */
    for (uint32_t r = 0; r < n_runs; r++) {
        int is_last = (r + 1 == n_runs);
        uint32_t run_bytes = mdim_run_bytes(size, r, n_runs);
        uint32_t d = mdim_data_slots(run_bytes);
        uint32_t run = new_links[r];
        uint32_t next_link = is_last ? 0 : new_links[r + 1];

        mdim_pending_begin(v);
        int rc;
        if (is_last) {
            rc = mdim_pending_add(v, e);
            if (rc != MDIM_OK) return rc;
        }
        rc = mdim_pending_add(v, run);
        if (rc != MDIM_OK) return rc;
        for (uint32_t i = 0; i < d; i++) {
            rc = mdim_pending_add(v, run + 1 + i);
            if (rc != MDIM_OK) return rc;
        }
        rc = mdim_frame_write(v);
        if (rc != MDIM_OK) return rc;
        MDIM_CRASH_HOOK(v, 1);

        const uint8_t *run_data = data ? data + r * MDIM_RUN_BYTES : NULL;
        mdim_file_store_run(v, run, run_data, run_bytes, next_link);
        v->n_blocks_used += d + 1;

        if (is_last) {                                        /* 3) atomic switch */
            s->prev = new_links[0];
            s->size = size;
            s->crc32 = mdim_crc32(data, size);
            s->n_runs = n_runs;
            s->n_data_slots = n_data;
            s->flags = (n_runs > 1) ? MDIM_F_CHAIN : 0;
        }
        mdim_wu32(v->bytes, MDIM_SUPER_N_USED, v->n_blocks_used);
        MDIM_CRASH_HOOK(v, 2);
        mdim_frame_commit(v, MDIM_FRAME_NONE);
        MDIM_CRASH_HOOK(v, 3);
    }

    /* 4) free the old chain, one frame per run */
    uint32_t link = old_first, guard = 0;
    while (link != 0 && guard < MDIM_MAX_RUNS) {
        MdimSlot *ls = mdim_slot(v, link);
        if (ls->type != MDIM_T_LINK) break;   /* already freed (crash remnant) */
        uint32_t d = mdim_data_slots(ls->size);
        uint32_t next = ls->prev;

        mdim_pending_begin(v);
        int rc = mdim_pending_add(v, link);
        if (rc != MDIM_OK) return rc;
        for (uint32_t i = 0; i < d; i++) {
            rc = mdim_pending_add(v, link + 1 + i);
            if (rc != MDIM_OK) return rc;
        }
        rc = mdim_frame_write(v);
        if (rc != MDIM_OK) return rc;
        MDIM_CRASH_HOOK(v, 1);

        for (uint32_t i = 0; i <= d; i++) {
            MdimSlot *t = mdim_slot(v, link + i);
            memset(t, 0, sizeof(*t));
            t->type = MDIM_T_TOMB;            /* keep stride probe chains alive */
            mdim_bit_set(v, link + i, 0);
        }
        v->n_blocks_used -= d + 1;
        mdim_wu32(v->bytes, MDIM_SUPER_N_USED, v->n_blocks_used);
        MDIM_CRASH_HOOK(v, 2);
        mdim_frame_commit(v, MDIM_FRAME_NONE);
        MDIM_CRASH_HOOK(v, 3);

        link = next;
        guard++;
    }
    return MDIM_OK;
}

static inline int mdim_unsummon(MdimVolume *v, const char *name) {
    uint32_t e = mdim_find_slot(v, name, NULL);
    if (e == MDIM_FRAME_NONE) return MDIM_ERR_NOENT;
    MdimSlot *s = mdim_slot(v, e);
    uint32_t first_link = s->prev;

    /* frame 0: tombstone the ENTRY — the file disappears immediately. A crash
     * after this point leaks chain blocks only (swept by rebuild on load). */
    mdim_pending_begin(v);
    int rc = mdim_pending_add(v, e);
    if (rc != MDIM_OK) return rc;
    rc = mdim_frame_write(v);
    if (rc != MDIM_OK) return rc;
    MDIM_CRASH_HOOK(v, 1);
    memset(s, 0, sizeof(*s));
    s->type = MDIM_T_TOMB;
    mdim_bit_set(v, e, 0);
    v->n_blocks_used -= 1;
    v->n_files--;
    mdim_wu32(v->bytes, MDIM_SUPER_N_FILES, v->n_files);
    mdim_wu32(v->bytes, MDIM_SUPER_N_USED, v->n_blocks_used);
    MDIM_CRASH_HOOK(v, 2);
    mdim_frame_commit(v, MDIM_FRAME_NONE);
    MDIM_CRASH_HOOK(v, 3);

    /* frames 1..: free each run's LINK + DATA slots (EMPTY) */
    uint32_t link = first_link, guard = 0;
    while (link != 0 && guard < MDIM_MAX_RUNS) {
        MdimSlot *ls = mdim_slot(v, link);
        if (ls->type != MDIM_T_LINK) break;
        uint32_t d = mdim_data_slots(ls->size);
        uint32_t next = ls->prev;

        mdim_pending_begin(v);
        rc = mdim_pending_add(v, link);
        if (rc != MDIM_OK) return rc;
        for (uint32_t i = 0; i < d; i++) {
            rc = mdim_pending_add(v, link + 1 + i);
            if (rc != MDIM_OK) return rc;
        }
        rc = mdim_frame_write(v);
        if (rc != MDIM_OK) return rc;
        MDIM_CRASH_HOOK(v, 1);

        for (uint32_t i = 0; i <= d; i++) {
            MdimSlot *t = mdim_slot(v, link + i);
            memset(t, 0, sizeof(*t));
            t->type = MDIM_T_TOMB;              /* keep stride probe chains alive */
            mdim_bit_set(v, link + i, 0);
        }
        v->n_blocks_used -= d + 1;
        mdim_wu32(v->bytes, MDIM_SUPER_N_USED, v->n_blocks_used);
        MDIM_CRASH_HOOK(v, 2);
        mdim_frame_commit(v, MDIM_FRAME_NONE);
        MDIM_CRASH_HOOK(v, 3);

        link = next;
        guard++;
    }
    return MDIM_OK;
}

static inline int mdim_verify(MdimVolume *v, const char *name) {
    MdimFile f;
    int rc = mdim_open(v, name, &f);
    if (rc != MDIM_OK) return rc;
    uint32_t crc = mdim_slot(v, f.entry)->crc32;
    uint8_t *tmp = (uint8_t *)malloc(f.size ? f.size : 1);
    if (!tmp) return MDIM_ERR_IO;
    mdim_file_load(v, f.run_start, tmp, f.size);
    uint32_t c = mdim_crc32(tmp, f.size);
    free(tmp);
    return (c == crc) ? MDIM_OK : MDIM_ERR_CORRUPT;
}

static inline uint32_t mdim_ls(MdimVolume *v, char names[][MDIM_MAX_NAME],
                               uint32_t max_names) {
    uint32_t n = 0;
    for (uint32_t i = MDIM_DATA_START; i < MDIM_SLOTS && n < max_names; i++) {
        MdimSlot *s = mdim_slot(v, i);
        if (s->type == MDIM_T_FILE && s->name[0]) {
            strncpy(names[n], s->name, MDIM_MAX_NAME - 1);
            names[n][MDIM_MAX_NAME - 1] = 0;
            n++;
        }
    }
    return n;
}

/* ═══════════════ TIMELINE — versioned state ═══════════════ */

/*
 * mdim_state_at — materialize the volume exactly as of commit `frame`
 * into out_bytes (MDIM_VOL_BYTES). base − undo(frames > frame).
 */
static inline int mdim_state_at(MdimVolume *v, uint32_t frame, uint8_t *out_bytes) {
    if (!v || !out_bytes) return MDIM_ERR_ARG;
    uint32_t last = mdim_last_frame(v);
    if (last == MDIM_FRAME_NONE || frame > last) return MDIM_ERR_ARG;
    if (frame <= v->checkpoint_frame) return MDIM_ERR_EVICTED;

    memcpy(out_bytes, v->bytes, MDIM_VOL_BYTES);

    /* undo committed frames with frame_no > frame (newest first). EVERY
     * frame is CRC-verified before any of its fields are trusted or any of
     * its records are undone — a torn frame (any byte: header, slot index,
     * before-image, or the per-change CRC) is fail-loud MDIM_ERR_CORRUPT.
     * Otherwise a torn frame_no/n_changes/prev/magic could silently stop
     * or misdirect the walk and serve WRONG history. */
    uint32_t f = v->journal_head;
    uint32_t prev_fn = MDIM_FRAME_NONE;
    while (f != MDIM_FRAME_NONE) {
        uint8_t *hdr = mdim_frame_hdr(v, f);
        uint32_t n = mdim_frame_n_changes(hdr);
        uint32_t span = mdim_frame_span(n);
        if (span > MDIM_JRNL_SLOTS || f + span > MDIM_JRNL_END)
            return MDIM_ERR_CORRUPT;      /* torn header — frame unreadable */
        uint8_t saved[4];
        memcpy(saved, hdr + MDIM_FH_CRC, 4);
        memset(hdr + MDIM_FH_CRC, 0, 4);
        uint32_t crc = mdim_crc32(mdim_slot_bytes(v, f), span * MDIM_SLOT_SZ);
        memcpy(hdr + MDIM_FH_CRC, saved, 4);
        if (crc != mdim_u32(hdr, MDIM_FH_CRC) ||
            hdr[MDIM_FH_MAGIC] != MDIM_JMAGIC)
            return MDIM_ERR_CORRUPT;      /* torn frame — fail-loud */
        uint32_t fn = mdim_u32(hdr, MDIM_FH_FRAME_NO);
        if (fn <= frame) break;           /* target reached — older frames stay */
        if (prev_fn != MDIM_FRAME_NONE && fn >= prev_fn) break; /* evicted region */
        prev_fn = fn;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t ca = f + 1 + 2 * k;
            uint8_t *a = mdim_slot_bytes(v, ca);
            uint8_t *b = mdim_slot_bytes(v, ca + 1);
            uint32_t idx = mdim_u32(a, 0);
            uint8_t before[MDIM_SLOT_SZ];
            memcpy(before, a + 4, 60);
            memcpy(before + 60, b, 4);
            if (idx >= MDIM_SLOTS ||
                mdim_u32(b, 4) != mdim_crc32(before, MDIM_SLOT_SZ))
                return MDIM_ERR_CORRUPT;  /* torn undo record — belt & braces */
            memcpy(&out_bytes[idx * MDIM_SLOT_SZ], before, MDIM_SLOT_SZ);
        }
        f = mdim_u32(hdr, MDIM_FH_PREV);
    }
    return MDIM_OK;
}

/* read a file's bytes as of commit `frame` */
static inline int mdim_read_at(MdimVolume *v, const char *name, uint32_t frame,
                               uint8_t *buf, uint32_t buf_size, uint32_t *actual) {
    uint8_t *snap = (uint8_t *)malloc(MDIM_VOL_BYTES);
    if (!snap) return MDIM_ERR_IO;
    int rc = mdim_state_at(v, frame, snap);
    if (rc != MDIM_OK) { free(snap); return rc; }
    MdimVolume tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.bytes = snap;
    tmp.journal_head = MDIM_FRAME_NONE;
    uint32_t a = mdim_find_slot(&tmp, name, NULL);
    if (a == MDIM_FRAME_NONE) { free(snap); return MDIM_ERR_NOENT; }
    MdimSlot *s = mdim_slot(&tmp, a);
    uint32_t n = buf_size < s->size ? buf_size : s->size;
    mdim_file_load(&tmp, s->prev, buf, n);
    if (actual) *actual = n;
    free(snap);
    return MDIM_OK;
}

/* ═══════════════ STATS / DEMO ═══════════════ */

typedef struct {
    uint32_t n_files;
    uint32_t n_blocks_used;
    uint32_t blocks_free;
    uint32_t last_frame;
    uint32_t checkpoint_frame;
    uint32_t journal_head;
    uint32_t max_probe;
    uint32_t n_frames_ring;
} MdimStats;

static inline MdimStats mdim_stats(MdimVolume *v) {
    MdimStats st;
    memset(&st, 0, sizeof(st));
    st.n_files = v->n_files;
    st.n_blocks_used = v->n_blocks_used;
    st.blocks_free = (MDIM_SLOTS - MDIM_DATA_START) - v->n_blocks_used;
    st.last_frame = mdim_last_frame(v);
    st.checkpoint_frame = v->checkpoint_frame;
    st.journal_head = v->journal_head;
    st.max_probe = v->max_probe;
    uint32_t f = v->journal_head;
    uint32_t prev_fn = MDIM_FRAME_NONE;
    while (f != MDIM_FRAME_NONE) {
        uint8_t *hdr = mdim_frame_hdr(v, f);
        if (hdr[MDIM_FH_MAGIC] != MDIM_JMAGIC) break;
        uint32_t fn = mdim_u32(hdr, MDIM_FH_FRAME_NO);
        if (prev_fn != MDIM_FRAME_NONE && fn >= prev_fn) break;
        prev_fn = fn;
        st.n_frames_ring++;
        uint32_t prev = mdim_u32(hdr, MDIM_FH_PREV);
        if (prev == MDIM_FRAME_NONE || fn <= v->checkpoint_frame) break;
        f = prev;
    }
    return st;
}

static inline void mdim_print_views(MdimVolume *v, uint32_t flat) {
    printf("  slot %u:", flat);
    for (int view = 0; view <= MDIM_VIEW_CELL; view++) {
        uint32_t a, b, c;
        mdim_view_coords((MdimView)view, flat, &a, &b, &c);
        printf("  %s(%u,%u,%u)", mdim_view_name((MdimView)view), a, b, c);
    }
    printf("\n");
}

#endif /* GEOFS_MDIM_H */
