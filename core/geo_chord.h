/*
 * geo_chord.h — Chord-Based Multi-Pointer Access
 *
 * CONCEPT:
 *   chord = root + quality → 3-4 addresses (notes) พร้อมกัน
 *   capo  = transpose (semitone shift) = shift root + intervals
 *   strum = read all notes from chord → parallel access
 *
 *   guitar analogy:
 *     chord shape (quality) = interval pattern
 *     capo (transpose)      = shift base_addr
 *     #/b (accidental)     = ±1 semitone ทุก interval
 *
 * ADDRESS SPACE:
 *   root = address ใน [0, GEO_FULL)
 *   intervals = offset ใน GEO_FULL space
 *   capo = shift root + wrap ใน [0, GEO_FULL)
 *
 * TWO-LEVEL CAPO:
 *   Shell level:  ShellContainer.key_offset  (global shift per shell)
 *   Chord level:  GeoChord.transpose         (per-chord shift)
 *   Result:       addr = (root + interval + transpose + key_offset) % GEO_FULL
 *
 * No float. No malloc. No global state.
 */

#pragma once

#include <stdint.h>
#include "geo_jump.h"
#include "shell_container.h"

/* ── Chord quality ──────────────────────────────────────── */
#define CHORD_MAJOR     0u   /* intervals: 0, 4, 7                  */
#define CHORD_MINOR     1u   /* intervals: 0, 3, 7                  */
#define CHORD_DOM7      2u   /* intervals: 0, 4, 7, 10              */
#define CHORD_MAJ7      3u   /* intervals: 0, 4, 7, 11              */
#define CHORD_MIN7      4u   /* intervals: 0, 3, 7, 10              */
#define CHORD_DIM       5u   /* intervals: 0, 3, 6                  */
#define CHORD_AUG       6u   /* intervals: 0, 4, 8                  */

#define CHORD_MAX_NOTES 4u   /* max notes per chord                 */

/* ── Accidental flags ───────────────────────────────────── */
#define CHORD_FLAG_SHARP    (1u << 0)   /* # → +1 offset          */
#define CHORD_FLAG_FLAT     (1u << 1)   /* b → -1 offset          */
#define CHORD_FLAG_EXT7     (1u << 2)   /* add 7th (interval 10)  */
#define CHORD_FLAG_EXT9     (1u << 3)   /* add 9th (interval 14)  */

/* ── Chord descriptor ───────────────────────────────────── */
typedef struct {
    uint32_t root_addr;     /* base address in [0, GEO_FULL)        */
    uint8_t  quality;       /* CHORD_MAJOR / MINOR / DOM7 / etc.    */
    uint8_t  flags;         /* CHORD_FLAG_* accidentals + extensions*/
    uint8_t  transpose;     /* capo: semitone offset (0 = no capo)  */
    uint8_t  _pad;
} GeoChord;

/* ── Resolved chord: N addresses ────────────────────────── */
typedef struct {
    uint32_t addrs[CHORD_MAX_NOTES];  /* resolved GEO_FULL addresses */
    uint8_t  count;                   /* actual note count (3-4)     */
} GeoChordResolved;

/* ── Interval tables ────────────────────────────────────── */
/* Intervals in GEO_FULL space units */
static const uint8_t _chord_intervals[7][4] = {
    {0, 4, 7,  0},   /* MAJOR — 3 notes */
    {0, 3, 7,  0},   /* MINOR — 3 notes */
    {0, 4, 7, 10},   /* DOM7  — 4 notes */
    {0, 4, 7, 11},   /* MAJ7  — 4 notes */
    {0, 3, 7, 10},   /* MIN7  — 4 notes */
    {0, 3, 6,  0},   /* DIM   — 3 notes */
    {0, 4, 8,  0},   /* AUG   — 3 notes */
};
static const uint8_t _chord_note_count[7] = {3, 3, 4, 4, 4, 3, 3};

/* ── Semitone → GEO_FULL offset ─────────────────────────── */
/*
 * 1 semitone = GEO_FULL / 12 = 1728 units
 * 12 semitones = 1 octave = GEO_FULL (full address space wrap)
 *
 * This maps naturally: 12 faces × 1728 pipes/tick per face
 */
static inline int32_t _chord_semitone_offset(uint8_t semitones) {
    return (int32_t)semitones * (int32_t)(GEO_FULL / 12);
}

/* ── Press chord → resolve to N addresses ───────────────── */
/*
 * geo_chord_press() — resolve GeoChord to N GEO_FULL addresses
 *
 * shell_offset: key_offset from ShellContainer (shell-level capo)
 *               pass 0 if no shell capo
 *
 * Formula: addr = (root + interval + transpose + shell_offset) % GEO_FULL
 */
static inline GeoChordResolved geo_chord_press(const GeoChord *ch,
                                                uint32_t shell_offset)
{
    GeoChordResolved r;
    uint8_t q = (ch->quality < 7) ? ch->quality : 0;
    r.count   = _chord_note_count[q];

    /* Accidental offset */
    int32_t acc = 0;
    if (ch->flags & CHORD_FLAG_SHARP) acc = +1;
    if (ch->flags & CHORD_FLAG_FLAT)  acc = -1;

    /* Capo: transpose + shell offset combined */
    int32_t capo = _chord_semitone_offset(ch->transpose)
                 + (int32_t)shell_offset;

    for (uint8_t i = 0; i < r.count; i++) {
        int32_t interval = (int32_t)_chord_intervals[q][i];
        int32_t addr = (int32_t)ch->root_addr + interval + acc + capo;
        /* wrap within GEO_FULL */
        addr = ((addr % (int32_t)GEO_FULL) + (int32_t)GEO_FULL)
               % (int32_t)GEO_FULL;
        r.addrs[i] = (uint32_t)addr;
    }
    return r;
}

/* ── Transpose chord (capo at chord level) ──────────────── */
/*
 * geo_chord_transpose() — shift chord root by semitones
 * Returns new chord (original unchanged)
 */
static inline GeoChord geo_chord_transpose(GeoChord ch, int8_t semitones) {
    ch.transpose = (uint8_t)((int16_t)ch.transpose + semitones);
    return ch;
}

/* ── Build chord from address + quality ─────────────────── */
static inline GeoChord geo_chord_make(uint32_t root_addr,
                                       uint8_t quality,
                                       uint8_t flags)
{
    GeoChord ch;
    ch.root_addr  = root_addr % GEO_FULL;
    ch.quality    = (quality < 7) ? quality : CHORD_MAJOR;
    ch.flags      = flags & 0x0Fu;
    ch.transpose  = 0;
    ch._pad       = 0;
    return ch;
}

/* ── Chord from ShellContainer chord + key_offset ───────── */
/*
 * Builds a chord from a shell Chord descriptor + shell key_offset.
 * The shell's Chord contains seed and chord_id; this function
 * uses the chord_id to determine quality and the seed as root.
 */
static inline GeoChord geo_chord_from_shell(const Chord *c,
                                              uint32_t shell_key_offset)
{
    /* Map chord_id to quality */
    uint8_t q;
    switch (c->chord_id) {
        case CHORD_ORBITAL: q = CHORD_MAJOR; break;
        case CHORD_CHIRAL:  q = CHORD_MINOR; break;
        case CHORD_CROSS:   q = CHORD_DOM7;  break;
        case CHORD_HUB:     q = CHORD_MAJ7;  break;
        default:            q = CHORD_MAJOR; break;
    }

    GeoChord ch;
    ch.root_addr  = c->seed % GEO_FULL;
    ch.quality    = q;
    ch.flags      = 0;
    ch.transpose  = 0;
    ch._pad       = 0;

    /* Apply transpose = key_offset converted to semitones */
    /* key_offset / (GEO_FULL/12) = key_offset / 1728 ≈ semitones */
    uint8_t semitones = (uint8_t)(shell_key_offset / (GEO_FULL / 12));
    if (semitones > 0) {
        GeoChord t = geo_chord_transpose(ch, (int8_t)semitones);
        ch = t;
    }

    return ch;
}

/* ── Strum: resolve chord + apply to function ───────────── */
/*
 * geo_chord_strum() — resolve chord addresses and call fn for each
 *
 * fn: callback called with each resolved address
 *     Return 0 to continue, non-zero to stop early
 * ctx: context passed to fn
 *
 * Returns number of addresses iterated.
 */
typedef int (*ChordStrumFn)(uint32_t addr, uint8_t note_idx, void *ctx);

static inline uint8_t geo_chord_strum(const GeoChord *ch,
                                       uint32_t shell_offset,
                                       ChordStrumFn fn,
                                       void *ctx)
{
    if (!ch || !fn) return 0;

    GeoChordResolved r = geo_chord_press(ch, shell_offset);
    for (uint8_t i = 0; i < r.count; i++) {
        if (fn(r.addrs[i], i, ctx) != 0)
            return i + 1;
    }
    return r.count;
}
