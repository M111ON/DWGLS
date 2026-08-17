/*
 * huff_codec.h — Canonical Huffman codec (256 byte symbols)
 * ═══════════════════════════════════════════════════════════════════════
 * Deterministic + integer-only + header-only — fits the project rules
 * (no float, no malloc, replay-able: same lens → same codec).
 *
 *   huff_build   : freq[256] → tree → lens → canonical model
 *   huff_rebuild : lens[256] (stored form) → canonical model   (decode side)
 *   huff_encode  : symbols → MSB-first bitstream
 *   huff_decode  : bitstream → symbols
 *
 * Stored form = 256 bytes of code lengths — decode rebuilds everything
 * deterministically (DEFLATE-style canonicalization).
 * Fallback: max code length > 32 → fixed 8-bit coding (coded ≤ n bytes).
 *
 * BUILD: nothing (header) — used by hyper_delta.h / tools
 */

#ifndef HUFF_CODEC_H
#define HUFF_CODEC_H

#include <stdint.h>
#include <string.h>

#define HUFF_SYMS 256

typedef struct {
    uint8_t  lens[HUFF_SYMS];    /* code length per symbol (0 = unused)   */
    uint32_t code[HUFF_SYMS];    /* canonical code per symbol (in-memory) */
    uint32_t count[33];          /* symbols per code length               */
    uint32_t first[33];          /* first canonical code per length       */
    uint32_t len_off[33];        /* offset into ordered[] per length      */
    uint8_t  ordered[HUFF_SYMS]; /* symbols sorted by (len, sym)          */
    uint32_t n_syms;
    uint64_t coded_bits;         /* Σ freq×len — for size reporting       */
} HuffModel;

/* ── internal: canonicalize from lens (shared by build/rebuild) ── */
static inline void huff_canonicalize(HuffModel *m) {
    uint32_t count[33] = {0}, off[33], pos[33];
    for (int s = 0; s < HUFF_SYMS; s++) if (m->lens[s]) count[m->lens[s]]++;
    off[1] = 0;
    for (uint32_t len = 2; len <= 32; len++) off[len] = off[len - 1] + count[len - 1];
    memcpy(pos, off, sizeof(pos));
    for (int s = 0; s < HUFF_SYMS; s++)
        if (m->lens[s]) m->ordered[pos[m->lens[s]]++] = (uint8_t)s;
    uint32_t c = 0;
    for (uint32_t len = 1; len <= 32; len++) { m->first[len] = c; c = (c + count[len]) << 1; }
    for (uint32_t len = 1; len <= 32; len++)
        for (uint32_t j = 0; j < count[len]; j++) {
            uint8_t sym = m->ordered[off[len] + j];
            m->code[sym] = m->first[len] + j;
        }
    memcpy(m->count, count, sizeof(count));
    memcpy(m->len_off, off, sizeof(off));
    m->n_syms = 0;
    for (int s = 0; s < HUFF_SYMS; s++) if (m->lens[s]) m->n_syms++;
}

/* ── build from frequencies ── */
static inline void huff_build(HuffModel *m, const uint64_t freq[256]) {
    memset(m, 0, sizeof(*m));

    typedef struct { uint64_t w; int16_t l, r, sym; } HNode;
    HNode nodes[511];
    int nidx = 0;
    for (int s = 0; s < HUFF_SYMS; s++)
        if (freq[s] > 0) {
            nodes[nidx].w = freq[s]; nodes[nidx].l = nodes[nidx].r = -1;
            nodes[nidx].sym = (int16_t)s; nidx++;
        }
    if (nidx == 0) return;
    m->n_syms = (uint32_t)nidx;

    if (nidx == 1) {
        m->lens[nodes[0].sym] = 1;
        huff_canonicalize(m);
        m->coded_bits = freq[nodes[0].sym];
        return;
    }

    /* Huffman tree: repeatedly merge two smallest (O(n²) — 256 syms) */
    uint8_t used[511] = {0};
    int roots = nidx;
    while (roots > 1) {
        int a = -1, b = -1;
        for (int i = 0; i < nidx; i++)
            if (!used[i] && (a < 0 || nodes[i].w < nodes[a].w)) a = i;
        used[a] = 1;
        for (int i = 0; i < nidx; i++)
            if (!used[i] && (b < 0 || nodes[i].w < nodes[b].w)) b = i;
        used[b] = 1;
        nodes[nidx].w = nodes[a].w + nodes[b].w;
        nodes[nidx].l = (int16_t)a; nodes[nidx].r = (int16_t)b; nodes[nidx].sym = -1;
        used[nidx] = 0;
        nidx++;
        roots--;
    }
    int root = -1;
    for (int i = 0; i < nidx; i++) if (!used[i]) { root = i; break; }

    /* depth traversal → lens (fallback: len > 32 → fixed 8-bit) */
    int st[511]; int sd[511]; int sp = 0;
    st[sp] = root; sd[sp] = 0; sp++;
    int maxlen = 0;
    while (sp > 0) {
        sp--;
        int nd = st[sp], dep = sd[sp];
        if (nodes[nd].sym >= 0) {
            m->lens[nodes[nd].sym] = (uint8_t)dep;
            if (dep > maxlen) maxlen = dep;
        } else {
            st[sp] = nodes[nd].l; sd[sp] = dep + 1; sp++;
            st[sp] = nodes[nd].r; sd[sp] = dep + 1; sp++;
        }
    }
    if (maxlen > 32) {
        /* pathological — fixed 8-bit */
        for (int s = 0; s < HUFF_SYMS; s++) if (freq[s] > 0) m->lens[s] = 8;
    }
    huff_canonicalize(m);
    m->coded_bits = 0;
    for (int s = 0; s < HUFF_SYMS; s++)
        if (m->lens[s]) m->coded_bits += (uint64_t)freq[s] * m->lens[s];
}

/* ── rebuild from stored lens (decode side) ── */
static inline void huff_rebuild(HuffModel *m, const uint8_t lens[256]) {
    memset(m, 0, sizeof(*m));
    memcpy(m->lens, lens, HUFF_SYMS);
    huff_canonicalize(m);
}

/* ── encode: symbols → MSB-first bitstream (returns bytes) ── */
static inline uint32_t huff_encode(const HuffModel *m, const uint8_t *data,
                                   uint32_t n, uint8_t *out, uint32_t cap) {
    uint64_t bitbuf = 0;
    uint32_t nbits = 0, opos = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t sym = data[i];
        uint32_t len = m->lens[sym];
        uint32_t code = m->code[sym];
        if (len == 0) return 0;                 /* unknown symbol */
        bitbuf = (bitbuf << len) | code;
        nbits += len;
        while (nbits >= 8) {
            if (opos >= cap) return 0;
            out[opos++] = (uint8_t)(bitbuf >> (nbits - 8));
            nbits -= 8;
        }
    }
    if (nbits) {
        if (opos >= cap) return 0;
        out[opos++] = (uint8_t)(bitbuf << (8 - nbits));
    }
    return opos;
}

/* ── decode: bitstream → symbols (returns 0 on success) ── */
static inline int huff_decode(const HuffModel *m, const uint8_t *in,
                              uint32_t in_len, uint8_t *out, uint32_t n) {
    uint32_t bp = 0;
    uint64_t total_bits = (uint64_t)in_len * 8;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t code = 0;
        int ok = 0;
        for (uint32_t len = 1; len <= 32; len++) {
            if (bp >= total_bits) return -1;
            code = (code << 1) | ((in[bp >> 3] >> (7 - (bp & 7))) & 1u);
            bp++;
            if (m->count[len] && code >= m->first[len] &&
                code - m->first[len] < m->count[len]) {
                out[i] = m->ordered[m->len_off[len] + (uint32_t)(code - m->first[len])];
                ok = 1;
                break;
            }
        }
        if (!ok) return -1;
    }
    return 0;
}

#endif /* HUFF_CODEC_H */
