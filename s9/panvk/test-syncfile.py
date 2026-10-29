#!/usr/bin/env python3
"""Exercise the generated adapter's actual sync-file functions on the host."""
import pathlib, subprocess, sys, tempfile
source = pathlib.Path(sys.argv[1]).read_text()
start = source.index('int\ndrmSyncobjExportSyncFile(')
end = source.index('int\ndrmSyncobjHandleToFD(', start)
io_start = source.index('int\ndrmIoctl(')
io_end = source.index('int\ndrmCommandWrite(', io_start)
preamble = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <xf86drm.h>
#include "drm-uapi/drm.h"
#include "drm-uapi/panfrost_drm.h"
#include <stdarg.h>
#define FD_SYNCFILE 1
struct ssync { uint32_t seq; int signaled; };
struct sfd { uint32_t value; };
static int gem_close(uint32_t h) { (void)h; return -ENOTSUP; }
static int command(unsigned long i, void *p) { (void)i; (void)p; return -ENOTSUP; }
static void sweet_drm_log(const char *fmt, ...) { (void)fmt; }
int drmSyncobjReset(int fd, const uint32_t *h, uint32_t n) { (void)fd; return n == 1 && *h == 1 ? 0 : -EINVAL; }
static struct ssync obj;
static struct sfd imported = {42};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct ssync *ssync(uint32_t h) { return h == 1 ? &obj : 0; }
static struct sfd *sfd(int fd, int kind) { (void)kind; return fd == 12 ? &imported : 0; }
static int new_fd(int kind, uint32_t seq) { (void)kind; return 100 + seq; }
'''
tests = r'''
int main(void) {
 int fd = -99;
 assert(drmSyncobjExportSyncFile(0, 1, &fd) == -EINVAL && fd == -99);
 assert(drmSyncobjImportSyncFile(0, 1, 1234) == -EINVAL && !obj.signaled);
 assert(drmSyncobjImportSyncFile(0, 1, -1) == 0 && obj.signaled);
 assert(drmSyncobjExportSyncFile(0, 1, &fd) == 0 && fd == 100);
 assert(drmSyncobjImportSyncFile(0, 1, 12) == 0 && obj.seq == 42 && !obj.signaled);
 assert(drmSyncobjImportSyncFile(0, 1, 1234) == -EINVAL && obj.seq == 42);
 assert(drmSyncobjExportSyncFile(0, 1, &fd) == 0 && fd == 142);
 assert(drmSyncobjImportSyncFile(0, 9, -1) == -EINVAL);
 assert(drmSyncobjExportSyncFile(0, 9, &fd) == -EINVAL);
 struct drm_syncobj_handle h = { .handle = 1, .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE, .fd = -1 };
 assert(drmIoctl(0, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &h) == 0 && h.fd == 142);
 h.fd = 12; h.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
 assert(drmIoctl(0, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &h) == 0 && obj.seq == 42);
 h.fd = 1234;
 assert(drmIoctl(0, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &h) == -1 && errno == EINVAL);
 h.flags = 0;
 assert(drmIoctl(0, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &h) == -1 && errno == ENOTSUP);
 uint32_t handle = 1;
 struct drm_syncobj_array a = { .handles = (uintptr_t)&handle, .count_handles = 1 };
 assert(drmIoctl(0, DRM_IOCTL_SYNCOBJ_RESET, &a) == 0);
 a.pad = 1;
 assert(drmIoctl(0, DRM_IOCTL_SYNCOBJ_RESET, &a) == -1 && errno == EINVAL);
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c = pathlib.Path(tmp)/'test.c'; exe = pathlib.Path(tmp)/'test'
 c.write_text(preamble + source[start:end] + source[io_start:io_end] + tests)
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-pthread','-I'+str(pathlib.Path(sys.argv[1]).parent),'-I'+str(pathlib.Path(sys.argv[1]).parents[2]/'include'),str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: empty fence export, invalid fd import, completed sentinel, pending fence import/export, invalid handle')
