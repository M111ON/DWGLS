#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "dramtile_store.h"

typedef struct {
    DtTensorView  view;
    size_t        elem_size;
    size_t        stride[6];
} DtContainer;

static inline size_t dtc_elem_size(uint32_t dtype) {
    switch (dtype) {
        case DT_F32: return 4;
        case DT_F16: return 2;
        case DT_I32: return 4;
        case DT_I8:  return 1;
        case DT_Q40: return sizeof(uint16_t) + 16;
        case DT_Q80: return sizeof(uint16_t) + 32;
        default:     return 1;
    }
}

static inline int dtc_is_kv(DtContainer *c) {
    return (c->view.dram_addr & DT_KV_FLAG) != 0;
}
static inline int dtc_is_bond(DtContainer *c) {
    return (c->view.dram_addr & DT_BOND_FLAG) != 0;
}
static inline int dtc_is_delta(DtContainer *c) {
    return (c->view.dram_addr & DT_DELTA_FLAG) != 0;
}

static inline DtContainer dtc_wrap(DtTensorView *view, uint32_t dtype) {
    DtContainer c;
    memset(&c, 0, sizeof(c));
    if (!view || !view->data) return c;
    c.view      = *view;
    c.view.dtype = dtype;
    c.elem_size = dtc_elem_size(dtype);
    size_t stride = c.elem_size;
    for (int i = c.view.ndim - 1; i >= 0; i--) {
        c.stride[i] = stride;
        stride *= c.view.shape[i];
    }
    return c;
}

static inline size_t dtc_offset(DtContainer *c, const size_t *indices) {
    size_t off = 0;
    for (int i = 0; i < c->view.ndim; i++)
        off += indices[i] * c->stride[i];
    return off;
}

static inline float dtc_f32_1d(DtContainer *c, size_t i0) {
    size_t idx[] = {i0};
    float val; memcpy(&val, c->view.data + dtc_offset(c, idx), 4);
    return val;
}
static inline float dtc_f32_2d(DtContainer *c, size_t i0, size_t i1) {
    size_t idx[] = {i0, i1};
    float val; memcpy(&val, c->view.data + dtc_offset(c, idx), 4);
    return val;
}
static inline float dtc_f32_3d(DtContainer *c, size_t i0, size_t i1, size_t i2) {
    size_t idx[] = {i0, i1, i2};
    float val; memcpy(&val, c->view.data + dtc_offset(c, idx), 4);
    return val;
}

static inline uint16_t dtc_f16_1d(DtContainer *c, size_t i0) {
    size_t idx[] = {i0};
    uint16_t val; memcpy(&val, c->view.data + dtc_offset(c, idx), 2);
    return val;
}
static inline uint16_t dtc_f16_2d(DtContainer *c, size_t i0, size_t i1) {
    size_t idx[] = {i0, i1};
    uint16_t val; memcpy(&val, c->view.data + dtc_offset(c, idx), 2);
    return val;
}

static inline int32_t dtc_i32_1d(DtContainer *c, size_t i0) {
    size_t idx[] = {i0};
    int32_t val; memcpy(&val, c->view.data + dtc_offset(c, idx), 4);
    return val;
}

static inline int8_t dtc_i8_1d(DtContainer *c, size_t i0) {
    return (int8_t)c->view.data[dtc_offset(c, (size_t[]){i0})];
}

static inline DtContainer dtc_slice(DtContainer *c, int dim, size_t start, size_t end) {
    DtContainer s;
    memset(&s, 0, sizeof(s));
    if (dim < 0 || dim >= c->view.ndim || start >= end || end > c->view.shape[dim])
        return s;
    s.view      = c->view;
    s.elem_size = c->elem_size;
    s.view.ndim = c->view.ndim;
    s.view.data = c->view.data + start * c->stride[dim];
    s.view.shape[dim] = (uint32_t)(end - start);
    size_t stride = s.elem_size;
    for (int i = s.view.ndim - 1; i >= 0; i--) {
        s.stride[i] = stride;
        stride *= s.view.shape[i];
    }
    return s;
}

static inline DtContainer dtc_flatten(DtContainer *c) {
    DtContainer f;
    memset(&f, 0, sizeof(f));
    f.view      = c->view;
    f.elem_size = c->elem_size;
    f.view.ndim = 1;
    size_t total = 1;
    for (int i = 0; i < c->view.ndim; i++) total *= c->view.shape[i];
    f.view.shape[0] = (uint32_t)total;
    f.stride[0] = f.elem_size;
    return f;
}

static inline void *dtc_ptr(DtContainer *c, const size_t *indices) {
    return c->view.data + dtc_offset(c, indices);
}

static inline int dtc_promote_to_cold(DtContainer *c, DRamTileStore *store) {
    if (!dtc_is_kv(c)) return -1;
    if (!store->cold_base) return -1;
    size_t sz = c->view.nbytes;
    uint8_t *cold_ptr = dt_cold_alloc(store, sz);
    if (!cold_ptr) return -1;
    memcpy(cold_ptr, c->view.data, sz);
    c->view.data = cold_ptr;
    return 0;
}

static inline uint8_t *dtc_delta_compose(DtContainer *c,
                                          DRamTileStore *store,
                                          uint8_t *dst)
{
    if (!dtc_is_delta(c) || !dst) return NULL;
    uint32_t slot = (c->view.dram_addr & ~DT_FLAGS_MASK) % DT_HASH_SLOTS;
    size_t sz = c->view.nbytes;
    kv_delta_compose_read(store, slot, dst, sz);
    c->view.data = dst;
    return dst;
}

static inline void dtc_print(DtContainer *c, const char *label) {
    fprintf(stderr, "[dtc] %s: dtype=%u ndim=%d flags=%s%s%s shape=[",
            label, c->view.dtype, c->view.ndim,
            dtc_is_kv(c)    ? "KV" : "",
            dtc_is_bond(c)  ? "|BOND" : "",
            dtc_is_delta(c) ? "|DELTA" : "");
    for (int i = 0; i < c->view.ndim; i++)
        fprintf(stderr, "%s%u", i ? "," : "", c->view.shape[i]);
    fprintf(stderr, "] elem_sz=%zu strides=[", c->elem_size);
    for (int i = 0; i < c->view.ndim; i++)
        fprintf(stderr, "%s%zu", i ? "," : "", c->stride[i]);
    fprintf(stderr, "] ptr=%p\n", (void*)c->view.data);
}
