#ifndef BEAM_TIMER_H
#define BEAM_TIMER_H
#include <stdint.h>
typedef struct { uint32_t step; uint32_t tick; } BeamTimer;
static inline void beam_timer_init(BeamTimer *t) { t->step = 0; t->tick = 0; }
static inline void beam_timer_advance(BeamTimer *t) { t->tick++; }
#endif
