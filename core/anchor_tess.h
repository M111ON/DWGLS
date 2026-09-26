/* anchor_tess.h — semantic joint: anchor-routed tensor retrieval into the
 * tess tile-serve path. Tensor centroid = 8 chunk-means of raw tensor bytes
 * (deterministic: same bytes -> same vector, no content knowledge).
 * Chain: centroid query -> anch_route top-b -> tile-load only routed tensors.
 */
#ifndef ANCHOR_TESS_H
#define ANCHOR_TESS_H

#include <stdint.h>
#include "anchor_route.h"

#ifndef AT_DIM
/* Sweep 2026-09-26 (291 tensors qwen2.5, stable@5/@50): d4=113/6,
 * d8=265/146, d12=277/177, d16=288/54, d24=289/100. 16 wins: most
 * stable under small noise AND steepest falloff under large. */
#define AT_DIM 16u
#endif

/* bytes per cell for GGML type codes 0..15 (41 handled by caller if needed). */
static inline uint32_t at_cell_size(uint8_t dtype) {
    static const uint32_t sz[] = {
        4, 2, 18, 20, 0, 0, 22, 24, 34, 36, 84, 110, 144, 176, 210, 292,
    };
    return dtype < 16 ? sz[dtype] : 0;
}

/* 8 chunk-means over [ptr, ptr+size). size==0 -> zero vector. */
static inline void at_centroid(const uint8_t *ptr, uint32_t size, float *out) {
    for (uint32_t d = 0; d < AT_DIM; d++) out[d] = 0.0f;
    if (!ptr || size == 0) return;
    for (uint32_t d = 0; d < AT_DIM; d++) {
        uint32_t s = (uint32_t)((uint64_t)size * d / AT_DIM);
        uint32_t e = (uint32_t)((uint64_t)size * (d + 1) / AT_DIM);
        if (e <= s) e = s + 1;
        uint64_t acc = 0;
        for (uint32_t i = s; i < e && i < size; i++) acc += ptr[i];
        out[d] = (float)acc / (float)(e - s);
    }
}

#endif /* ANCHOR_TESS_H */
