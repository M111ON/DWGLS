/* winfsp_vram.c — WinFsp FUSE filesystem backed by GPU VRAM (Saturn-style).
 * Mounts GPU #1 VRAM as a drive letter (e.g., V:).
 * Read/write goes through cudaMemcpy (H2D/D2H).
 *
 * Build: gcc -O2 -I "C:/Program Files (x86)/WinFsp/inc" -I "C:/Program Files (x86)/WinFsp/inc/fuse" \
 *        -I "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.0/include" \
 *        -o build/winfsp_vram tools/winfsp_vram.c \
 *        "C:/Program Files (x86)/WinFsp/bin/winfsp-x64.dll" \
 *        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.0/lib/x64/cudart.lib"
 *
 * Usage: winfsp_vram.exe V: [size_mb]
 */
#define FUSE_USE_VERSION 26
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fuse.h"
#include <cuda_runtime.h>

static FILE *dbg;
#define LOG(fmt,...) do{ if(dbg){ fprintf(dbg,fmt,##__VA_ARGS__); fflush(dbg);} }while(0)

#define MAXFILES 64
#define MAXNAME 256
#define MAXDATA (256*1024*1024)  /* 256 MB per file */
#define VRAM_SIZE_MB 512           /* default VRAM allocation */

typedef struct {
    char name[MAXNAME];
    int used;
    size_t size;
    size_t cap;
    void *d_data;       /* GPU VRAM pointer */
    int isdir;
} VramFile;

static VramFile files[MAXFILES];
static int nfiles = 0;
static int gpu_device = 1;  /* use GPU #1 */
static size_t vram_total = 0;
static void *vram_pool = NULL;
static size_t vram_used = 0;

static VramFile* find(const char *path) {
    if (strcmp(path, "/") == 0) return NULL;
    const char *name = path + 1;
    for (int i = 0; i < MAXFILES; i++)
        if (files[i].used && strcmp(files[i].name, name) == 0) return &files[i];
    return NULL;
}

static int vramfs_getattr(const char *path, struct fuse_stat *st) {
    LOG("getattr %s\n", path);
    memset(st, 0, sizeof(*st));
    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0777;
        st->st_nlink = 2;
        return 0;
    }
    VramFile *f = find(path);
    if (!f) return -ENOENT;
    st->st_mode = f->isdir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_nlink = 1;
    st->st_size = (fuse_off_t)f->size;
    return 0;
}

static int vramfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, fuse_off_t off, struct fuse_file_info *fi) {
    LOG("readdir %s\n", path);
    (void)off; (void)fi;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    for (int i = 0; i < MAXFILES; i++)
        if (files[i].used) filler(buf, files[i].name, NULL, 0);
    return 0;
}

static int vramfs_open(const char *path, struct fuse_file_info *fi) {
    LOG("open %s\n", path);
    VramFile *f = find(path);
    if (!f) return -ENOENT;
    return 0;
}

static int vramfs_read(const char *path, char *buf, size_t size, fuse_off_t off, struct fuse_file_info *fi) {
    LOG("read %s size=%zu off=%ld\n", path, size, (long)off);
    VramFile *f = find(path);
    if (!f) return -ENOENT;
    if ((size_t)off >= f->size) return 0;
    size_t avail = f->size - (size_t)off;
    if (size > avail) size = avail;
    /* D2H: GPU VRAM → CPU buffer */
    cudaSetDevice(gpu_device);
    cudaMemcpy(buf, (char *)f->d_data + off, size, cudaMemcpyDeviceToHost);
    return (int)size;
}

static int vramfs_write(const char *path, const char *buf, size_t size, fuse_off_t off, struct fuse_file_info *fi) {
    LOG("write %s size=%zu off=%ld\n", path, size, (long)off);
    VramFile *f = find(path);
    if (!f) return -ENOENT;
    size_t end = (size_t)off + size;
    if (end > f->cap) {
        /* grow: allocate more VRAM */
        size_t newcap = f->cap ? f->cap * 2 : 4096;
        while (newcap < end) newcap *= 2;
        if (newcap > MAXDATA) return -ENOSPC;
        void *newdata;
        cudaSetDevice(gpu_device);
        cudaMalloc(&newdata, newcap);
        if (f->d_data) {
            cudaMemcpy(newdata, f->d_data, f->size, cudaMemcpyDeviceToDevice);
            cudaFree(f->d_data);
        }
        f->d_data = newdata;
        f->cap = newcap;
    }
    /* H2D: CPU buffer → GPU VRAM */
    cudaSetDevice(gpu_device);
    cudaMemcpy((char *)f->d_data + off, buf, size, cudaMemcpyHostToDevice);
    if (end > f->size) f->size = end;
    return (int)size;
}

static int vramfs_create(const char *path, fuse_mode_t mode, struct fuse_file_info *fi) {
    LOG("create %s\n", path);
    (void)mode; (void)fi;
    if (find(path)) return -EEXIST;
    for (int i = 0; i < MAXFILES; i++) {
        if (!files[i].used) {
            memset(&files[i], 0, sizeof(files[i]));
            const char *name = path + 1;
            strncpy(files[i].name, name, MAXNAME - 1);
            files[i].used = 1;
            files[i].isdir = 0;
            files[i].size = 0;
            files[i].cap = 0;
            files[i].d_data = NULL;
            nfiles++;
            return 0;
        }
    }
    return -ENOSPC;
}

static int vramfs_unlink(const char *path) {
    LOG("unlink %s\n", path);
    VramFile *f = find(path);
    if (!f) return -ENOENT;
    if (f->d_data) { cudaSetDevice(gpu_device); cudaFree(f->d_data); }
    f->used = 0;
    nfiles--;
    return 0;
}

static int vramfs_truncate(const char *path, fuse_off_t size) {
    LOG("truncate %s %ld\n", path, (long)size);
    VramFile *f = find(path);
    if (!f) return -ENOENT;
    if ((size_t)size > f->cap) {
        size_t newcap = (size_t)size;
        if (newcap < 4096) newcap = 4096;
        void *newdata;
        cudaSetDevice(gpu_device);
        cudaMalloc(&newdata, newcap);
        if (f->d_data) {
            cudaMemcpy(newdata, f->d_data, f->size, cudaMemcpyDeviceToDevice);
            cudaFree(f->d_data);
        }
        f->d_data = newdata;
        f->cap = newcap;
    }
    f->size = (size_t)size;
    return 0;
}

static int vramfs_flush(const char *path, struct fuse_file_info *fi) { (void)path; (void)fi; return 0; }
static int vramfs_fsync(const char *path, int isdatasync, struct fuse_file_info *fi) { (void)path; (void)isdatasync; (void)fi; return 0; }
static int vramfs_mkdir(const char *path, fuse_mode_t mode) { (void)path; (void)mode; return 0; }
static int vramfs_rmdir(const char *path) { (void)path; return 0; }
static int vramfs_rename(const char *from, const char *to) {
    VramFile *f = find(from);
    if (!f) return -ENOENT;
    if (find(to)) return -EEXIST;
    strncpy(f->name, to + 1, MAXNAME - 1);
    return 0;
}
static int vramfs_chmod(const char *path, fuse_mode_t mode) { (void)path; (void)mode; return 0; }
static int vramfs_chown(const char *path, fuse_uid_t uid, fuse_gid_t gid) { (void)path; (void)uid; (void)gid; return 0; }
static int vramfs_utimens(const char *path, const struct fuse_timespec tv[2]) { (void)path; (void)tv; return 0; }

static struct fuse_operations vramfs_ops = {
    .getattr = vramfs_getattr,
    .readdir = vramfs_readdir,
    .open = vramfs_open,
    .read = vramfs_read,
    .write = vramfs_write,
    .create = vramfs_create,
    .unlink = vramfs_unlink,
    .truncate = vramfs_truncate,
    .flush = vramfs_flush,
    .fsync = vramfs_fsync,
    .mkdir = vramfs_mkdir,
    .rmdir = vramfs_rmdir,
    .rename = vramfs_rename,
    .chmod = vramfs_chmod,
    .chown = vramfs_chown,
    .utimens = vramfs_utimens,
};

int main(int argc, char **argv) {
    dbg = fopen("C:\\\\winfsp_vram.log", "w");
    LOG("=== winfsp_vram starting ===\n");

    /* Use GPU #1 */
    int ndev;
    cudaGetDeviceCount(&ndev);
    LOG("CUDA devices: %d\n", ndev);
    if (ndev < 2) { gpu_device = 0; LOG("Only 1 GPU, using device 0\n"); }
    else { gpu_device = 1; LOG("Using GPU #1\n"); }
    cudaSetDevice(gpu_device);

    struct cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, gpu_device);
    LOG("GPU %d: %s, %.0f MB VRAM\n", gpu_device, prop.name, (double)prop.totalGlobalMem / 1e6);

    /* Pre-allocate VRAM pool */
    size_t pool_sz = (size_t)VRAM_SIZE_MB * 1024 * 1024;
    cudaError_t err = cudaMalloc(&vram_pool, pool_sz);
    if (err != cudaSuccess) {
        LOG("cudaMalloc failed: %s\n", cudaGetErrorString(err));
        /* Try smaller */
        pool_sz = 64 * 1024 * 1024;
        err = cudaMalloc(&vram_pool, pool_sz);
        if (err != cudaSuccess) {
            LOG("cudaMalloc 64MB failed: %s\n", cudaGetErrorString(err));
            return 1;
        }
    }
    vram_total = pool_sz;
    LOG("VRAM pool: %.0f MB allocated\n", vram_total / 1e6);

    cudaMemset(vram_pool, 0, vram_total);
    vram_used = 0;

    /* Default mount point */
    if (argc < 2) {
        argv[1] = "V:";
        argc = 2;
    }

    LOG("Mounting on %s\n", argv[1]);
    int rc = fuse_main(argc, argv, &vramfs_ops, NULL);
    LOG("fuse_main returned %d\n", rc);

    /* Cleanup */
    cudaFree(vram_pool);
    for (int i = 0; i < MAXFILES; i++)
        if (files[i].used && files[i].d_data) cudaFree(files[i].d_data);
    if (dbg) fclose(dbg);
    return rc;
}
