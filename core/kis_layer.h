/* kis_layer.h — Single source of truth for layer slot counts */
#ifndef KIS_LAYER_H
#define KIS_LAYER_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════
   SACRED CONSTANTS — don't touch
   ═══════════════════════════════════════════════════════ */
#define ICO_SLOTS  20u    /* icosa layer */
#define DEC_SLOTS  12u    /* dodeca layer */
#define SLOT_SZ    64u    /* bytes per slot */

/* ═══════════════════════════════════════════════════════
   ALTERNATING PATTERN
   ═══════════════════════════════════════════════════════ */
#define KIS_SLOTS(n)       (((n) & 1) ? DEC_SLOTS : ICO_SLOTS)

/* Total slots up to layer n (exclusive) */
#define KIS_TOTAL(n)       ((uint64_t)((n) >> 1) * (ICO_SLOTS + DEC_SLOTS) + \
                            (uint64_t)((n) & 1) * ICO_SLOTS)

/* Byte offset for layer n */
#define KIS_OFFSET(n)      (KIS_TOTAL(n) * SLOT_SZ)

/* Address: (layer, k) → byte offset */
#define KIS_ADDR(n, k)     (KIS_OFFSET(n) + (uint64_t)(k) * SLOT_SZ)

/* Verify k in bounds */
#define KIS_VALID(n, k)    ((k) < KIS_SLOTS(n))

#endif /* KIS_LAYER_H */
