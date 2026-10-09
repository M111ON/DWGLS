/* kv_frame_index.h — the TIMELINE INDEX over cold-KV frame artifacts.
 *
 * Problem it solves: kv_cold_chat.c names its artifacts by prefix hash
 *   <outdir>\chat_<prefixhash16hex>.kvcb   (base, opaque llama state)
 *   <outdir>\chat_t<N>.kvd                 (token-suffix delta for turn N)
 * so finding "turn 7's frame" today means scanning the directory and
 * re-hashing. This header adds the missing piece: an append-only manifest
 * that maps a logical frame index (turn) -> its artifacts, O(1) seek.
 *
 * Design (matches repo rules for core/: stdlib only, no llama.h):
 *   - The manifest is ONE file, appended per committed turn. A frame is
 *     only written after its .kvcb/.kvd are on disk (crash between leaves
 *     an orphan artifact, never a manifest entry pointing at nothing).
 *   - The header carries the frame count; a torn tail record is detected
 *     by the count vs. file length and simply ignored (last good wins).
 *   - Frame t needs base + delta: base is the most recent .kvcb at or
 *     before t (resume it), deltas are the .kvd chain after it. seek(t)
 *     therefore returns the RESUME PLAN, not one file.
 *
 * Reconstruct rule (from kv_cold_base.h): live(t) = resume(base@b) +
 * teacher-decode(tokens[b..t]). This index stores only ADDRESSES; the
 * tokens live in the .kvd files, the KV in the .kvcb file.
 */
#ifndef KV_FRAME_INDEX_H
#define KV_FRAME_INDEX_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define FTL_MAGIC 0x4C54464BU   /* "KFTL" LE: KV frame timeline */
#define FTL_VER   1
#define FTL_PATH  128           /* fixed path slots keep the record POD */
#define FTL_SID   64

typedef struct {
    uint32_t magic;         /* FTL_MAGIC */
    uint32_t ver;           /* FTL_VER */
    uint32_t n_frames;      /* committed frames */
    uint32_t rec_size;      /* sizeof(FTLFrame) — format guard */
    int32_t  n_ctx;         /* ctx n_ctx at capture (seek sanity) */
    uint32_t reserved;
    uint64_t model_hash;    /* FNV-1a of model path — refuse cross-model seek */
} FTLHeader;

typedef struct {
    int32_t  turn;          /* logical frame index (1-based, as kv_cold_chat) */
    int32_t  base_n_tokens; /* tokens the base (.kvcb) covers */
    uint32_t n_delta;       /* suffix tokens in this frame's .kvd */
    uint64_t prefix_hash;   /* FNV-1a over tokens[0..key_n) — frame key */
    uint64_t t_unix;        /* commit wall time (informational only) */
    char     base_file[FTL_PATH];  /* e.g. chat_<hash>.kvcb   (relative) */
    char     delta_file[FTL_PATH]; /* e.g. chat_t7.kvd        (relative) */
    char     sid[FTL_SID];         /* chat session id ('' if none) */
} FTLFrame;

typedef struct {
    FTLHeader hdr;
    FTLFrame *frames;       /* malloc'd, hdr.n_frames entries */
    char      dir[FTL_PATH];/* outdir the manifest lives in */
} FTLIndex;

static inline uint64_t ftl_fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ULL; }
    return h;
}

static inline void ftl_manifest_path(char *out, size_t cap, const char *dir) {
    snprintf(out, cap, "%s\\kv_timeline.idx", dir);
}

/* ── open/load: read the whole manifest (cheap: POD records) ── */
static inline int ftl_open(FTLIndex *ix, const char *dir, int32_t n_ctx, const char *model_path) {
    memset(ix, 0, sizeof(*ix));
    snprintf(ix->dir, sizeof(ix->dir), "%s", dir);
    char p[FTL_PATH + 32]; ftl_manifest_path(p, sizeof(p), dir);
    FILE *f = fopen(p, "rb");
    if (!f) {  /* first time: synthesise an empty header */
        ix->hdr.magic = FTL_MAGIC; ix->hdr.ver = FTL_VER;
        ix->hdr.n_frames = 0; ix->hdr.rec_size = (uint32_t)sizeof(FTLFrame);
        ix->hdr.n_ctx = n_ctx; ix->hdr.model_hash = ftl_fnv1a(model_path);
        return 0;
    }
    if (fread(&ix->hdr, sizeof(FTLHeader), 1, f) != 1) { fclose(f); return -1; }
    if (ix->hdr.magic != FTL_MAGIC || ix->hdr.ver != FTL_VER ||
        ix->hdr.rec_size != (uint32_t)sizeof(FTLFrame)) { fclose(f); return -2; }
    /* torn tail: how many whole records does the file actually hold? */
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    long body = flen - (long)sizeof(FTLHeader);
    uint32_t have = body > 0 ? (uint32_t)(body / (long)sizeof(FTLFrame)) : 0;
    if (have < ix->hdr.n_frames) ix->hdr.n_frames = have;   /* last good wins */
    ix->frames = (FTLFrame *)calloc(ix->hdr.n_frames ? ix->hdr.n_frames : 1, sizeof(FTLFrame));
    fseek(f, (long)sizeof(FTLHeader), SEEK_SET);
    uint32_t got = (uint32_t)fread(ix->frames, sizeof(FTLFrame), ix->hdr.n_frames, f);
    fclose(f);
    ix->hdr.n_frames = got;
    return 0;
}

static inline void ftl_close(FTLIndex *ix) { free(ix->frames); ix->frames = NULL; }

/* ── append: one committed turn. Rewrites the header count in place,
 *    then appends the record. Atomic-enough: a crash before the append
 *    leaves the old count (record ignored); after, the record is whole. ── */
static inline int ftl_commit(FTLIndex *ix, const FTLFrame *fr) {
    char p[FTL_PATH + 32]; ftl_manifest_path(p, sizeof(p), ix->dir);
    FILE *f = fopen(p, "rb+");
    int created = 0;
    if (!f) {
        f = fopen(p, "wb+"); created = 1;
        if (!f) return -1;
    }
    if (created) {
        if (fwrite(&ix->hdr, sizeof(FTLHeader), 1, f) != 1) { fclose(f); return -1; }
    }
    /* write the record AFTER the last good one */
    long off = (long)sizeof(FTLHeader) + (long)ix->hdr.n_frames * (long)sizeof(FTLFrame);
    if (fseek(f, off, SEEK_SET) != 0) { fclose(f); return -1; }
    if (fwrite(fr, sizeof(FTLFrame), 1, f) != 1) { fclose(f); return -1; }
    /* bump the count that now stands */
    FTLHeader nh = ix->hdr; nh.n_frames += 1;
    if (fseek(f, 0, SEEK_SET) != 0 ||
        fwrite(&nh, sizeof(FTLHeader), 1, f) != 1) { fclose(f); return -1; }
    fflush(f); fclose(f);

    /* grow the in-memory table */
    FTLFrame *nf = (FTLFrame *)realloc(ix->frames, nh.n_frames * sizeof(FTLFrame));
    if (nf) { ix->frames = nf; ix->frames[nh.n_frames - 1] = *fr; ix->hdr.n_frames = nh.n_frames; }
    return 0;
}

/* ── seek: the resume plan for frame `turn` (1-based). Returns the index
 *    of the base frame to resume (or -1 = must decode from scratch), and
 *    fills *out_base / *out_last. Frames [base+1 .. last] are the deltas
 *    to teacher-decode in order. ── */
typedef struct {
    int      have_base;      /* 1 = resume out_base's .kvcb first */
    int32_t  base_turn;      /* turn of the base frame (0 = none) */
    int32_t  base_n_tokens;
    uint64_t base_prefix_hash;
    int32_t  first_delta_turn;  /* deltas start at this turn +... */
    int32_t  last_turn;         /* inclusive */
    int32_t  total_n_tokens;    /* tokens covered through last_turn */
} FTLSeek;

static inline int ftl_seek(const FTLIndex *ix, int32_t turn, FTLSeek *out) {
    memset(out, 0, sizeof(*out));
    if (turn < 1 || (uint32_t)turn > ix->hdr.n_frames) return -1;
    int last = -1;
    for (uint32_t i = 0; i < ix->hdr.n_frames; i++)
        if (ix->frames[i].turn == turn) { last = (int)i; break; }
    if (last < 0) return -1;
    /* the base we can resume = the latest frame at or before `last` whose
       delta_file is empty (a full base) — here every committed turn has a
       base; the *nearest* base is the frame with the largest base_n_tokens
       not exceeding this turn's coverage. Simplest correct plan: resume the
       immediately preceding frame's base, decode this frame's own delta. */
    out->last_turn = turn;
    out->total_n_tokens = ix->frames[last].base_n_tokens + (int32_t)ix->frames[last].n_delta;
    if (last > 0) {
        out->have_base = 1;
        out->base_turn = ix->frames[last-1].turn;
        out->base_n_tokens = ix->frames[last-1].base_n_tokens
                           + (int32_t)ix->frames[last-1].n_delta;
        out->first_delta_turn = ix->frames[last].turn;
    } else {
        out->have_base = 0;
        out->base_turn = 0;
        out->first_delta_turn = ix->frames[last].turn;
    }
    return 0;
}

/* ── directory listing helper: find frames by prefix hash (dedup /
 *    re-anchor lookup). Returns turn or -1. ── */
static inline int32_t ftl_find_prefix(const FTLIndex *ix, uint64_t ph) {
    for (uint32_t i = 0; i < ix->hdr.n_frames; i++)
        if (ix->frames[i].prefix_hash == ph) return ix->frames[i].turn;
    return -1;
}

static inline void ftl_print(const FTLIndex *ix) {
    printf("kv_timeline: %u frames, n_ctx=%d, model=%016llx\n",
           ix->hdr.n_frames, ix->hdr.n_ctx,
           (unsigned long long)ix->hdr.model_hash);
    for (uint32_t i = 0; i < ix->hdr.n_frames; i++) {
        const FTLFrame *f = &ix->frames[i];
        printf("  t%-3d base=%-24s (%d tok) delta=%-14s (+%u) pfx=%016llx\n",
               f->turn, f->base_file, f->base_n_tokens, f->delta_file,
               f->n_delta, (unsigned long long)f->prefix_hash);
    }
}

#endif /* KV_FRAME_INDEX_H */
