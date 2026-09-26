/*
 * zone_card_sid.h — ZoneCard + SID coordinate + inference signal
 * Layout: resid(16) + card(12) + inf(8) + node_id(4) + capo_key(2) = 42B packed
 *
 * Two-phase:
 *   1. zcsid_make()       — static capture (weight-time)
 *   2. zcsid_hook_logits() — runtime update (forward pass)
 *
 * Y-Triangle retarget (replaces old TRing 1440 / dodeca 12-face):
 *   geo_pentagon_id(node_id) → face 1..12
 *   geo_shell_level(node_id) → layer 0..11
 *   geo_clock_tick(node_id)  → position within pentagon (0..1439)
 *   geo_capo(node_id, key)   → offset routing within pentagon
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "zone_card.h"
#define GEO_JUMP_INLINE
#include "geo_jump.h"

#define ZCSID_FLAG_FROZEN 0x01
#define ZCSID_FLAG_DRAIN  0x02
#define ZCSID_FLAG_ACTIVE 0x04

/* 8B — filled by runtime hook, proxy values until then */
typedef struct {
    uint8_t  logit_entropy; /* perplexity proxy: high=confused, low=certain */
    uint8_t  confidence;    /* 255 - logit_entropy                           */
    uint16_t tick;          /* token position in sequence                    */
    uint8_t  flags;         /* FROZEN | DRAIN | ACTIVE                       */
    uint8_t  layer;         /* geo_shell_level(node_id) 0..11               */
    uint16_t clock_tick;    /* geo_clock_tick(node_id) 0..1439              */
} ZCSIDInference;

_Static_assert(sizeof(ZCSIDInference) == 8, "ZCSIDInference must be 8B packed");

/* 42B packed — core unit for geometric tensor memory */
typedef struct __attribute__((packed)) {
    int64_t        resid_x;   /* residual X (TW_SCALE units)      */
    int64_t        resid_y;   /* residual Y (TW_SCALE units)      */
    ZoneCard       card;      /* 12B: pattern/entropy/neighbors   */
    ZCSIDInference inf;       /* 8B: runtime signal               */
    uint32_t       node_id;   /* Y-triangle address 0..20735      */
    uint16_t       capo_key;  /* geo_capo routing key             */
} ZoneCardSID;

_Static_assert(sizeof(ZoneCardSID) == 42, "ZoneCardSID must be 42B (packed)");

/* Phase 1: static capture from weight data */
static inline ZoneCardSID zcsid_make(const ZoneCard *card,
                                     uint32_t node_id,
                                     uint16_t capo_key,
                                     uint16_t tick,
                                     uint8_t flags,
                                     int64_t resid_x, int64_t resid_y)
{
    ZoneCardSID z;
    z.resid_x   = resid_x;
    z.resid_y   = resid_y;
    z.card      = *card;
    z.node_id   = node_id;
    z.capo_key  = capo_key;
    z.inf = (ZCSIDInference){
        .logit_entropy = card->entropy,
        .confidence    = (uint8_t)(255 - card->entropy),
        .tick          = tick,
        .flags         = flags,
        .layer         = (uint8_t)geo_shell_level(node_id),
        .clock_tick    = (uint16_t)geo_clock_tick(node_id),
    };
    return z;
}

/* Phase 2: update with real logits during forward pass */
static inline void zcsid_hook_logits(ZoneCardSID *z,
                                     const float *logits, size_t n_vocab,
                                     uint16_t tick)
{
    z->inf.logit_entropy = zone_card_entropy((const uint8_t*)logits,
                                              n_vocab * sizeof(float));
    z->inf.confidence    = (uint8_t)(255 - z->inf.logit_entropy);
    z->inf.tick          = tick;
    z->inf.flags        |= ZCSID_FLAG_ACTIVE;
}

/* Topology check: same pentagon + adjacent layer = same dependency cluster */
static inline int zcsid_linked(const ZoneCardSID *a, const ZoneCardSID *b)
{
    uint32_t pa = geo_pentagon_id(a->node_id);
    uint32_t pb = geo_pentagon_id(b->node_id);
    if (pa != pb) return 0;
    int dl = (int)a->inf.layer - (int)b->inf.layer;
    return (dl < 0 ? -dl : dl) <= 1;
}
