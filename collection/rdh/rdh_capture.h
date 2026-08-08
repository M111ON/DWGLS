#ifndef RDH_CAPTURE_H
#define RDH_CAPTURE_H
#include <stdint.h>
typedef struct { uint32_t slot; uint8_t data[64]; } RdhCapture;
static inline void rdh_capture_init(RdhCapture *c) { c->slot = 0; }
#endif
