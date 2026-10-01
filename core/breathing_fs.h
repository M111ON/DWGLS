/*
 * breathing_fs.h — Geometric File System via Breathing Seeker
 * ════════════════════════════════════════════════════════════════════
 * CORE: Compression = Space Movement
 *   Write:  seeker อยู่ position X → วาง data ที่ X
 *   Read:   seeker ต้องอยู่ X เท่านั้น → อ่าน lossless
 *   Delta:  ทุกการขยับมีผลกับ compression
 * SPACE: 20736 slots = 144 cubes × 144 slots
 * SACRED: 20736, 1728, 144, 12, 18
 * ════════════════════════════════════════════════════════════════════
 */
#ifndef BREATHING_FS_H
#define BREATHING_FS_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "bfs_v6b_adapter.h"
#include "bfs_magnify.h"
#include "bfs_fan24.h"
#include "geo_planet.h"   /* per-block watcher: birth at write, verify in tick */
#include "bfs_vmem.h"      /* reserve/commit/decommit payload region (residency layer) */

#define BFS_MAGIC          0x42524548u
#define BFS_VERSION        1u
#define BFS_TOTAL_SLOTS    20736u
#define BFS_BLOCKS         144u
#define BFS_SLOTS_BLOCK    144u
#define BFS_SEEKER_K       ((8u*9u)*(8u*9u)) /* 5184 = 72^2 = 64*81
                                              * (interaction plane squared =
                                              * binary-square x ternary-square
                                              * = quarter field). Derived,
                                              * not magic. */
#define BFS_MAX_FILES      64u
#define BFS_MAX_NAME       32u

/* Payload region geometry (residency layer): one 4 KB page per block so a
 * block is the commit/decommit granularity. 144 × 4096 = 589,824 B of
 * address space, reserved once (0 physical), committed per written block. */
#define BFS_PAYLOAD_PAGE   4096u
#define BFS_PAYLOAD_STRIDE BFS_PAYLOAD_PAGE
#define BFS_PAYLOAD_REGION (BFS_BLOCKS * BFS_PAYLOAD_STRIDE)

/* ═══════════════ BREATHING SEEKER ═══════════════ */
typedef struct {
    double   scale;
    uint32_t space_size;
    uint32_t window;
    uint32_t current_pos;
    uint32_t home_pos;
    uint8_t  is_hyperbolic;
} BreathingSeeker;

static inline void seeker_init(BreathingSeeker *s) {
    if (!s) return;
    s->scale = 1.0; s->space_size = BFS_TOTAL_SLOTS; s->window = BFS_SEEKER_K;
    s->current_pos = 0; s->home_pos = 0; s->is_hyperbolic = 0;
}

static inline void seeker_scale(BreathingSeeker *s, double ns) {
    if (!s || ns <= 0.0) return;
    /* STABILITY FIX (Aug 10, 2026): window = K/ns overflows uint32 at
     * ns < ~1.2e-6 (float-cast UB). Floor the scale at 1e-6 — hyperbolic
     * behavior (window > space) still activates for any ns < 1.0. */
    if (ns < 1e-6) ns = 1e-6;
    s->scale = ns;
    s->space_size = (uint32_t)(BFS_TOTAL_SLOTS * ns);
    if (s->space_size < 1) s->space_size = 1;
    s->window = (uint32_t)((double)BFS_SEEKER_K / ns);
    s->is_hyperbolic = (s->window > s->space_size) ? 1 : 0;
    if (s->current_pos >= s->space_size)
        s->current_pos %= s->space_size;
}

static inline void seeker_advance(BreathingSeeker *s) {
    if (!s) return;
    s->current_pos += BFS_SLOTS_BLOCK;
    if (s->current_pos >= s->space_size) s->current_pos %= s->space_size;
}

static inline int32_t seeker_delta(const BreathingSeeker *s) {
    return s ? (int32_t)s->current_pos - (int32_t)s->home_pos : 0;
}

static inline int seeker_is_home(const BreathingSeeker *s) {
    return (s && s->current_pos == s->home_pos) ? 1 : 0;
}

static inline void seeker_print(const BreathingSeeker *s) {
    if (!s) return;
    printf("  Seeker: scale=%.4f space=%u window=%u pos=%u home=%u delta=%d %s\n",
           s->scale, s->space_size, s->window, s->current_pos, s->home_pos,
           seeker_delta(s), s->is_hyperbolic ? "[HYPERBOLIC]" : "");
}

/* ═══════════════ FILE ENTRY ═══════════════ */
typedef struct {
    char     name[BFS_MAX_NAME];
    uint32_t n_blocks;
    uint32_t home_block;
    uint32_t total_bytes;
    uint8_t  strategies[4];
    uint8_t  valid;
} BFSFileEntry;

/* ═══════════════ BLOCK METADATA ═══════════════ */
typedef struct {
    uint32_t home_pos;
    uint32_t current_pos;
    int32_t  delta;
    uint8_t  strategy;
    uint8_t  scale_at_write;
    uint16_t payload_size;
} BFSBlockMeta;

/* ═══════════════ EVICTION HOOKS (card #46; v6res pattern) ═══════════════
 * Spill whole file: return 0 ok, nonzero = veto (victim kept, caller fails
 * loud, nothing lost). Fill by name: return 0 = restored (*out malloc'd by
 * backend, caller frees), 1 = no data, <0 = error. Hooks are in-memory only
 * (never persisted; parse/load resets them to NULL — caller re-attaches). */
typedef int (*bfs_spill_fn)(const char *name, const int8_t *data,
                            uint32_t size, void *user);
typedef int (*bfs_fill_fn)(const char *name, int8_t **out,
                           uint32_t *size, void *user);

/* ═══════════════ FILE SYSTEM ═══════════════ */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t n_files;
    uint32_t n_blocks_used;
    uint32_t total_bytes;
    BreathingSeeker seeker;
    BFSFileEntry files[BFS_MAX_FILES];
    uint32_t block_owner[BFS_BLOCKS];
    BFSBlockMeta block_meta[BFS_BLOCKS];
    int8_t block_data[BFS_BLOCKS][BFS_SLOTS_BLOCK];
    /* ── payload region: RESERVED virtual address space, committed per block ──
     * block_encoded[bi] points at page bi of _payload_mem; a block's page is
     * committed on write and decommitted on delete/evict, so RSS tracks the
     * resident working set instead of the full 288 KB. The indirection array
     * keeps every fs->block_encoded[bi] call site (including tests poking the
     * bytes) compiling and behaving the same. */
    uint8_t *block_encoded[BFS_BLOCKS];
    uint16_t block_encoded_size[BFS_BLOCKS];
    void    *_payload_mem;        /* reserved region base (NULL = not reserved) */
    uint32_t payload_committed;   /* blocks whose page is currently committed   */
    uint32_t delta_log[256];
    uint32_t delta_count;
    FGXLog   fg_log;        /* fan24 gear events (8-bit, replaces delta_log future) */
    Planet   planets[BFS_BLOCKS]; /* watcher per block (in-memory; restart reborn).
                                   * +~37KB. No delete path exists in v1, so no
                                   * retire hook — planets live with their blocks. */
    uint32_t planet_mismatch;     /* cumulative block mismatches seen by ticks
                                   * (rc 1 or 2). Caller resets manually. */
    uint32_t fold_count;          /* explicit folds performed (bfs_fold.h) */
    PlanetTomb tombs[BFS_BLOCKS]; /* retired-planet archive: latest tomb per
                                   * block id (overwritten on next retire of
                                   * the same block). tomb.magic==0 = none. */
    uint32_t tomb_count;          /* cumulative retires (audit) */
    /* ── residency bound + LRU eviction (card #46) ── */
    uint64_t clock;               /* LRU clock, bumped per touch */
    uint32_t file_tick[BFS_MAX_FILES]; /* last-access tick per file slot */
    uint32_t max_blocks;          /* residency cap in blocks; 0 = BFS_BLOCKS */
    bfs_spill_fn spill;           /* NULL = no spill path (writes fail -2/-3/-4) */
    bfs_fill_fn fill;             /* NULL = evicted files stay missing */
    void *spill_user;             /* opaque backend context for both hooks */
    /* ── layer separation (HJ doctrine §0: planet is layer 3, NOT counted
     * in residency) ── */
    uint8_t planets_off;          /* 0 = watch (default, v1); 1 = skip birth/
                                   * retire/verify-collect — clean measurement
                                   * of the residency layer alone */
    /* ── jet freeride counters (doctrine §5: ambulance, not highway) ──
     * Counted on the read/fault path: resident hits vs fill-faults
     * (inbound ambulance trips) + spill-outs (outbound). Writes are not
     * traffic — only reads and displacements count. */
    uint64_t direct_q;            /* resident-hit reads */
    uint64_t direct_b;            /* bytes served resident */
    uint64_t fault_q;             /* fill-fault reads (ambulance inbound) */
    uint64_t fault_b;             /* bytes restored via fill */
    uint64_t spill_q;             /* spill-out displacements */
    uint64_t spill_b;             /* bytes displaced to spill */
} BreathingFS;

/* forward decls (evict path needs delete+read; read-needs-fill needs write) */
static inline int bfs_delete(BreathingFS *fs, const char *name);
static inline int bfs_evict_oldest(BreathingFS *fs);

/* ═══════════════ PAYLOAD REGION (reserve / commit / decommit) ═══════════════
 * bfs_payload_init: reserve the full 144-page address space (0 physical) and
 * point block_encoded[bi] at page bi. Idempotent. If reservation ever fails,
 * fall back to a single committed calloc region so writes stay correct (old
 * full-resident behavior) — never a silent NULL deref.
 * bfs_payload_commit: commit page bi (safe to call repeatedly).
 * bfs_payload_release: decommit page bi — RSS drops immediately.
 * bfs_payload_free: release the whole region (file close / fs destroy). */
static inline int bfs_payload_init(BreathingFS *fs)
{
    if (!fs) return -1;
    if (!fs->_payload_mem) {
        void *mem = bfs_vmem_reserve(BFS_PAYLOAD_REGION);
        if (mem) {
            fs->_payload_mem = mem;
            fs->payload_committed = 0;
        } else {
            fs->_payload_mem = calloc(1, BFS_PAYLOAD_REGION);
            if (!fs->_payload_mem) return -1;
            fs->payload_committed = BFS_BLOCKS;   /* whole region live */
        }
    }
    for (uint32_t bi = 0; bi < BFS_BLOCKS; bi++)
        fs->block_encoded[bi] = (uint8_t *)fs->_payload_mem + (size_t)bi * BFS_PAYLOAD_STRIDE;
    return 0;
}

static inline int bfs_payload_commit(BreathingFS *fs, uint32_t bi)
{
    if (!fs || !fs->_payload_mem || bi >= BFS_BLOCKS) return -1;
    if (fs->payload_committed == BFS_BLOCKS) return 0;   /* calloc fallback: all live */
    if (bfs_vmem_commit(fs->_payload_mem, (size_t)bi * BFS_PAYLOAD_STRIDE,
                        BFS_PAYLOAD_STRIDE) != 0) return -1;
    return 0;
}

static inline void bfs_payload_release(BreathingFS *fs, uint32_t bi)
{
    if (!fs || !fs->_payload_mem || bi >= BFS_BLOCKS) return;
    if (fs->payload_committed == BFS_BLOCKS) return;     /* calloc fallback: cannot free */
    bfs_vmem_decommit(fs->_payload_mem, (size_t)bi * BFS_PAYLOAD_STRIDE,
                      BFS_PAYLOAD_STRIDE);
}

static inline void bfs_payload_free(BreathingFS *fs)
{
    if (!fs || !fs->_payload_mem) return;
    if (fs->payload_committed == BFS_BLOCKS) free(fs->_payload_mem);  /* calloc fallback */
    else bfs_vmem_unmap(fs->_payload_mem, BFS_PAYLOAD_REGION);
    fs->_payload_mem = NULL;
    for (uint32_t bi = 0; bi < BFS_BLOCKS; bi++) fs->block_encoded[bi] = NULL;
}

/* ═══════════════ INIT ═══════════════ */
static inline void bfs_init(BreathingFS *fs) {
    if (!fs) return;
    /* Contract: call bfs_destroy() first if re-initializing a live fs. Reading
     * magic from uninitialized stack here would be UB, so init never frees. */
    memset(fs, 0, sizeof(*fs));
    fs->magic = BFS_MAGIC;
    fs->version = BFS_VERSION;
    seeker_init(&fs->seeker);
    fgx_log_init(&fs->fg_log);
    for (uint32_t i = 0; i < BFS_BLOCKS; i++)
        fs->block_owner[i] = 0xFFFFFFFF;
    bfs_payload_init(fs);
}

/* Release the residency region. Call once when done with a heap/long-lived
 * fs (stack fs in tests may skip it — process exit reclaims). Safe twice. */
static inline void bfs_destroy(BreathingFS *fs) {
    if (!fs) return;
    bfs_payload_free(fs);
    fs->magic = 0;
}

/* ═══════════════ WRITE ═══════════════
 * No spill path: full table returns -2/-3/-4 exactly as v1 (tests pin this).
 * Spill path set: evict LRU files and retry until the write fits or no
 * victim is spillable (veto → fail loud, nothing lost). */
static inline int bfs_write(BreathingFS *fs, const char *name,
                             const int8_t *data, uint32_t size)
{
    if (!fs || !name || !data || size == 0) return -1;
    uint32_t n_blocks = (size + BFS_SLOTS_BLOCK - 1) / BFS_SLOTS_BLOCK;
    uint32_t cap = (fs->max_blocks && fs->max_blocks < BFS_BLOCKS)
                 ? fs->max_blocks : BFS_BLOCKS;
    if (n_blocks > cap) return -3;   /* single file larger than residency */

    /* evict-and-retry: each round frees >=1 file or stops, so snapshot the
     * bound up front (n_files shrinks as victims are evicted — rereading it
     * as the bound would exit early with a stale failure code). No spill
     * path → first failure returns its v1 code. */
    uint32_t ok_start = BFS_BLOCKS;
    int v1_rc = 0;
    uint32_t max_rounds = fs->n_files + 1;
    for (uint32_t round = 0; round < max_rounds; round++) {
        if (fs->n_files >= BFS_MAX_FILES) { v1_rc = -2; }
        else if (n_blocks > cap - fs->n_blocks_used) { v1_rc = -3; }
        else {
            /* CONTIGUOUS-RUN ALLOC (Aug 10, 2026 — consensus v3):
             * a file owns a contiguous span [start, start+n_blocks) of the block
             * address space — matches bfs_read (home_block+b) and the geometry
             * DNA "file = contiguous address span". OLD first-free-scan left
             * holes after any deletion that silently broke reads. O(n) run scan,
             * zero malloc. Returns -4 if no run of n free blocks exists. */
            uint32_t run_start = 0, found = 0;
            ok_start = BFS_BLOCKS;
            for (uint32_t i = 0; i < BFS_BLOCKS; i++) {
                if (fs->block_owner[i] == 0xFFFFFFFF) {
                    if (found == 0) run_start = i;
                    found++;
                    if (found >= n_blocks) { ok_start = run_start; break; }
                } else {
                    found = 0;
                }
            }
            if (ok_start != BFS_BLOCKS) { v1_rc = 0; break; }
            v1_rc = -4;   /* fragmented: no run of n free */
        }
        if (!fs->spill || bfs_evict_oldest(fs) != 0) return v1_rc;
        ok_start = BFS_BLOCKS;   /* re-scan after eviction */
    }
    if (ok_start == BFS_BLOCKS) return v1_rc ? v1_rc : -4;

    BFSFileEntry *fe = &fs->files[fs->n_files];
    memset(fe, 0, sizeof(*fe));
    strncpy(fe->name, name, BFS_MAX_NAME - 1);
    fe->n_blocks = n_blocks;
    fe->home_block = ok_start;
    fe->total_bytes = size;
    fe->valid = 1;

    for (uint32_t b = 0; b < n_blocks; b++) {
        uint32_t bi = ok_start + b;
        uint32_t offset = b * BFS_SLOTS_BLOCK;
        uint32_t bsz = BFS_SLOTS_BLOCK;
        if (offset + bsz > size) bsz = size - offset;

        memset(fs->block_data[bi], 0, BFS_SLOTS_BLOCK);
        memcpy(fs->block_data[bi], data + offset, bsz);

        BFSBlockMeta *bm = &fs->block_meta[bi];
        bm->home_pos = fs->seeker.current_pos;
        bm->current_pos = fs->seeker.current_pos;
        bm->delta = 0;
        bm->scale_at_write = (uint8_t)(fs->seeker.scale * 100);

        /* Use v6b streaming codec — static, no heap alloc */
        V6bContainer _bfs_dc;
        v6b_dc_init(&_bfs_dc);
        int rc = v6b_dc_encode(&_bfs_dc, fs->block_data[bi], BFS_SLOTS_BLOCK);
        if (rc == 0) {
            bm->strategy = _bfs_dc.strategy;
            bm->payload_size = (uint16_t)_bfs_dc.payload_size;
            bfs_payload_commit(fs, bi);   /* commit page only when written */
            memcpy(fs->block_encoded[bi], _bfs_dc.payload, _bfs_dc.payload_size);
            fs->block_encoded_size[bi] = (uint16_t)_bfs_dc.payload_size;
            fe->strategies[0]++;  /* v6b always uses strategy 0 (v6b) */
        }

        fs->block_owner[bi] = fs->n_files;
        /* watcher birth (layer 3; skipped when planets_off — residency
         * measurement runs without the integrity layer). Watches
         * block_ENCODED (the bytes reads consume). W=0 = full-field frame. */
        if (!fs->planets_off)
            planet_birth(&fs->planets[bi], bi, 0u, bi,
                         (const int8_t *)fs->block_encoded[bi],
                         fs->block_encoded_size[bi]);
        seeker_advance(&fs->seeker);
    }

    fs->n_files++;
    fs->n_blocks_used += n_blocks;
    fs->total_bytes += size;
    fs->file_tick[fs->n_files - 1] = (uint32_t)++fs->clock; /* write = touch */
    return 0;
}

/* ═══════════════ READ ═══════════════
 * Hit: LRU touch. Miss (-2) with fill hook: fault the file back in
 * (bfs_write may itself evict) and retry once — eviction is transparent. */
static inline int bfs_read(BreathingFS *fs, const char *name,
                            int8_t *out, uint32_t out_size, uint32_t *actual_size)
{
    if (!fs || !name || !out) return -1;

    int file_idx = -1;
    for (uint32_t i = 0; i < fs->n_files; i++) {
        if (fs->files[i].valid && strcmp(fs->files[i].name, name) == 0) {
            file_idx = (int)i;
            break;
        }
    }
    int was_fault = 0;
    if (file_idx < 0) {
        if (!fs->fill) return -2;
        int8_t *fb = NULL;
        uint32_t fsz = 0;
        int frc = fs->fill(name, &fb, &fsz, fs->spill_user);
        if (frc != 0 || !fb || fsz == 0) { free(fb); return -2; }
        int wrc = bfs_write(fs, name, fb, fsz);
        free(fb);
        if (wrc != 0) return -2;
        for (uint32_t i = 0; i < fs->n_files; i++) {
            if (fs->files[i].valid && strcmp(fs->files[i].name, name) == 0) {
                file_idx = (int)i;
                break;
            }
        }
        if (file_idx < 0) return -2;
        was_fault = 1;   /* ambulance inbound: this read rode the fill path */
    }
    fs->file_tick[file_idx] = (uint32_t)++fs->clock; /* read = touch */

    const BFSFileEntry *fe = &fs->files[file_idx];
    if (actual_size) *actual_size = fe->total_bytes;
    if (out_size < fe->total_bytes) return -3;

    for (uint32_t b = 0; b < fe->n_blocks; b++) {
        uint32_t bi = fe->home_block + b;
        if (bi >= BFS_BLOCKS) return -4;

        /* Use v6b codec — static, no heap alloc */
        V6bContainer _bfs_dc;
        v6b_dc_init(&_bfs_dc);
        _bfs_dc.strategy = fs->block_meta[bi].strategy;
        _bfs_dc.payload_size = fs->block_encoded_size[bi];
        memcpy(_bfs_dc.payload, fs->block_encoded[bi], _bfs_dc.payload_size);
        _bfs_dc.checksum = v6b_dc_crc32(_bfs_dc.payload, _bfs_dc.payload_size);

        uint32_t offset = b * BFS_SLOTS_BLOCK;
        uint32_t bsz = BFS_SLOTS_BLOCK;
        if (offset + bsz > fe->total_bytes) bsz = fe->total_bytes - offset;

        int8_t dec[BFS_SLOTS_BLOCK];
        int rc = v6b_dc_decode(&_bfs_dc, dec, BFS_SLOTS_BLOCK);
        if (rc != 0) return -5;
        memcpy(out + offset, dec, bsz);
    }
    /* freeride counters (doctrine §5): success only — failures are the
     * caller's backpressure signal, not traffic. */
    if (was_fault) { fs->fault_q++; fs->fault_b += fe->total_bytes; }
    else           { fs->direct_q++; fs->direct_b += fe->total_bytes; }
    return 0;
}

/* ═══════════════ DELETE (retire-then-free) ═══════════════
 * Lifecycle close: every block's planet is retired (tomb archived, gate
 * shut at death per T13) BEFORE the block is freed — the grave cannot be
 * reused while alive. death W=0 v1 (same YAGNI as birth W; refine when a
 * reader needs it). Returns 0 ok, -1 args, -2 not found. */
static inline int bfs_delete(BreathingFS *fs, const char *name)
{
    if (!fs || !name) return -1;
    int file_idx = -1;
    for (uint32_t i = 0; i < fs->n_files; i++) {
        if (fs->files[i].valid && strcmp(fs->files[i].name, name) == 0) {
            file_idx = (int)i;
            break;
        }
    }
    if (file_idx < 0) return -2;

    BFSFileEntry *fe = &fs->files[file_idx];
    for (uint32_t b = 0; b < fe->n_blocks; b++) {
        uint32_t bi = fe->home_block + b;
        if (bi >= BFS_BLOCKS) continue;
        /* retire-then-free (layer 3; skipped when planets_off — nothing
         * was born, so no grave to keep). */
        if (!fs->planets_off &&
            fs->planets[bi].magic == PLANET_MAGIC && !fs->planets[bi].retired) {
            planet_retire(&fs->planets[bi], 0u);
            fs->tombs[bi] = fs->planets[bi].tomb;
            fs->tomb_count++;
        }
        fs->block_owner[bi] = 0xFFFFFFFF;
        fs->block_encoded_size[bi] = 0;
        bfs_payload_release(fs, bi);   /* decommit page → RSS drops now */
        memset(&fs->block_meta[bi], 0, sizeof(fs->block_meta[bi]));
    }
    fs->n_blocks_used -= fe->n_blocks;
    fs->total_bytes -= fe->total_bytes;
    /* compact file slots (swap-with-last) so deletes really free; blocks
     * point at file indices, so repoint the moved file's blocks. Ticks move
     * with their file slot (LRU identity follows the entry). */
    uint32_t last = fs->n_files - 1u;
    if ((uint32_t)file_idx != last) {
        fs->files[file_idx] = fs->files[last];
        fs->file_tick[file_idx] = fs->file_tick[last];
        BFSFileEntry *mv = &fs->files[file_idx];
        for (uint32_t b = 0; b < mv->n_blocks; b++) {
            uint32_t bi = mv->home_block + b;
            if (bi < BFS_BLOCKS) fs->block_owner[bi] = (uint32_t)file_idx;
        }
    }
    memset(&fs->files[last], 0, sizeof(fs->files[last]));  /* valid=0 */
    fs->file_tick[last] = 0;
    fs->n_files--;
    return 0;
}

/* ═══════════════ EVICTION + RESIDENCY (card #46) ═══════════════
 * LRU victim = valid file with smallest tick (untouched files tick 0 evict
 * first — oldest by construction). Spill runs BEFORE delete (retire-then-free
 * inside bfs_delete archives planets first); spill veto → victim kept.
 * Returns 0 ok, -1 args/empty, -2 no spill path, -3 spill vetoed. */
static inline int bfs_evict_oldest(BreathingFS *fs)
{
    if (!fs) return -1;
    if (fs->n_files == 0) return -1;
    if (!fs->spill) return -2;
    uint32_t victim = 0;
    for (uint32_t i = 1; i < fs->n_files; i++)
        if (fs->file_tick[i] < fs->file_tick[victim]) victim = i;
    BFSFileEntry *fe = &fs->files[victim];
    if (!fe->valid) return -1;
    int8_t *buf = (int8_t *)malloc(fe->total_bytes);
    if (!buf) return -1;
    uint32_t act = 0;
    /* direct decode (no fill recursion: victim is resident by construction) */
    int rc = -2;
    {
        uint32_t save_tick = fs->file_tick[victim];
        /* inline read without touch/fill: victim exists, decode straight */
        V6bContainer _ev_dc;
        uint8_t ok = 1;
        for (uint32_t b = 0; b < fe->n_blocks; b++) {
            uint32_t bi = fe->home_block + b;
            if (bi >= BFS_BLOCKS) { ok = 0; break; }
            v6b_dc_init(&_ev_dc);
            _ev_dc.strategy = fs->block_meta[bi].strategy;
            _ev_dc.payload_size = fs->block_encoded_size[bi];
            memcpy(_ev_dc.payload, fs->block_encoded[bi], _ev_dc.payload_size);
            _ev_dc.checksum = v6b_dc_crc32(_ev_dc.payload, _ev_dc.payload_size);
            uint32_t offset = b * BFS_SLOTS_BLOCK;
            uint32_t bsz = BFS_SLOTS_BLOCK;
            if (offset + bsz > fe->total_bytes) bsz = fe->total_bytes - offset;
            int8_t dec[BFS_SLOTS_BLOCK];
            if (v6b_dc_decode(&_ev_dc, dec, BFS_SLOTS_BLOCK) != 0) { ok = 0; break; }
            memcpy(buf + offset, dec, bsz);
        }
        if (ok) { rc = 0; act = fe->total_bytes; }
        fs->file_tick[victim] = save_tick;
    }
    if (rc != 0 || act != fe->total_bytes) { free(buf); return -1; }
    char vname[BFS_MAX_NAME];
    memcpy(vname, fe->name, BFS_MAX_NAME);
    vname[BFS_MAX_NAME - 1] = '\0';
    if (fs->spill(vname, buf, act, fs->spill_user) != 0) { free(buf); return -3; }
    free(buf);
    fs->spill_q++;             /* outbound ambulance trip (displacement) */
    fs->spill_b += act;
    return bfs_delete(fs, vname);
}

static inline void bfs_set_spill(BreathingFS *fs, bfs_spill_fn spill,
                                 bfs_fill_fn fill, void *user)
{
    if (!fs) return;
    fs->spill = spill;
    fs->fill = fill;
    fs->spill_user = user;
}

/* Residency cap in blocks (0 = full 144). Shrinking below current use does
 * NOT evict — the bound applies to future writes (fail/evict per policy). */
static inline void bfs_set_planets(BreathingFS *fs, int on)
{
    if (!fs) return;
    fs->planets_off = on ? 0 : 1;
}

/* Residency cap in blocks (0 = full 144). Shrinking below current use does
 * NOT evict — the bound applies to future writes (fail/evict per policy). */
static inline void bfs_set_capacity(BreathingFS *fs, uint32_t max_blocks)
{
    if (!fs) return;
    fs->max_blocks = (max_blocks >= BFS_BLOCKS || max_blocks == 0)
                   ? 0 : max_blocks;
}

static inline void bfs_residency(const BreathingFS *fs, uint32_t *blocks,
                                 uint32_t *bytes, uint32_t *cap)
{
    if (!fs) return;
    if (blocks) *blocks = fs->n_blocks_used;
    if (bytes) *bytes = fs->total_bytes;
    if (cap) *cap = (fs->max_blocks ? fs->max_blocks : BFS_BLOCKS);
}

/* ═══════════════ FREERIDE REPORT (doctrine §5) ═══════════════
 * Permille of reads that rode the ambulance (fault_q*1000/total_q).
 * >500‰ on one consumer's working set = freeriding (primary data must
 * arrive direct). System-wide sustained >50‰ (5%) = upstream addressing
 * is broken — fix addressing, never widen the spur. Live ratio, no latch:
 * the caller trends it, the header only counts. */
static inline uint32_t bfs_jet_ratio(const BreathingFS *fs)
{
    if (!fs) return 0;
    uint64_t total = fs->direct_q + fs->fault_q;
    if (total == 0) return 0;
    return (uint32_t)((fs->fault_q * 1000u) / total);
}
static inline int bfs_jet_alarm(const BreathingFS *fs)
{
    return bfs_jet_ratio(fs) > 50;   /* sustained >5% faults = investigate */
}
static inline void bfs_jet_report(const BreathingFS *fs, uint64_t *direct_q,
                                  uint64_t *direct_b, uint64_t *fault_q,
                                  uint64_t *fault_b, uint64_t *spill_q,
                                  uint64_t *spill_b)
{
    if (!fs) return;
    if (direct_q) *direct_q = fs->direct_q;
    if (direct_b) *direct_b = fs->direct_b;
    if (fault_q) *fault_q = fs->fault_q;
    if (fault_b) *fault_b = fs->fault_b;
    if (spill_q) *spill_q = fs->spill_q;
    if (spill_b) *spill_b = fs->spill_b;
}

static inline void bfs_move_seeker(BreathingFS *fs, double new_scale) {
    if (!fs) return;
    uint32_t old_pos = fs->seeker.current_pos;
    seeker_scale(&fs->seeker, new_scale);
    for (uint32_t b = 0; b < BFS_BLOCKS; b++) {
        if (fs->block_owner[b] != 0xFFFFFFFF) {
            BFSBlockMeta *bm = &fs->block_meta[b];
            uint32_t shifted = (uint32_t)((double)bm->home_pos * fs->seeker.scale);
            bm->current_pos = shifted % fs->seeker.space_size;
            bm->delta = (int32_t)bm->current_pos - (int32_t)bm->home_pos;
        }
    }
    /* gear event: 8-bit ring-24 CRT bijection */
    if (fs->delta_count < 256) {
        fs->delta_log[fs->delta_count++] = fs->seeker.current_pos;
        bfs_gear_push(&fs->fg_log, old_pos, fs->seeker.current_pos);
    }
    /* magnifier glass: store as separate field (is_hyperbolic = window>space) */
    fs->seeker.is_hyperbolic |= bfs_mg_flat_in_glass(fs->seeker.current_pos) ? 2 : 0;
}

static inline void bfs_go_home(BreathingFS *fs) {
    if (!fs) return;
    /* seeker must physically return to home_pos — lossless read is defined
     * at home (delta=0). seeker_scale keeps current_pos; restore it here. */
    uint32_t old_pos = fs->seeker.current_pos;
    fs->seeker.home_pos = fs->seeker.home_pos % BFS_TOTAL_SLOTS;
    fs->seeker.current_pos = fs->seeker.home_pos;
    seeker_scale(&fs->seeker, 1.0);
    fs->seeker.is_hyperbolic = 0;  /* clear glass flag on return home */
    for (uint32_t b = 0; b < BFS_BLOCKS; b++) {
        if (fs->block_owner[b] != 0xFFFFFFFF) {
            fs->block_meta[b].current_pos = fs->block_meta[b].home_pos;
            fs->block_meta[b].delta = 0;
        }
    }
    /* gear event for home return */
    bfs_gear_push(&fs->fg_log, old_pos, fs->seeker.current_pos);
}

static inline void bfs_delta_stats(const BreathingFS *fs) {
    if (!fs) return;
    uint32_t non_zero = 0;
    int32_t max_d = 0;
    for (uint32_t b = 0; b < BFS_BLOCKS; b++) {
        if (fs->block_owner[b] != 0xFFFFFFFF) {
            int32_t d = fs->block_meta[b].delta;
            if (d != 0) non_zero++;
            if (d < 0) d = -d;
            if (d > max_d) max_d = d;
        }
    }
    printf("  Deltas: %u non-zero / %u blocks | max |delta| = %d\n",
           non_zero, fs->n_blocks_used, max_d);
    printf("  Gear: %u events (%u bytes) | RIM=%s\n",
           bfs_gear_count(&fs->fg_log), bfs_gear_bytes(&fs->fg_log),
           bfs_gear_is_rim(&fs->fg_log) ? "yes" : "no");
}

/* ═══════════════ VERIFY ═══════════════ */
static inline int bfs_verify_file(BreathingFS *fs, const char *name,
                                   const int8_t *original, uint32_t size)
{
    if (!fs || !name || !original) return -1;
    int8_t *recon = (int8_t *)malloc(size);
    if (!recon) return -1;
    uint32_t actual = 0;
    int rc = bfs_read(fs, name, recon, size, &actual);
    if (rc != 0) { free(recon); return rc; }
    if (actual != size) { free(recon); return -6; }
    int match = (memcmp(original, recon, size) == 0);
    free(recon);
    return match ? 0 : -7;
}

/* ═══════════════ MIGRATE (wrap-relocate, not delete-destroy) ═══════════════
 * Model surgery primitive: read file LOSSLESS from src, write to dst (new
 * planets born watching), verify dst bytes == src bytes, then retire+free
 * src (tombs archived, blocks reusable). The old world keeps the graves;
 * the new world gets the living data with fresh watchers. Time stays
 * frozen throughout (no ticks inside) — the wrap is static by #849.
 * Returns 0 ok, -1 args, -2 src missing, -3 src==dst, -4 dst name taken,
 * -5 dst write failed, -6 dst verify mismatch (dst may hold a partial —
 * caller deletes it; fail-closed, never silent). Placed after VERIFY +
 * DELETE (uses both). */
static inline int bfs_migrate(BreathingFS *src, BreathingFS *dst, const char *name)
{
    if (!src || !dst || !name) return -1;
    if (src == dst) return -3;
    uint32_t size = 0;
    for (uint32_t i = 0; i < src->n_files; i++) {
        if (src->files[i].valid && strcmp(src->files[i].name, name) == 0) {
            size = src->files[i].total_bytes;
            break;
        }
    }
    if (size == 0) return -2;
    for (uint32_t i = 0; i < dst->n_files; i++) {
        if (dst->files[i].valid && strcmp(dst->files[i].name, name) == 0)
            return -4;
    }
    int8_t *buf = (int8_t *)malloc(size);
    if (!buf) return -1;
    uint32_t act = 0;
    int rc = bfs_read(src, name, buf, size, &act);
    if (rc != 0 || act != size) { free(buf); return -2; }
    rc = bfs_write(dst, name, buf, size);
    if (rc != 0) { free(buf); return -5; }
    int ok = (bfs_verify_file(dst, name, buf, size) == 0);
    free(buf);
    if (!ok) return -6;
    return bfs_delete(src, name);   /* retire-then-free: graves stay in src */
}

/* ═══════════════ PRINT ═══════════════ */
static inline void bfs_print_dir(const BreathingFS *fs) {
    if (!fs) return;
    printf("  Files: %u | Blocks: %u / %u | Bytes: %u\n",
           fs->n_files, fs->n_blocks_used, BFS_BLOCKS, fs->total_bytes);
    for (uint32_t i = 0; i < fs->n_files; i++) {
        const BFSFileEntry *fe = &fs->files[i];
        if (!fe->valid) continue;
        printf("    %-20s %3u blocks  %6u bytes  S=%u C=%u D=%u R=%u\n",
               fe->name, fe->n_blocks, fe->total_bytes,
               fe->strategies[0], fe->strategies[1], fe->strategies[2], fe->strategies[3]);
    }
}

#endif /* BREATHING_FS_H */
