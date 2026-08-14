/*
 * tw_tensor_capture.h — TW Capture from Real Quantized Tensor Data
 *
 * Bridge between ggml-quantized tensor bytes (Q8_0) and TW capture.
 * Reads raw .qdat bytes as stored by build_smollm2_store.py,
 * dequantizes to float, computes a 2D signature, runs TW capture.
 *
 * Q8_0 block: 2B f16 scale + 32 int8 quants = 34B per 32 elements.
 * 2D signature: mean(first half of first row), mean(second half of first row)
 *
 * Usage:
 *   #include "tw_tensor_capture.h"
 *   // via raw bridge
 *   RawBridge rb;
 *   rb_load(&rb, "path/to/tensors_raw/");
 *   TWTensorCapture tc;
 *   tw_capture_tensor_by_name(&rb, "model.layers.0.attn.q_proj.weight", &tc);
 *
 *   // or via raw bytes
 *   tw_capture_tensor_raw(bytes, nbytes, rows, cols, &tc);
 */
#ifndef TW_TENSOR_CAPTURE_H
#define TW_TENSOR_CAPTURE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "tw_capture_int.h"
#include "geom_raw_bridge.h"

/* ── Q8_0 constants ── */
#define TW_Q8_BLOCK_SZ   32
#define TW_Q8_BLOCK_BYTES 34   /* 2B f16 scale + 32 int8 quants */

#define TW_TENSOR_MAX_ROWS 128
#define TW_TENSOR_MAX_COLS 4096

typedef struct {
    TWCaptureInt cap;      /* primary TW capture result */
    float        sig_x;    /* 2D signature float values (before scaling) */
    float        sig_y;
    uint32_t     n_dequant; /* number of elements dequantized */
    float        mean;     /* overall mean of dequantized data */
    float        std;      /* overall std of dequantized data */
} TWTensorCapture;

/* ── F16 half → float ── */
static inline float _tw_half_to_float(uint16_t h) {
    uint32_t sign  = (h >> 15) & 1;
    uint32_t exp   = (h >> 10) & 0x1F;
    uint32_t mant  = h & 0x3FF;
    if (exp == 0) {
        /* Subnormal or zero: value = 2^(-14) * (mant / 1024) = mant * 2^(-24) */
        float r = (float)mant * 5.960464477539063e-8f;
        return sign ? -r : r;
    }
    uint32_t f;
    if (exp == 0x1F) {
        f = (sign << 31) | 0x7F800000 | (mant << 13);
    } else {
        f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    }
    float result;
    memcpy(&result, &f, sizeof(result));
    return result;
}

/*
 * Dequantize F32 raw bytes into float buffer (direct copy).
 * Returns actual number of values dequantized.
 */
static inline uint32_t _tw_dequant_f32(const uint8_t *raw, size_t nbytes,
                                        float *buf, uint32_t max_vals)
{
    uint32_t n = (uint32_t)(nbytes / 4);
    if (n > max_vals) n = max_vals;
    for (uint32_t i = 0; i < n; i++) {
        float val;
        memcpy(&val, raw + i * 4, 4);
        buf[i] = (isfinite(val) && val > -1e10f && val < 1e10f) ? val : 0.0f;
    }
    return n;
}

/*
 * Dequantize Q8_0 blocks into float buffer.
 * Returns actual number of values dequantized.
 */
static inline uint32_t _tw_dequant_q80(const uint8_t *raw, size_t nbytes,
                                        float *buf, uint32_t max_vals)
{
    uint32_t n_blocks = (uint32_t)(nbytes / TW_Q8_BLOCK_BYTES);
    uint32_t total = n_blocks * TW_Q8_BLOCK_SZ;
    if (total > max_vals) total = max_vals;
    if (total == 0) return 0;

    uint32_t out_idx = 0;
    for (uint32_t b = 0; b < n_blocks && out_idx < total; b++) {
        uint16_t scale_bits;
        memcpy(&scale_bits, raw + b * TW_Q8_BLOCK_BYTES, 2);
        float scale = _tw_half_to_float(scale_bits);
        if (!isfinite(scale) || scale == 0.0f) scale = 1.0f;

        for (uint32_t i = 0; i < TW_Q8_BLOCK_SZ && out_idx < total; i++) {
            int8_t q = (int8_t)raw[b * TW_Q8_BLOCK_BYTES + 2 + i];
            buf[out_idx++] = (float)q * scale;
        }
    }
    return out_idx;
}

/*
 * Compute 2D signature from dequantized float buffer.
 * sig_x = mean of first half, sig_y = mean of second half.
 * If n < 2, pads with zeros.
 */
static inline void _tw_compute_sig(const float *buf, uint32_t n,
                                    float *sig_x, float *sig_y,
                                    float *mean_out, float *std_out)
{
    if (n == 0) { *sig_x = 0; *sig_y = 0; *mean_out = 0; *std_out = 0; return; }

    double sum = 0, sum2 = 0;
    for (uint32_t i = 0; i < n; i++) {
        sum  += buf[i];
        sum2 += (double)buf[i] * buf[i];
    }
    float mean = (float)(sum / n);
    float var  = (float)(sum2 / n - mean * mean);
    if (var < 0) var = 0;
    *mean_out = mean;
    *std_out  = sqrtf(var);

    uint32_t half = n / 2;
    if (half == 0) { *sig_x = buf[0]; *sig_y = 0; return; }

    double sx = 0, sy = 0;
    for (uint32_t i = 0; i < half; i++) sx += buf[i];
    for (uint32_t i = half; i < n; i++) sy += buf[i];
    *sig_x = (float)(sx / half);
    *sig_y = (float)(sy / (n - half));
}

/*
 * TW capture from raw tensor bytes.
 * dtype: 0=F32, 8=Q8_0, other default to F32.
 * rows, cols: expected tensor shape (for stride calculation).
 * If rows=0 or cols=0, reads first N elements as flat array.
 */
static inline void tw_capture_tensor_raw(
    const uint8_t *raw, size_t nbytes,
    uint32_t rows, uint32_t cols,
    int dtype,
    TWTensorCapture *out)
{
    memset(out, 0, sizeof(*out));

    uint32_t max_vals = TW_TENSOR_MAX_COLS;
    float buf[TW_TENSOR_MAX_COLS];
    uint32_t n;

    if (dtype == 0) {
        n = _tw_dequant_f32(raw, nbytes, buf, max_vals);
    } else {
        n = _tw_dequant_q80(raw, nbytes, buf, max_vals);
    }

    out->n_dequant = n;
    _tw_compute_sig(buf, n, &out->sig_x, &out->sig_y, &out->mean, &out->std);

    /* scale to TW_SCALE using int64 (sig can be ~20000, SCALE=207360 → 4.1B > int32) */
    int64_t vx = (int64_t)(out->sig_x * TW_SCALE);
    int64_t vy = (int64_t)(out->sig_y * TW_SCALE);
    tw_capture_int(vx, vy, &out->cap);
}

/*
 * TW capture from RawBridge by tensor name.
 * rb: loaded RawBridge with .qdat files.
 * name: tensor name (e.g. "model.layers.0.attn.q_proj.weight")
 * Returns RB_OK on success, RB_ERR if tensor not found.
 */
static inline int tw_capture_tensor_by_name(
    RawBridge *rb, const char *name,
    TWTensorCapture *out)
{
    void *data;
    size_t size;
    if (rb_get(rb, name, &data, &size) != RB_OK)
        return RB_ERR;

    /* Find dtype by linear scan of occupied entries */
    int dtype = 8;
    for (uint32_t i = 0; i < RB_MAX_ENTRIES; i++) {
        if (rb->entries[i].occupied && strcmp(rb->entries[i].name, name) == 0) {
            dtype = rb->entries[i].dtype;
            break;
        }
    }

    tw_capture_tensor_raw((const uint8_t *)data, size, 0, 0, dtype, out);
    return RB_OK;
}

#endif /* TW_TENSOR_CAPTURE_H */
