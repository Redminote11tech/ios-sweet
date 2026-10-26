/* s9/sweetgl: panfrost DRM emulation (sweet_drm.c) and its log. */
#ifndef SWEET_DRM_H
#define SWEET_DRM_H

#include <stdint.h>

/* A "DRM fd" for the panfrost screen (dup of /dev/null), or -1 without SweetGPU. */
int sweet_drm_open(void);

/* Our mapping of the bo behind a panfrost mmap offset (MAP_FAILED if none). */
void *sweet_drm_bo_map(int fd, uint64_t offset);

/* Size of the bo behind a prime fd (-1 if none): Mesa's lseek(SEEK_END). */
int64_t sweet_drm_fd_size(int fd);

/* A prime fd for an IOSurface's memory (imported as a GEM bo): 0 or -errno. */
int sweet_drm_import_iosurface(uint32_t surface_id, uint64_t map, int *prime_fd, uint64_t *size);

/* printf-style line to the kernel log (writes to /dev/null are teed there). */
void sweet_drm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
