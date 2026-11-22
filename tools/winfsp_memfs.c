#define FUSE_USE_VERSION 26
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fuse.h"

static FILE *dbg;
#define LOG(fmt,...) do{ if(dbg){ fprintf(dbg,fmt,##__VA_ARGS__); fflush(dbg);} }while(0)

#define MAXFILES 64
#define MAXNAME 256
#define MAXDATA (256*1024*1024)

typedef struct {
    char name[MAXNAME];
    int used;
    size_t size;
    size_t cap;
    char *data;
    int isdir;
} MemFile;

static MemFile files[MAXFILES];
static int nfiles = 0;

static MemFile* find(const char *path) {
    if (strcmp(path, "/") == 0) return NULL;
    const char *name = path + 1;
    for (int i = 0; i < MAXFILES; i++)
        if (files[i].used && strcmp(files[i].name, name) == 0) return &files[i];
    return NULL;
}

static int memfs_getattr(const char *path, struct fuse_stat *st) {
    LOG("getattr %s\n", path);
    memset(st, 0, sizeof(*st));
    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0777;
        st->st_nlink = 2;
        return 0;
    }
    MemFile *f = find(path);
    if (!f) return -ENOENT;
    if (f->isdir) {
        st->st_mode = S_IFDIR | 0777;
        st->st_nlink = 2;
    } else {
        st->st_mode = S_IFREG | 0666;
        st->st_nlink = 1;
        st->st_size = f->size;
    }
    return 0;
}

static int memfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, fuse_off_t off, struct fuse_file_info *fi) {
    LOG("readdir %s\n", path);
    (void)off; (void)fi;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    for (int i = 0; i < MAXFILES; i++)
        if (files[i].used) filler(buf, files[i].name, NULL, 0);
    return 0;
}

static int memfs_open(const char *path, struct fuse_file_info *fi) {
    LOG("open %s flags=%d\n", path, fi->flags);
    MemFile *f = find(path);
    if (!f) return -ENOENT;
    fi->fh = (unsigned long long)(size_t)f;
    return 0;
}

static int memfs_read(const char *path, char *buf, size_t size, fuse_off_t off, struct fuse_file_info *fi) {
    (void)path;
    MemFile *f = (MemFile*)(size_t)fi->fh;
    if (off >= (fuse_off_t)f->size) return 0;
    if (off + (fuse_off_t)size > (fuse_off_t)f->size) size = f->size - off;
    memcpy(buf, f->data + off, size);
    return (int)size;
}

static int memfs_write(const char *path, const char *buf, size_t size, fuse_off_t off, struct fuse_file_info *fi) {
    (void)path;
    MemFile *f = (MemFile*)(size_t)fi->fh;
    size_t need = off + size;
    if (need > f->cap) {
        size_t nc = f->cap ? f->cap * 2 : 65536;
        while (nc < need) nc *= 2;
        if (nc > MAXDATA) nc = MAXDATA;
        char *nd = realloc(f->data, nc);
        if (!nd) return -ENOMEM;
        f->data = nd; f->cap = nc;
    }
    memcpy(f->data + off, buf, size);
    if (need > f->size) f->size = need;
    return (int)size;
}

static int memfs_create(const char *path, fuse_mode_t mode, struct fuse_file_info *fi) {
    LOG("create %s mode=%o\n", path, mode);
    (void)mode;
    if (nfiles >= MAXFILES) return -ENOSPC;
    const char *name = path + 1;
    for (int i = 0; i < MAXFILES; i++)
        if (files[i].used && strcmp(files[i].name, name) == 0) return -EEXIST;
    for (int i = 0; i < MAXFILES; i++) {
        if (!files[i].used) {
            memset(&files[i], 0, sizeof(files[i]));
            strcpy(files[i].name, name);
            files[i].used = 1;
            files[i].data = NULL;
            files[i].cap = 0;
            files[i].size = 0;
            nfiles++;
            fi->fh = (unsigned long long)(size_t)&files[i];
            return 0;
        }
    }
    return -ENOSPC;
}

static int memfs_unlink(const char *path) {
    LOG("unlink %s\n", path);
    MemFile *f = find(path);
    if (!f) return -ENOENT;
    free(f->data);
    f->used = 0; f->data = NULL; f->size = 0; f->cap = 0;
    nfiles--;
    return 0;
}

static int memfs_truncate(const char *path, fuse_off_t size) {
    LOG("truncate %s size=%lld\n", path, (long long)size);
    MemFile *f = find(path);
    if (!f) return -ENOENT;
    if ((size_t)size > f->cap) {
        char *nd = realloc(f->data, size);
        if (!nd && size > 0) return -ENOMEM;
        f->data = nd; f->cap = size;
    }
    if ((size_t)size > f->size)
        memset(f->data + f->size, 0, size - f->size);
    f->size = size;
    return 0;
}

static int memfs_flush(const char *path, struct fuse_file_info *fi) { LOG("flush %s\n", path); (void)fi; return 0; }
static int memfs_fsync(const char *path, int isdatasync, struct fuse_file_info *fi) { LOG("fsync %s\n", path); (void)isdatasync; (void)fi; return 0; }

static int memfs_mkdir(const char *path, fuse_mode_t mode) { (void)path; (void)mode; return 0; }
static int memfs_rmdir(const char *path) { (void)path; return 0; }
static int memfs_rename(const char *from, const char *to) {
    MemFile *f = find(from);
    if (!f) return -ENOENT;
    const char *n = to + 1;
    strcpy(f->name, n);
    return 0;
}

static int memfs_chmod(const char *path, fuse_mode_t mode) { (void)path; (void)mode; return 0; }
static int memfs_chown(const char *path, fuse_uid_t uid, fuse_gid_t gid) { (void)path; (void)uid; (void)gid; return 0; }
static int memfs_utimens(const char *path, const struct fuse_timespec tv[2]) { (void)path; (void)tv; return 0; }

static struct fuse_operations memfs_ops = {
    .getattr = memfs_getattr,
    .readdir = memfs_readdir,
    .open = memfs_open,
    .read = memfs_read,
    .write = memfs_write,
    .create = memfs_create,
    .unlink = memfs_unlink,
    .truncate = memfs_truncate,
    .flush = memfs_flush,
    .fsync = memfs_fsync,
    .mkdir = memfs_mkdir,
    .rmdir = memfs_rmdir,
    .rename = memfs_rename,
    .chmod = memfs_chmod,
    .chown = memfs_chown,
    .utimens = memfs_utimens,
};

int main(int argc, char *argv[]) {
    dbg = fopen("C:\\winfsp_memfs.log", "w");
    LOG("starting\n");
    int rc = fuse_main(argc, argv, &memfs_ops, NULL);
    LOG("exit rc=%d\n", rc);
    if (dbg) fclose(dbg);
    return rc;
}