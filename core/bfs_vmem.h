/*
 * bfs_vmem.h — reserve / commit / decommit primitives for the BFS payload region
 * ════════════════════════════════════════════════════════════════════
 * The whole point of BFS memory economy: reserve the full address space
 * once (costs 0 physical), commit only the blocks actually written, and
 * decommit on delete/evict so RSS tracks the working set — not
 * sizeof(BreathingFS).
 *
 * POSIX: mmap(PROT_NONE|MAP_NORESERVE) reserve; mprotect RW = commit;
 *        madvise(MADV_DONTNEED)+mprotect(PROT_NONE) = decommit; munmap.
 * WIN32: VirtualAlloc MEM_RESERVE / MEM_COMMIT / VirtualFree MEM_DECOMMIT.
 *
 * Commit/decommit offsets MUST be page-aligned (the caller uses 4096-byte
 * stride so every block owns a whole page).
 * ════════════════════════════════════════════════════════════════════ */
#ifndef BFS_VMEM_H
#define BFS_VMEM_H

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  static inline void *bfs_vmem_reserve(size_t bytes) {
      return VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_NOACCESS);
  }
  static inline int bfs_vmem_commit(void *base, size_t off, size_t bytes) {
      void *p = VirtualAlloc((uint8_t *)base + off, bytes, MEM_COMMIT, PAGE_READWRITE);
      return p ? 0 : -1;
  }
  static inline int bfs_vmem_decommit(void *base, size_t off, size_t bytes) {
      return VirtualFree((uint8_t *)base + off, bytes, MEM_DECOMMIT) ? 0 : -1;
  }
  static inline int bfs_vmem_unmap(void *base, size_t bytes) {
      (void)bytes;
      return VirtualFree(base, 0, MEM_RELEASE) ? 0 : -1;
  }
#else
  #include <sys/mman.h>
  static inline void *bfs_vmem_reserve(size_t bytes) {
      void *p = mmap(NULL, bytes, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS
#ifdef MAP_NORESERVE
                     | MAP_NORESERVE
#endif
                     , -1, 0);
      return (p == MAP_FAILED) ? NULL : p;
  }
  static inline int bfs_vmem_commit(void *base, size_t off, size_t bytes) {
      return mprotect((uint8_t *)base + off, bytes, PROT_READ | PROT_WRITE);
  }
  static inline int bfs_vmem_decommit(void *base, size_t off, size_t bytes) {
      madvise((uint8_t *)base + off, bytes, MADV_DONTNEED);
      return mprotect((uint8_t *)base + off, bytes, PROT_NONE);
  }
  static inline int bfs_vmem_unmap(void *base, size_t bytes) {
      return munmap(base, bytes);
  }
#endif

#endif /* BFS_VMEM_H */