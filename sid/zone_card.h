#pragma once
#include <stdint.h>
#include <stddef.h>

#define CARD_SPARSE 0
#define CARD_BATCH  1
#define CARD_LZ     2
#define NO_NEIGHBOR 0xFFFF

/* v2: 12 bytes */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint8_t  card_type;
    uint8_t  entropy;
    uint16_t pattern;
    uint8_t  locality;
    uint8_t  stability;
    uint16_t neighbor_left;
    uint16_t neighbor_right;
} ZoneCard;

_Static_assert(sizeof(ZoneCard) == 12, "ZoneCard v2 must be 12 bytes");

/* Extended: 20 bytes (+hash) */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint8_t  card_type;
    uint8_t  entropy;
    uint16_t pattern;
    uint8_t  locality;
    uint8_t  stability;
    uint16_t neighbor_left;
    uint16_t neighbor_right;
    uint64_t hash_val;
} ZoneCardExt;

_Static_assert(sizeof(ZoneCardExt) == 20, "ZoneCardExt v2 must be 20 bytes");

static inline uint64_t zone_card_hash(const uint8_t *data, size_t len) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++) { h ^= data[i]; h *= 0x100000001B3ULL; }
    return h;
}

static inline uint8_t zone_card_entropy(const uint8_t *data, size_t len) {
    if (!data || len == 0) return 0;
    uint32_t counts[256] = {0};
    for (size_t i = 0; i < len; i++) counts[data[i]]++;
    double ent = 0.0;
    for (int i = 0; i < 256; i++) {
        if (counts[i]) { double p = (double)counts[i] / (double)len; ent -= p * __builtin_log2(p); }
    }
    uint8_t s = (uint8_t)((ent / 8.0) * 255.0);
    return s > 255 ? 255 : s;
}

static inline uint8_t zone_card_stability(const float *arr, size_t rows, size_t cols) {
    if (!arr || rows < 2) return 128;
    double sum = 0.0, sum_abs = 0.0;
    for (size_t r = 1; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            double d = (double)arr[r * cols + c] - (double)arr[(r-1) * cols + c];
            sum += (d < 0 ? -d : d);
            double v = (double)arr[(r-1) * cols + c];
            sum_abs += (v < 0 ? -v : v);
        }
    }
    double avg_diff = sum / ((rows-1) * cols);
    double avg_mag = sum_abs / (rows * cols);
    if (avg_mag < 1e-10) return 255;
    double norm = avg_diff / avg_mag;
    if (norm > 1.0) norm = 1.0;
    return (uint8_t)((1.0 - norm) * 255.0);
}

static inline uint8_t zone_card_locality_from_cards(const ZoneCard *self,
                                                     const ZoneCard *neighbors,
                                                     int n_neighbors) {
    if (!neighbors || n_neighbors == 0) return 128;
    int total = 0;
    for (int i = 0; i < n_neighbors; i++) {
        int s = 0;
        if (self->card_type == neighbors[i].card_type) s += 102;
        int ed = (int)self->entropy - (int)neighbors[i].entropy;
        if (ed < 0) ed = -ed;
        s += (76 - ed / 3);
        if (s < 0) s = 0;
        total += s;
    }
    int avg = total / n_neighbors;
    return avg > 255 ? 255 : (uint8_t)(avg < 0 ? 0 : avg);
}

static inline uint8_t zone_card_sparsity_f(const float *arr, size_t n, float threshold) {
    if (!arr || n == 0) return 0;
    double sum = 0.0, sum2 = 0.0;
    for (size_t i = 0; i < n; i++) { sum += arr[i]; sum2 += arr[i] * arr[i]; }
    double mean = sum / n, var = sum2 / n - mean * mean;
    if (var < 1e-16) return (mean == 0.0) ? 255 : 0;
    double std = __builtin_sqrt(var);
    size_t near_zero = 0;
    for (size_t i = 0; i < n; i++)
        if (__builtin_fabs((double)arr[i]) < threshold * std) near_zero++;
    return (uint8_t)((near_zero * 255) / n);
}

static inline int zone_card_type_f(const float *arr, size_t n) {
    if (!arr || n == 0) return CARD_BATCH;
    double sum = 0.0, sum2 = 0.0;
    for (size_t i = 0; i < n; i++) { sum += arr[i]; sum2 += arr[i] * arr[i]; }
    double mean = sum / n, var = sum2 / n - mean * mean;
    if (var < 1e-16) return (mean == 0.0) ? CARD_SPARSE : CARD_BATCH;
    if (zone_card_sparsity_f(arr, n, 0.05f) > 178) return CARD_SPARSE; /* >0.7 */
    return CARD_LZ;
}

static inline uint16_t zone_card_pattern_f(const float *arr, size_t n) {
    if (!arr || n == 0) return 0;
    uint16_t code = 0;
    if (zone_card_sparsity_f(arr, n, 0.01f) > 127) code |= 0x01;
    if (zone_card_entropy((const uint8_t*)arr, n * sizeof(float)) < 100) code |= 0x02;
    size_t alt = 0;
    for (size_t i = 1; i < n; i++)
        if ((arr[i] > 0 && arr[i-1] < 0) || (arr[i] < 0 && arr[i-1] > 0)) alt++;
    if ((float)alt / n > 0.3f) code |= 0x08;
    uint64_t h = zone_card_hash((const uint8_t*)arr, n * sizeof(float));
    code |= (h & 0xFF) << 8;
    return code;
}

static inline ZoneCard zone_card_make(const float *arr, size_t n,
                                      uint16_t id,
                                      uint16_t nl, uint16_t nr) {
    ZoneCard card;
    card.id = id;
    card.card_type = zone_card_type_f(arr, n);
    card.entropy = zone_card_entropy((const uint8_t*)arr, n * sizeof(float));
    card.pattern = zone_card_pattern_f(arr, n);
    card.locality = 128;
    card.stability = 128;
    card.neighbor_left = nl;
    card.neighbor_right = nr;
    return card;
}

static inline ZoneCardExt zone_card_make_ext(const float *arr, size_t n,
                                              uint16_t id,
                                              uint16_t nl, uint16_t nr) {
    ZoneCardExt card;
    card.id = id;
    card.card_type = zone_card_type_f(arr, n);
    card.entropy = zone_card_entropy((const uint8_t*)arr, n * sizeof(float));
    card.pattern = zone_card_pattern_f(arr, n);
    card.locality = 128;
    card.stability = 128;
    card.neighbor_left = nl;
    card.neighbor_right = nr;
    card.hash_val = zone_card_hash((const uint8_t*)arr, n * sizeof(float));
    return card;
}
