/* ═══════════════════════════════════════════════════════════════════════════
 * geo_zerocopy.h — Zero-Copy GCube Access via Memory Mapping
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Maps a .gcube file into memory and returns a GCubeContainer whose `blocks`
 * pointer is the mapped region itself — no fread, no malloc for block data.
 *
 * FILE LAYOUT (on disk):
 *   [FileHdr 64B] [TensorEntry × N] [Blocks total×64B] [CRC32 4B]
 *
 * ZERO-COPY PATH:
 *   mmap(file) → pointer into mapped region → return to caller
 *   OS pages in on demand — no fread, no copy, no allocation.
 *
 * PLATFORM:
 *   Windows: CreateFileMappingA + MapViewOfFile
 *   Unix:    mmap + open/close
 *
 * DEPENDS:
 *   geo_cube_container.h (for GCubeContainer, header structs, magic)
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_ZEROCOPY_H
#define GEO_ZEROCOPY_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geo_cube_container.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <unistd.h>
#endif

/* ═══════════════════════════════════════════════════════════
   ZERO-COPY CONTEXT — holds mmap state for cleanup
   ═══════════════════════════════════════════════════════════ */
typedef struct {
#ifdef _WIN32
    HANDLE  hFile;          /* file handle                        */
    HANDLE  hMapping;       /* file mapping object                 */
#else
    int     fd;             /* file descriptor                     */
    size_t  file_size;      /* for munmap size                     */
#endif
    uint8_t *base;          /* base of mapped region               */
    size_t    mapped_size;   /* total bytes mapped                 */
    GCubeContainer cube;    /* container with blocks = mapped ptr  */
    int       is_open;       /* 1 = mapping active                 */
} GeoZeroCopy;

/* Forward declarations */
static inline void geo_zerocopy_close(GeoZeroCopy *z);

/* ═══════════════════════════════════════════════════════════
   ZEROCOPY OPEN — mmap .gcube, populate container
   ═══════════════════════════════════════════════════════════ */

static inline int geo_zerocopy_open(GeoZeroCopy *z, const char *path) {
    if (!z || !path) return -1;
    memset(z, 0, sizeof(*z));

    /* ── Get file size ──────────────────────────────────── */
    FILE *f = fopen(path, "rb");
    if (!f) return -2;
    fseeko(f, 0, SEEK_END);
    long file_sz = ftello(f);
    fclose(f);
    if (file_sz <= 0) return -2;

    size_t total = (size_t)file_sz;

    /* ── Mmap entire file ───────────────────────────────── */
#ifdef _WIN32
    z->hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (z->hFile == INVALID_HANDLE_VALUE) return -3;

    z->hMapping = CreateFileMappingA(z->hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!z->hMapping) { CloseHandle(z->hFile); return -3; }

    z->base = (uint8_t *)MapViewOfFile(z->hMapping, FILE_MAP_READ, 0, 0, 0);
    if (!z->base) {
        CloseHandle(z->hMapping);
        CloseHandle(z->hFile);
        return -3;
    }
#else
    z->fd = open(path, O_RDONLY);
    if (z->fd < 0) return -3;

    z->base = (uint8_t *)mmap(NULL, total, PROT_READ, MAP_PRIVATE, z->fd, 0);
    if (z->base == MAP_FAILED) { z->base = NULL; close(z->fd); return -3; }
    z->file_size = total;
#endif

    z->mapped_size = total;
    z->is_open = 1;

    /* ── Parse header from mapped memory ────────────────── */
    uint8_t *p = z->base;

    /* File header: magic (4B) + version (4B) + n_tensors (4B) + ... */
    if (memcmp(p, GCUBE_MAGIC, 4) != 0) {
        geo_zerocopy_close(z);
        return -4;  /* bad magic */
    }

    GCubeContainer *cube = &z->cube;
    memset(cube, 0, sizeof(*cube));

    /* Copy header (first GCUBE_FILE_HDR_SZ bytes) */
    memcpy(&cube->header, p, GCUBE_FILE_HDR_SZ);
    p += GCUBE_FILE_HDR_SZ;

    /* Parse tensor entries */
    uint32_t n_tensors = cube->header.n_tensors;
    if (n_tensors > GCUBE_MAX_TENSORS) n_tensors = GCUBE_MAX_TENSORS;
    cube->header.n_tensors = n_tensors;

    for (uint32_t i = 0; i < n_tensors; i++) {
        memcpy(&cube->tensors[i], p, GCUBE_TENSOR_HDR_SZ);
        p += GCUBE_TENSOR_HDR_SZ;
    }

    /* ── blocks = pointer into mapped memory (zero-copy!) ── */
    cube->blocks = p;  /* NO malloc — points into mmap'd region */

    printf("[ZC] mapped %s: %u bytes, %u tensors, %u blocks\n",
           path, (unsigned)total, n_tensors, cube->header.total_blocks);

    return 0;
}

/* ═══════════════════════════════════════════════════════════
   ZEROCOPY CLOSE — unmap + release
   ═══════════════════════════════════════════════════════════ */

static inline void geo_zerocopy_close(GeoZeroCopy *z) {
    if (!z || !z->is_open) return;

    /* DO NOT free cube->blocks — it's the mmap'd region */
    z->cube.blocks = NULL;

#ifdef _WIN32
    if (z->base) UnmapViewOfFile(z->base);
    if (z->hMapping) CloseHandle(z->hMapping);
    if (z->hFile != INVALID_HANDLE_VALUE) CloseHandle(z->hFile);
#else
    if (z->base) munmap(z->base, z->file_size);
    if (z->fd >= 0) close(z->fd);
#endif

    memset(z, 0, sizeof(*z));
}

/* ═══════════════════════════════════════════════════════════
   ZEROCOPY LOAD — like geo_hub_load but zero-copy
   Returns pointer directly into mmap'd region (no malloc, no copy)
   ═══════════════════════════════════════════════════════════ */

static inline int geo_zerocopy_load(GeoZeroCopy *z,
                                     const char *tensor_name,
                                     uint8_t **data_out,
                                     uint32_t *n_elems_out,
                                     uint32_t *dtype_out)
{
    if (!z || !z->is_open || !tensor_name ||
        !data_out || !n_elems_out || !dtype_out) {
        return -1;
    }

    /* Find tensor in container */
    const GCubeTensorEntry *ge = gcube_find(&z->cube, tensor_name);
    if (!ge) {
        *data_out = NULL;
        *n_elems_out = 0;
        *dtype_out = 0;
        return -2;
    }

    /* Zero-copy: pointer into mapped region, no malloc */
    *data_out    = z->cube.blocks + ge->block_start * GCUBE_BLOCK_SZ;
    *n_elems_out = ge->n_elems;
    *dtype_out   = ge->dtype;

    return 0;
}

#endif /* GEO_ZEROCOPY_H */
