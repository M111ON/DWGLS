/*
 * fibo_spine.h — Fibo Spine + P5H Ribcage + Jet Bridge
 *
 * Architecture:
 *   1728 pipes x 12 ticks = 20736 slots (= GEO_FULL)
 *   Each pipe cycles through 12 ticks.
 *   Tick 11 → Jet Bridge triggers → enter residual_space → re-enter at tick 13
 *
 * Ribcage (p5h) wraps the spine:
 *   P5H = P5(GEOM) + H(hop) integration point
 *   At tick 12 boundary, P5H freezes data → assigns residual address
 *
 * Jet Bridge:
 *   exit at tick 11 → residual_space → re-entry at tick 13
 *   Skips tick 12 entirely (tick 12 = barrier/freeze boundary)
 *   CPU uses bridge window for async work while GPU processes
 *
 * All header-only, static inline, no malloc in hot path.
 */

#pragma once

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "geo_config.h"
#include "geo_spoke_sync.h"   /* SpokeSync for pipeline sync awareness */

/* ── Sacred constants ─────────────────────────────────── */
#define FS_PIPES            1728u    /* 12 × 144 = 1728            */
#define FS_TICKS_PER_CYCLE  12u      /* 0..11                      */
#define FS_SLOTS            (FS_PIPES * FS_TICKS_PER_CYCLE) /* 20736 = GEO_FULL */
#define FS_JET_BRIDGE_TICK  11u      /* exit tick                  */
#define FS_REENTRY_TICK     13u      /* re-entry tick (mod 12 = 1) */
#define FS_TICK_12          12u      /* barrier boundary (skipped) */

/* ── Ribcage constants ────────────────────────────────── */
#define RC_N_CYCLES         144u     /* fibo completeness cycles   */
#define RC_N_PIPES          FS_PIPES /* 1728                      */
#define RC_N_TICKS          12u      /* per cycle                  */
#define RC_TOTAL_OPS        (RC_N_CYCLES * RC_N_PIPES * RC_N_TICKS) /* 3M — too large for stack */
#define RC_CAPACITY_DEFAULT 65536u   /* heap-allocated entry cap   */

/* ── Jet Bridge states ────────────────────────────────── */
#define JB_INACTIVE     0u      /* normal tick, no bridge           */
#define JB_ARMED         1u      /* tick=10, bridge will fire next  */
#define JB_BRIDGING      2u      /* tick=11 → entering residual     */
#define JB_RESIDENT      3u      /* inside residual_space           */
#define JB_RETURNING     4u      /* tick=13 → re-entering from res  */

/* ── Spine tick mode ──────────────────────────────────── */
#define FS_MODE_ACTIVE   0u      /* normal tick progression         */
#define FS_MODE_BRIDGE   1u      /* jet bridge active               */
#define FS_MODE_RESIDENT 2u      /* in residual_space               */
#define FS_MODE_PERPIPE  4u      /* per-pipe independent ticks      */

/* ── Pipe flags ───────────────────────────────────────── */
#define PIPE_FLAG_NONE      0x00
#define PIPE_FLAG_BRIDGED   0x01
#define PIPE_FLAG_FROZEN    0x02
#define PIPE_FLAG_RESIDENT  0x04

/* ════════════════════════════════════════════════════════════
   DATA STRUCTURES
   ════════════════════════════════════════════════════════════ */

/* One pipe in the spine — 12 ticks of state */
typedef struct {
    uint8_t  ticks[FS_TICKS_PER_CYCLE]; /* state per tick (0..11)  */
    uint8_t  flags;                      /* PIPE_FLAG_*              */
    uint8_t  current_tick;              /* 0..11 (synced or local)  */
    uint8_t  local_tick;                /* 0..11 per-pipe tick      */
    uint16_t pipe_id;                   /* 0..1727                  */
    uint32_t residual_addr;             /* address in residual_space */
} FiboPipe;

/* Ribcage entry — P5H integration point */
typedef struct {
    uint32_t entry_id;      /* unique entry ID          */
    uint8_t  tick;          /* which tick (0..11)       */
    uint8_t  phase;         /* P5H phase (0..5)         */
    uint16_t pipe_id;       /* which pipe               */
    uint64_t bond_key;      /* associated bond key       */
    uint32_t residual_off;  /* offset in residual_space  */
    uint8_t  frozen;        /* 1 = frozen at tick 12    */
} RibcageEntry;

/* Fibo Spine context */
typedef struct {
    FiboPipe   pipes[FS_PIPES];       /* 1728 pipes              */
    uint16_t   active_pipe;            /* current pipe index      */
    uint8_t    global_tick;            /* current tick (0..11)    */
    uint8_t    bridge_state;           /* JB_* state              */
    uint64_t   tick_count;             /* total ticks processed   */
    uint8_t    mode;                   /* FS_MODE_*               */
    uint32_t   resident_pipe_count;    /* pipes in residual       */
    SpokeSync  spoke_sync;             /* 6-lane sync for spoke tracking */
} FiboSpine;

/* P5H Ribcage context (wraps spine with freeze capability) */
typedef struct {
    FiboSpine   *spine;                /* parent spine             */
    RibcageEntry *entries;             /* heap-allocated buffer     */
    uint32_t      entry_count;
    uint32_t      capacity;            /* allocated entry capacity   */
    uint32_t      freeze_count;        /* total freeze events       */
    uint8_t       global_phase;        /* 0..5                      */
} P5HRibcage;

/* ════════════════════════════════════════════════════════════
   INIT
   ════════════════════════════════════════════════════════════ */

static inline void fibo_spine_init(FiboSpine *fs) {
    memset(fs, 0, sizeof(*fs));
    for (uint16_t p = 0; p < FS_PIPES; p++) {
        fs->pipes[p].pipe_id      = p;
        fs->pipes[p].current_tick = 0;
        fs->pipes[p].local_tick   = 0;
        fs->pipes[p].flags        = PIPE_FLAG_NONE;
    }
    fs->bridge_state = JB_INACTIVE;
    fs->mode         = FS_MODE_ACTIVE;
    spoke_sync_init(&fs->spoke_sync);
}

static inline void p5h_ribcage_init(P5HRibcage *rc, FiboSpine *spine) {
    memset(rc, 0, sizeof(*rc));
    rc->spine    = spine;
    rc->capacity = RC_CAPACITY_DEFAULT;
    rc->entries  = (RibcageEntry *)calloc(rc->capacity, sizeof(RibcageEntry));
}

static inline void p5h_ribcage_free(P5HRibcage *rc) {
    if (rc) {
        free(rc->entries);
        rc->entries = NULL;
        rc->entry_count = 0;
        rc->capacity = 0;
    }
}

/* ════════════════════════════════════════════════════════════
   TICK ADVANCE
   ════════════════════════════════════════════════════════════ */

/*
 * fibo_spine_tick() — advance spine by one tick
 *
 * Returns JB_* state after tick.
 * Triggers Jet Bridge at tick 11:
 *   tick 10 → JB_ARMED (bridge will fire next tick)
 *   tick 11 → JB_BRIDGING (entering residual)
 *   tick 12 → JB_RESIDENT (inside residual, this tick is skipped)
 *   tick 13 → JB_RETURNING (exiting residual)
 *
 * Jet Bridge = exit at 11 → skip 12 → re-enter at 13
 */
static inline uint8_t fibo_spine_tick(FiboSpine *fs) {
    if (!fs) return JB_INACTIVE;

    fs->tick_count++;

    /* Advance global tick counter (orchestrator reference) */
    fs->global_tick = (uint8_t)((fs->global_tick + 1) % FS_TICKS_PER_CYCLE);

    /* ── Per-pipe mode: each pipe ticks independently ────────── */
    if (fs->mode & FS_MODE_PERPIPE) {
        /* Global tick advances as reference only.
         * Each pipe's local_tick is managed via fibo_spine_pipe_tick().
         * No force-sync here. Bridge detection is per-pipe. */
        fs->bridge_state = JB_INACTIVE;
        return JB_INACTIVE;
    }

    /* ── Global mode: all pipes sync to global_tick ──────────── */
    /* Detect Jet Bridge trigger — fires when entering tick 11 */
    if (fs->global_tick == FS_JET_BRIDGE_TICK) {
        fs->bridge_state = JB_BRIDGING;
        fs->mode = FS_MODE_BRIDGE;

        /* For each active pipe at tick 11, flag as bridged */
        for (uint16_t p = 0; p < FS_PIPES; p++) {
            if (fs->pipes[p].flags & PIPE_FLAG_NONE) {
                /* This pipe is about to enter residual */
                fs->pipes[p].flags |= PIPE_FLAG_BRIDGED;
                fs->pipes[p].current_tick = 0; /* reset on re-entry */
                fs->pipes[p].local_tick   = 0;
                fs->resident_pipe_count++;
            }
        }

        /* Skip tick 12: directly advance to what would be tick 13
         * After this, global_tick should be 1 (tick 13 mod 12 = 1) */
        fs->global_tick = 1;  /* tick 13 mod 12 */
        fs->bridge_state = JB_RETURNING;
        fs->mode = FS_MODE_ACTIVE;

        return JB_BRIDGING;
    }

    /* Normal tick update for active pipes */
    for (uint16_t p = 0; p < FS_PIPES; p++) {
        fs->pipes[p].current_tick = fs->global_tick;
        if (!(fs->mode & FS_MODE_PERPIPE))
            fs->pipes[p].local_tick = fs->global_tick;
    }

    fs->bridge_state = JB_INACTIVE;
    return JB_INACTIVE;
}

/*
 * fibo_spine_tick_n() — advance N ticks, calls tick() each time
 * Returns number of Jet Bridge events triggered
 */
static inline uint32_t fibo_spine_tick_n(FiboSpine *fs, uint32_t n) {
    uint32_t bridges = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t state = fibo_spine_tick(fs);
        if (state == JB_BRIDGING) bridges++;
    }
    return bridges;
}

/* ════════════════════════════════════════════════════════════
   SPOKE SYNC — pipeline-level sync awareness
   ════════════════════════════════════════════════════════════ */

static inline void fibo_spine_sync_enc(FiboSpine *fs, uint16_t enc) {
    spoke_sync_update(&fs->spoke_sync, enc);
}

static inline uint8_t fibo_spine_sync_active(const FiboSpine *fs) {
    return fs->spoke_sync.active;
}

static inline uint8_t fibo_spine_sync_rewind(FiboSpine *fs, uint8_t spoke,
                                              uint16_t target_enc) {
    spoke_sync_rewind(&fs->spoke_sync, spoke, target_enc);
    return fs->spoke_sync.active;
}

static inline int fibo_spine_sync_brake(FiboSpine *fs, int max_polls) {
    for (int i = 0; i < max_polls; i++) {
        if (fs->spoke_sync.active == 0) return i;
    }
    return -1;
}

/* ════════════════════════════════════════════════════════════
   PER-PIPE TICK ADVANCE — each pipe advances independently
   ════════════════════════════════════════════════════════════ */

/*
 * fibo_spine_pipe_tick() — advance a single pipe's local_tick
 *
 * Each pipe has its own tick counter independent of global_tick.
 * Returns the pipe's new local_tick (0..11).
 * When local_tick wraps to 0, it signals a full cycle.
 * In per-pipe mode, this is the primary tick advance mechanism.
 *
 * Use when different pipes need independent timing (e.g. 12 origins
 * each with their own timeline). The global_tick remains as an
 * orchestrator reference but is NOT tied to pipe progression.
 */
static inline uint8_t fibo_spine_pipe_tick(FiboSpine *fs, uint16_t pipe_id) {
    if (!fs || pipe_id >= FS_PIPES) return 0xFFu;

    FiboPipe *pipe = &fs->pipes[pipe_id];

    /* Advance this pipe's local tick */
    pipe->local_tick = (pipe->local_tick + 1) % FS_TICKS_PER_CYCLE;

    /* Sync current_tick to local_tick for per-pipe mode */
    if (fs->mode & FS_MODE_PERPIPE) {
        pipe->current_tick = pipe->local_tick;
    }

    /* Update pipe tick state array */
    pipe->ticks[pipe->local_tick]++;

    /* Check jet bridge: pipe at tick 11 bridges independently */
    if (pipe->local_tick == FS_JET_BRIDGE_TICK) {
        pipe->flags |= PIPE_FLAG_BRIDGED;
        fs->bridge_state = JB_BRIDGING;
        fs->resident_pipe_count++;
    }

    /* If pipe loops back to 0, it completed a cycle */
    if (pipe->local_tick == 0) {
        pipe->flags &= ~PIPE_FLAG_BRIDGED;
    }

    return pipe->local_tick;
}

/*
 * fibo_spine_pipe_is_bridge() — check if a pipe is at bridge boundary
 *
 * A pipe bridges when its local_tick == FS_JET_BRIDGE_TICK (11).
 * Returns 1 if bridge should fire, 0 otherwise.
 */
static inline uint8_t fibo_spine_pipe_is_bridge(const FiboSpine *fs, uint16_t pipe_id) {
    if (!fs || pipe_id >= FS_PIPES) return 0;
    return (fs->pipes[pipe_id].local_tick == FS_JET_BRIDGE_TICK) ? 1 : 0;
}

/*
 * fibo_spine_pipe_sync() — sync a pipe's local_tick to global_tick
 *
 * Resets the pipe's local tick to match the global orchestrator.
 * Used when switching between global and per-pipe modes.
 */
static inline void fibo_spine_pipe_sync(FiboSpine *fs, uint16_t pipe_id) {
    if (!fs || pipe_id >= FS_PIPES) return;
    FiboPipe *pipe = &fs->pipes[pipe_id];
    pipe->local_tick   = fs->global_tick;
    pipe->current_tick = fs->global_tick;
}

/*
 * fibo_spine_pipe_sync_all() — sync all pipes' local_ticks to global
 */
static inline void fibo_spine_pipe_sync_all(FiboSpine *fs) {
    if (!fs) return;
    for (uint16_t p = 0; p < FS_PIPES; p++)
        fibo_spine_pipe_sync(fs, p);
}

/* ════════════════════════════════════════════════════════════
   PIPE ACCESS
   ════════════════════════════════════════════════════════════ */

/*
 * fibo_spine_get_pipe() — get pipe by index
 */
static inline FiboPipe *fibo_spine_get_pipe(FiboSpine *fs, uint16_t pipe_id) {
    if (!fs || pipe_id >= FS_PIPES) return NULL;
    return &fs->pipes[pipe_id];
}

/*
 * fibo_spine_active_pipe_count() — pipes not bridged
 */
static inline uint32_t fibo_spine_active_pipe_count(const FiboSpine *fs) {
    if (!fs) return 0;
    uint32_t count = 0;
    for (uint16_t p = 0; p < FS_PIPES; p++) {
        if (!(fs->pipes[p].flags & PIPE_FLAG_BRIDGED))
            count++;
    }
    return count;
}

/*
 * fibo_spine_is_bridge_tick() — check if current tick triggers bridge
 */
static inline uint8_t fibo_spine_is_bridge_tick(uint8_t tick) {
    return tick == FS_JET_BRIDGE_TICK;
}

/* ════════════════════════════════════════════════════════════
   JET BRIDGE
   ════════════════════════════════════════════════════════════ */

/*
 * jet_bridge_hop() — execute Jet Bridge for a single pipe
 *
 * Takes a pipe at tick 11 and redirects its data into residual_space.
 * Returns the residual address where data was placed.
 * The pipe's current_tick is set to 13 (mod 12 = 1) on re-entry.
 *
 * residual_fn: callback to store data in residual_space.
 *              Signature: uint32_t (*residual_fn)(uint16_t pipe_id, const void *data, uint32_t size)
 *              Returns residual address.
 */
static inline uint32_t jet_bridge_hop(
    FiboSpine      *fs,
    uint16_t        pipe_id,
    const void     *data,
    uint32_t        size,
    uint32_t      (*residual_fn)(uint16_t pipe_id, const void *data, uint32_t size))
{
    if (!fs || pipe_id >= FS_PIPES) return 0xFFFFFFFFu;
    FiboPipe *pipe = &fs->pipes[pipe_id];

    /* Store data in residual_space via callback */
    uint32_t r_addr = 0xFFFFFFFFu;
    if (residual_fn) {
        r_addr = residual_fn(pipe_id, data, size);
    }

    /* Mark pipe as bridged and record residual address */
    pipe->flags         |= PIPE_FLAG_BRIDGED | PIPE_FLAG_RESIDENT;
    pipe->residual_addr  = r_addr;
    pipe->current_tick   = 1;  /* tick 13 mod 12 */

    fs->bridge_state = JB_BRIDGING;
    fs->mode         = FS_MODE_BRIDGE;
    fs->resident_pipe_count++;

    return r_addr;
}

/*
 * jet_bridge_hop_all() — bridge all active pipes at tick boundary
 * Returns count of pipes bridged
 */
static inline uint32_t jet_bridge_hop_all(
    FiboSpine      *fs,
    const void    **data_ptrs,
    const uint32_t *sizes,
    uint32_t      (*residual_fn)(uint16_t pipe_id, const void *data, uint32_t size))
{
    if (!fs) return 0;
    uint32_t count = 0;
    for (uint16_t p = 0; p < FS_PIPES; p++) {
        FiboPipe *pipe = &fs->pipes[p];
        if (!(pipe->flags & PIPE_FLAG_BRIDGED)) {
            const void *d = data_ptrs ? data_ptrs[p] : NULL;
            uint32_t sz = sizes ? sizes[p] : 0;
            jet_bridge_hop(fs, p, d, sz, residual_fn);
            count++;
        }
    }
    return count;
}

/*
 * jet_bridge_return() — return from residual_space
 * Sets pipe back to active mode at tick 13
 */
static inline void jet_bridge_return(FiboSpine *fs, uint16_t pipe_id) {
    if (!fs || pipe_id >= FS_PIPES) return;
    FiboPipe *pipe = &fs->pipes[pipe_id];

    pipe->flags         &= ~(PIPE_FLAG_BRIDGED | PIPE_FLAG_RESIDENT);
    pipe->current_tick   = 1;  /* tick 13 mod 12 */
    if (fs->resident_pipe_count > 0)
        fs->resident_pipe_count--;

    if (fs->resident_pipe_count == 0) {
        fs->bridge_state = JB_RETURNING;
        fs->mode         = FS_MODE_ACTIVE;
    }
}

/* ════════════════════════════════════════════════════════════
   P5H RIBCAGE OPERATIONS
   ════════════════════════════════════════════════════════════ */

/*
 * p5h_ribcage_step() — record one cycle step in ribcage
 *
 * Called each time data passes through a pipe at a given tick.
 * Records entry for later freeze/retrieval.
 */
static inline uint32_t p5h_ribcage_step(P5HRibcage *rc,
                                         uint16_t pipe_id,
                                         uint8_t  tick,
                                         uint64_t bond_key)
{
    if (!rc || !rc->entries || rc->entry_count >= rc->capacity) return 0xFFFFFFFFu;

    RibcageEntry *e = &rc->entries[rc->entry_count];
    e->entry_id     = rc->entry_count;
    e->tick         = tick;
    e->phase        = rc->global_phase;
    e->pipe_id      = pipe_id;
    e->bond_key     = bond_key;
    e->residual_off = 0xFFFFFFFFu;
    e->frozen       = 0;

    rc->entry_count++;

    /* Advance phase when we complete a full pipe cycle */
    if (tick == 0 && pipe_id == FS_PIPES - 1) {
        rc->global_phase = (rc->global_phase + 1) % 6;
    }

    return rc->entry_count - 1;
}

/*
 * p5h_freeze_at_tick12() — freeze all data at tick 12 boundary
 *
 * Called when Jet Bridge triggers: freezes bridged pipe data
 * by assigning residual_off + setting frozen flag.
 * This is the P5H barrier.
 */
static inline uint32_t p5h_freeze_at_tick12(P5HRibcage *rc)
{
    if (!rc) return 0;

    uint32_t frozen = 0;
    FiboSpine *fs = rc->spine;

    for (uint16_t p = 0; p < FS_PIPES; p++) {
        FiboPipe *pipe = &fs->pipes[p];
        if (!(pipe->flags & PIPE_FLAG_BRIDGED)) continue;

        for (uint32_t e = 0; e < rc->entry_count; e++) {
            RibcageEntry *re = &rc->entries[e];
            if (re->pipe_id == p && re->tick == FS_JET_BRIDGE_TICK && !re->frozen) {
                re->residual_off = (uint32_t)(re->bond_key);
                re->frozen       = 1;
                pipe->flags     |= PIPE_FLAG_FROZEN;
                frozen++;
                rc->freeze_count++;
                break;
            }
        }
    }

    return frozen;
}

/* ════════════════════════════════════════════════════════════
   QUERY / STATUS
   ════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t total_pipes;
    uint32_t active_pipes;
    uint32_t bridged_pipes;
    uint32_t resident_pipes;
    uint32_t frozen_pipes;
    uint64_t total_ticks;
    uint8_t  current_tick;
    uint8_t  bridge_state;
    uint8_t  mode;                  /* FS_MODE_* */
    uint32_t ribcage_entries;
    uint32_t freeze_count;
    uint32_t pipes_at_tick[FS_TICKS_PER_CYCLE]; /* per-tick pipe counts */
    uint32_t min_local_tick;
    uint32_t max_local_tick;
} FiboSpineStats;

static inline FiboSpineStats fibo_spine_stats(const FiboSpine *fs) {
    FiboSpineStats s;
    memset(&s, 0, sizeof(s));
    if (!fs) return s;

    s.total_pipes    = FS_PIPES;
    s.total_ticks    = fs->tick_count;
    s.current_tick   = fs->global_tick;
    s.bridge_state   = fs->bridge_state;
    s.mode           = fs->mode;
    s.min_local_tick = FS_TICKS_PER_CYCLE;
    s.max_local_tick = 0;

    for (uint16_t p = 0; p < FS_PIPES; p++) {
        const FiboPipe *pipe = &fs->pipes[p];
        if (pipe->flags & PIPE_FLAG_BRIDGED) {
            s.bridged_pipes++;
            if (pipe->flags & PIPE_FLAG_RESIDENT)
                s.resident_pipes++;
        } else {
            s.active_pipes++;
        }
        if (pipe->flags & PIPE_FLAG_FROZEN)
            s.frozen_pipes++;

        /* Per-pipe tick distribution */
        uint8_t lt = pipe->local_tick;
        if (lt < FS_TICKS_PER_CYCLE)
            s.pipes_at_tick[lt]++;
        if (lt < s.min_local_tick) s.min_local_tick = lt;
        if (lt > s.max_local_tick) s.max_local_tick = lt;
    }

    return s;
}

static inline const char *fibo_spine_bridge_name(uint8_t state) {
    switch (state) {
        case JB_INACTIVE:   return "INACTIVE";
        case JB_ARMED:      return "ARMED";
        case JB_BRIDGING:   return "BRIDGING";
        case JB_RESIDENT:   return "RESIDENT";
        case JB_RETURNING:  return "RETURNING";
        default:            return "UNKNOWN";
    }
}

static inline const char *fibo_spine_mode_name(uint8_t mode) {
    switch (mode) {
        case FS_MODE_ACTIVE:   return "ACTIVE";
        case FS_MODE_BRIDGE:   return "BRIDGE";
        case FS_MODE_RESIDENT: return "RESIDENT";
        default:               return "UNKNOWN";
    }
}
