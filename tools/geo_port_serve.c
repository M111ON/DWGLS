/*
 * tools/geo_port_serve.c — Phase 2: NEGATIVE PORT for window overflow
 * ════════════════════════════════════════════════════════════════════
 * The case that used to FAIL: LFM2.5-2.6B needs 22,014 parts but the
 * window has only 20,736 slots. Old behaviour: "FAIL exceeds window".
 *
 * New behaviour — overflow goes through the NEGATIVE PORT into a
 * RESIDUAL SPILL file, with a BOND REGISTRY pointing home:
 *
 *   part p <  FIELD_PARTS : field slot p        (the "world")
 *   part p >= FIELD_PARTS : residual spill      (the "space")
 *                          bond[p] = spill offset
 *
 * Read path replays the bond — lossless either way.
 * Carried state = bond entries (∝ overflow parts ONLY, never ∝ data).
 *
 * Oracle: memcmp every part vs source, both tiers.
 *
 * BUILD: gcc -O2 -Wall -D__USE_MINGW_ANSI_STDIO=1 -Icore -o build/geo_port_serve tools/geo_port_serve.c -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#ifndef FSCTL_SET_SPARSE
#define FSCTL_SET_SPARSE CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 35, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

#include "../core/gguf_reader.h"
#include "../core/kis_cube_views.h"
#include "../core/iso_fold.h"

#define PART_BYTES   (128u * 1024u)
#define N_CUBES      12u
#define MAX_PARTS    (N_CUBES * 1728u)          /* 20,736 = field capacity */

typedef struct { uint32_t part; uint64_t spill_off; } Bond;

static HANDLE map_file(const char *path, uint64_t size, int sparse,
                       HANDLE *hOut, HANDLE *mapOut, uint8_t **view) {
    DeleteFileA(path);
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (sparse) {
        DWORD d = 0;
        DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &d, NULL);
    }
    LONG hi = (LONG)(size >> 32), lo = (LONG)(size & 0xFFFFFFFFu);
    SetFilePointer(h, lo, &hi, FILE_BEGIN);
    SetEndOfFile(h);
    HANDLE m = CreateFileMappingA(h, NULL, PAGE_READWRITE, hi, lo, NULL);
    if (!m) { CloseHandle(h); return NULL; }
    *view = (uint8_t *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!*view) { CloseHandle(m); CloseHandle(h); return NULL; }
    *hOut = h; *mapOut = m;
    return view ? *view : NULL;
}

static inline size_t part_offset(uint32_t f) {
    uint32_t tes = f / ISO_TES_SIZE;
    uint32_t rem = f % ISO_TES_SIZE;
    IsoFold fo   = iso_fold(tes, rem / ISO_TES_SLOTS, rem % ISO_TES_SLOTS);
    return (size_t)iso_unfold(&fo) * PART_BYTES;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1]
        : "I:\\model\\LFM2.5-2.6B-Q8_0.gguf";

    GgufReader r;
    if (gguf_open((char *)path, &r) != 0) { printf("FAIL open gguf\n"); return 1; }
    printf("=== geo_port_serve — negative port for window overflow ===\n");
    printf("%s · %u tensors\n", path, r.n_tensors);

    uint64_t total_bytes = 0; uint32_t total_parts = 0;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        total_bytes += r.sizes[i];
        total_parts += (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
    }
    printf("parts %u (%.1f MB) · field capacity %u\n",
           total_parts, (double)total_bytes / 1e6, MAX_PARTS);

    uint32_t n_field = total_parts < MAX_PARTS ? total_parts : MAX_PARTS;
    uint32_t n_resid = total_parts - n_field;
    printf("plan : in-field %u · via negative port %u\n\n", n_field, n_resid);

    /* ── open both tiers ─────────────────────────────────────────────── */
    HANDLE hf, mf, hr, mr;
    uint8_t *field, *resid;
    if (!map_file("build\\port_field.geo",  (uint64_t)n_field * PART_BYTES, 1, &hf, &mf, &field)) {
        printf("FAIL field\n"); return 1;
    }
    uint64_t resid_sz = (uint64_t)(n_resid ? n_resid : 1) * PART_BYTES;
    if (!map_file("build\\port_residual.geo", resid_sz, 1, &hr, &mr, &resid)) {
        printf("FAIL residual\n"); return 1;
    }

    /* ── BAKE through the port ───────────────────────────────────────── */
    Bond *bonds = (Bond *)malloc(sizeof(Bond) * (n_resid ? n_resid : 1));
    double t0 = now_ms();
    uint32_t n_bonds = 0;
    for (uint32_t fid = 0, i = 0; i < r.n_tensors; i++) {
        const uint8_t *src = r.base + r.data_offset + r.offsets[i];
        uint32_t np = (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
        for (uint32_t p = 0; p < np; p++, fid++) {
            uint32_t off = p * PART_BYTES;
            uint32_t len = r.sizes[i] - off; if (len > PART_BYTES) len = PART_BYTES;
            if (fid < n_field) {
                memcpy(field + part_offset(fid), src + off, len);
            } else {
                uint32_t q = fid - n_field;
                bonds[n_bonds].part = fid;
                bonds[n_bonds].spill_off = (uint64_t)q * PART_BYTES;
                memcpy(resid + bonds[n_bonds].spill_off, src + off, len);
                n_bonds++;
            }
        }
    }
    FlushViewOfFile(field, (uint64_t)n_field * PART_BYTES);
    FlushViewOfFile(resid, resid_sz);
    double bake_ms = now_ms() - t0;

    /* disk reality */
    DWORD hd = 0;
    uint64_t fd_disk = ((uint64_t)GetCompressedFileSizeA("build\\port_field.geo", &hd) << 0);
    uint64_t rd_disk = ((uint64_t)GetCompressedFileSizeA("build\\port_residual.geo", &hd) << 0);
    printf("BAKE   %.0f ms · in-field %u · residual %u · bonds %u (%u B)\n",
           bake_ms, n_field, n_resid, n_bonds, n_bonds * (uint32_t)sizeof(Bond));
    printf("DISK   field on-disk %.2f GB · residual on-disk %.2f GB\n\n",
           (double)fd_disk / 1e9, (double)rd_disk / 1e9);

    /* ── VERIFY every part via its tier ──────────────────────────────── */
    int failures = 0;
    t0 = now_ms();
    uint32_t ok_f = 0, ok_r = 0, badm = 0;
    for (uint32_t fid = 0, i = 0; i < r.n_tensors; i++) {
        const uint8_t *src = r.base + r.data_offset + r.offsets[i];
        uint32_t np = (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
        for (uint32_t p = 0; p < np; p++, fid++) {
            uint32_t off = p * PART_BYTES;
            uint32_t len = r.sizes[i] - off; if (len > PART_BYTES) len = PART_BYTES;
            const uint8_t *got;
            if (fid < n_field) got = field + part_offset(fid);
            else               got = resid + bonds[fid - n_field].spill_off;
            if (memcmp(got, src + off, len) != 0) badm++;
            else if (fid < n_field) ok_f++; else ok_r++;
        }
    }
    double vf = now_ms() - t0;
    printf("VERIFY in-field %u/%u · residual %u/%u · bad %u · %.0f ms · %s\n",
           ok_f, n_field, ok_r, n_resid, badm, vf,
           badm ? "LOSSLESS BROKEN" : "lossless BOTH TIERS");
    failures += badm ? 1 : 0;

    printf("\nRESULT: %s · old behaviour was 'FAIL exceeds window'\n",
           failures ? "FAILED" :
           "NEGATIVE PORT LOSSLESS — model larger than window now serves");

    UnmapViewOfFile(field); CloseHandle(mf); CloseHandle(hf);
    UnmapViewOfFile(resid); CloseHandle(mr); CloseHandle(hr);
    free(bonds);
    gguf_close(&r);
    return failures ? 1 : 0;
}
