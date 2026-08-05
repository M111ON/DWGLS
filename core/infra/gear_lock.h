#pragma once

#include <stdint.h>

#define GEAR_CPU_WORLD      128u
#define GEAR_GPU_WORLD      162u
#define GEAR_C144_CYCLE     144u
#define GEAR_GEO_FULL       (GEAR_CPU_WORLD * GEAR_GPU_WORLD)

typedef struct {
    volatile const uint8_t *c144_ref;
    uint32_t               cpu_ops;
    uint32_t               gpu_ops;
    uint32_t               cpu_worlds;
    uint32_t               gpu_worlds;
} GearLock;

static inline uint32_t gear_tag(const GearLock *g) {
    return g->c144_ref ? (uint32_t)*g->c144_ref : 0;
}

static inline void gear_cpu_tick(GearLock *g) {
    g->cpu_ops++;
    if (g->cpu_ops % GEAR_CPU_WORLD == 0)
        g->cpu_worlds = g->cpu_ops / GEAR_CPU_WORLD;
}

static inline void gear_gpu_tick(GearLock *g, uint32_t n) {
    uint32_t prev = g->gpu_ops;
    g->gpu_ops += n;
    if ((prev % GEAR_GPU_WORLD) + n >= GEAR_GPU_WORLD)
        g->gpu_worlds = g->gpu_ops / GEAR_GPU_WORLD;
}
