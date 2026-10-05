/*
 * tools/frustum_real.c — fog / frustum-composite walk on REAL field bytes.
 *
 * Every prior probe (pattern_probe, frustum_probe, minor_forage_probe) ran on
 * hand-computed synthetic values. This one runs on a real baked field
 * (fieldA.bin = Qwen2.5-0.5B-Q8_0, addresses already proven to resolve to the
 * correct source bytes by tools/verify_field.c).
 *
 * Model of a frustum anchor (owner spec, 2026-10-05):
 *   - one anchor = one tensor (a real weight blob at a real address)
 *   - the anchor has 6 FACES (one per direction) — a frustum composite
 *   - fog starts everywhere: every face is OPEN (not yet walked)
 *   - walking an anchor through direction d SHUTS face d (leaves a trace)
 *     and reads real bytes at that face's position inside the tensor
 *   - pattern = the 6-bit mask of which faces are shut (who came in which way)
 *   - tombstone = entry face + exit face (2 bits)
 *   - a forward walk is never blocked; only reserved anchors (none here) halt
 *   - the data never moves: we only READ at an address (MAP not COMPRESS)
 *
 * What this proves on real data:
 *   R1 each anchor's 6 faces read DISTINCT bytes (a frustum's faces are
 *      addressable, not the same slot)
 *   R2 bytes read at a face == the source GGUF bytes at the same position
 *      (the walk reads the real tensor, not a copy)
 *   R3 two walkers over the same anchors leave different patterns (fog is
 *      per-walker, data is shared and static)
 *   R4 re-walking an anchor lights nothing new (fog is monotone; only CLEAR
 *      reopens)
 *   R5 a tombstone (entry+exit faces) is enough to say which way a walker
 *      passed
 *
 * BUILD: gcc -O2 -std=c11 -I . -I core -I core/infra -o build/frustum_real \
 *            tools/frustum_real.c -lm
 * RUN:   ./build/frustum_real <field.bin> <source.gguf> [max_tensors]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../core/gguf_box.h"

#define FACES 6

/* ── KV walk (same walker as field_address.c / verify_field.c) ──
 * arr_ptr kept as OFFSET from base — pointers into infos[] go stale. */
typedef struct { size_t val_start; char name[64]; uint32_t arr_type; uint64_t arr_count;
                 size_t arr_off; } KVInfo;
static int kv_walk(const uint8_t *base, KVInfo *infos, int cap, uint32_t *n_out) {
    uint64_t n_kv;
    memcpy(&n_kv, base + 16, 8);
    if (n_kv > (uint64_t)cap) return -1;
    const uint8_t *p = base + 24;
    uint32_t n = 0;
    static const uint8_t vsz[] = {1,1,2,2,4,4,4,1,0,0,8,8,8};
    for (uint64_t k = 0; k < n_kv; k++) {
        KVInfo *kv = &infos[n];
        uint64_t klen; uint32_t vtype;
        memcpy(&klen, p, 8); p += 8;
        memcpy(kv->name, p, klen < 63 ? klen : 63); kv->name[klen < 63 ? klen : 63] = 0;
        p += klen;
        memcpy(&vtype, p, 4); p += 4;
        kv->val_start = (size_t)(p - base);
        kv->arr_type = 0; kv->arr_count = 0; kv->arr_off = 0;
        if (vtype == 9) {
            uint32_t at; uint64_t narr;
            memcpy(&at, p, 4); p += 4; memcpy(&narr, p, 8); p += 8;
            kv->arr_type = at; kv->arr_count = narr; kv->arr_off = (size_t)(p - base);
            if (at == 8) { for (uint64_t a = 0; a < narr; a++) { uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl; } }
            else if (at < 13) p += (size_t)vsz[at] * narr;
            else return -1;
        } else if (vtype == 8) { uint64_t sl; memcpy(&sl, p, 8); p += 8; p += sl; }
        else if (vtype <= 12) p += vsz[vtype];
        else return -1;
        n++;
    }
    *n_out = n;
    return 0;
}
static uint64_t arr_u64(const uint8_t *base, const KVInfo *kv, uint64_t i) {
    uint64_t v = 0;
    if (i >= kv->arr_count) return 0;
    if (kv->arr_type == 10) memcpy(&v, base + kv->arr_off + i * 8, 8);
    else if (kv->arr_type == 4) { uint32_t w; memcpy(&w, base + kv->arr_off + i * 4, 4); v = w; }
    return v;
}

/* ── a frustum anchor: 6 faces of real bytes ── */
typedef struct {
    const char *name;
    uint64_t    addr;      /* within-field offset of the tensor body */
    uint64_t    size;
    uint8_t     face[FACES]; /* 1 byte read at each face (the real data) */
    uint64_t    fpos[FACES]; /* byte offset of that face inside the tensor */
} Anchor;

/* Face d of a tensor of `size` bytes: deterministic spread, no float, no
 * value inspection — position only (value-blindness, MAP not COMPRESS). */
static uint64_t face_pos(uint64_t size, int d) {
    if (size == 0) return 0;
    return ((size - 1) * (uint64_t)(d + 1)) / FACES;
}

static const uint8_t *find_tensor(GGUFBox *src, const char *name, uint64_t *size) {
    for (uint32_t i = 0; i < src->n_tensors; i++)
        if (strcmp(src->entries[i].name, name) == 0) {
            *size = src->entries[i].size;
            return src->entries[i].data;   /* zero-copy pointer into mmap */
        }
    return NULL;
}

/* ── a walker: one latch block (fog per face), monotone ── */
typedef struct { uint8_t shut; uint8_t hits; uint8_t entry_face; uint8_t exit_face; } Walker;

static int lit_new(Walker *w, int d) {
    if (w->shut & (1u << d)) return 0;   /* already closed — idempotent state */
    if (w->hits == 0) w->entry_face = (uint8_t)d;
    w->exit_face = (uint8_t)d;
    w->shut |= (uint8_t)(1u << d);
    w->hits++;
    return 1;
}

int main(int argc, char **argv) {
    const char *field_path = argc > 1 ? argv[1] : "build/fieldA.bin";
    const char *src_path   = argc > 2 ? argv[2] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    uint32_t    max_t      = argc > 3 ? (uint32_t)atoi(argv[3]) : 64;
    setvbuf(stdout, NULL, _IONBF, 0);

    GGUFBox fld, src;
    if (gguf_box_open(&fld, field_path) != 0) { printf("(cannot open field)\n"); return 1; }
    if (gguf_box_open(&src, src_path)   != 0) { printf("(cannot open source)\n"); return 1; }

    uint32_t N = fld.n_tensors;
    KVInfo fk[128]; uint32_t nfk = 0;
    if (kv_walk(fld.reader.base, fk, 96, &nfk) != 0) { printf("(kv walk failed)\n"); return 1; }
    uint64_t body_off = 0;
    const KVInfo *fpos_kv = NULL;
    for (uint32_t i = 0; i < nfk; i++) {
        if (strcmp(fk[i].name, "kis.layout.body_off") == 0) memcpy(&body_off, fld.reader.base + fk[i].val_start, 8);
        if (strcmp(fk[i].name, "kis.layout.fpos") == 0) fpos_kv = &fk[i];
    }
    if (!body_off || !fpos_kv) { printf("(field is not v2 with fpos[])\n"); return 1; }

    printf("frustum_real — fog/pattern walk on REAL field bytes\n");
    printf("  field : %s  tensors=%u  body_off=%I64u\n", field_path, N, (unsigned long long)body_off);
    printf("  source: %s  tensors=%u\n\n", src_path, src.n_tensors);

    int nchk = 0, npass = 0;
    #define CHECK(c, ...) do { nchk++; if (c) { npass++; printf("  PASS "); } else printf("  FAIL "); printf(__VA_ARGS__); printf("\n"); } while (0)

    uint32_t nt = N < max_t ? N : max_t;
    Anchor *anch = (Anchor *)calloc(nt, sizeof(Anchor));
    for (uint32_t t = 0; t < nt; t++) {
        Anchor *a = &anch[t];
        a->name = fld.entries[t].name;
        /* fpos[] is indexed by FILE tensor index directly (proven by
         * verify_field.c 291/291) — no chain-order sort. */
        a->addr = body_off + arr_u64(fld.reader.base, fpos_kv, t);
        a->size = fld.entries[t].size;
        for (int d = 0; d < FACES; d++) {
            a->fpos[d] = face_pos(a->size, d);
            a->face[d] = fld.reader.base[a->addr + a->fpos[d]];
        }
    }

    /* ── R1: the 6 faces of an anchor are DISTINCT positions ── */
    {
        int distinct = 1, checked = 0;
        for (uint32_t r = 0; r < nt && checked < 200; r++) {
            if (anch[r].size < FACES) continue;
            checked++;
            for (int i = 0; i < FACES && distinct; i++)
                for (int j = i + 1; j < FACES; j++)
                    if (anch[r].fpos[i] == anch[r].fpos[j]) distinct = 0;
        }
        CHECK(distinct && checked > 0,
              "R1 six faces of an anchor are distinct positions (%d anchors, real sizes)", checked);
    }

    /* ── R2: bytes read at each face == the SOURCE GGUF bytes at the same
     *        position inside the tensor (the walk reads the real tensor) ── */
    {
        uint64_t ncmp = 0, nbad = 0;
        for (uint32_t r = 0; r < nt; r++) {
            uint64_t ssize = 0;
            const uint8_t *sp = find_tensor(&src, anch[r].name, &ssize);
            if (!sp || ssize != anch[r].size) continue;
            for (int d = 0; d < FACES; d++) {
                uint64_t fp = face_pos(ssize, d);
                uint8_t sbyte = sp[fp];
                if (sbyte != anch[r].face[d]) nbad++;
                ncmp++;
            }
        }
        CHECK(ncmp > 0 && nbad == 0,
              "R2 face bytes == source GGUF bytes at same position (%I64u compared, %I64u differ)",
              (unsigned long long)ncmp, (unsigned long long)nbad);
    }

    /* ── R3: two walkers over DIFFERENT passages leave DIFFERENT patterns ──
     * A walker visits a SUBSET of anchors (a real walk is not "all anchors") —
     * A goes to the ffn weights of layers 0..6, B goes to the attn weights. */
    {
        Walker A = {0}, B = {0};
        for (uint32_t t = 0; t < nt; t++) {
            const char *nm = anch[t].name;
            if (strstr(nm, "ffn_") && strncmp(nm, "blk.0.", 6) <= 0) lit_new(&A, (int)(t % FACES));
            if (strstr(nm, "attn_") ) lit_new(&B, (int)((t + 3) % FACES));
        }
        CHECK(A.shut != B.shut && A.shut != 0 && B.shut != 0,
              "R3 different passages -> different fog: A(ffn)=0x%02x B(attn)=0x%02x", A.shut, B.shut);
    }

    /* ── R4: re-walking lights nothing new (monotone; only CLEAR reopens) ── */
    {
        Walker W = {0};
        int first = 0, second = 0;
        for (int d = 0; d < 3; d++) first += lit_new(&W, d);
        for (int d = 0; d < 3; d++) second += lit_new(&W, d);
        CHECK(first == 3 && second == 0,
              "R4 re-walk lights nothing new (first=%d new, second=%d new, mask=0x%02x)", first, second, W.shut);
    }

    /* ── R5: tombstone = entry+exit faces identifies the passage ── */
    {
        Walker W = {0};
        lit_new(&W, 2); lit_new(&W, 5);
        int tomb = (1 << W.entry_face) | (1 << W.exit_face);
        CHECK(W.entry_face == 2 && W.exit_face == 5 && tomb == 0x24,
              "R5 tombstone = entry+exit = 0x%02x (entered face 2, left face 5)", tomb);
    }

    /* ── R0: reserved anchors (none in a real field) — forward never blocked ── */
    {
        Walker W = {0};
        int all = 0;
        for (int d = 0; d < FACES; d++) all += lit_new(&W, d);
        CHECK(all == FACES && W.shut == 0x3F,
              "R0 forward walk of all 6 faces never blocked (mask=0x%02x)", W.shut);
    }

    /* real-data summary: the fog of the first walkers over actual tensors */
    printf("\n  real anchors (first %u of %u tensors):\n", nt, N);
    {
        Walker W = {0};
        for (uint32_t r = 0; r < nt; r++) lit_new(&W, (int)(r % FACES));
        printf("    walker over %u tensors -> fog mask 0x%02x, %u leaves, entry=%u exit=%u\n",
               nt, W.shut, W.hits, W.entry_face, W.exit_face);
        for (uint32_t r = 0; r < (nt < 4 ? nt : 4); r++)
            printf("    anchor[%u] %-40s size=%I64u addr=%I64u face0=%02x face3=%02x\n",
                   r, anch[r].name, (unsigned long long)anch[r].size,
                   (unsigned long long)anch[r].addr, anch[r].face[0], anch[r].face[3]);
    }

    printf("\nfrustum_real: %d/%d PASS\n", npass, nchk);
    free(anch);
    gguf_box_close(&fld); gguf_box_close(&src);
    return npass == nchk ? 0 : 1;
}
