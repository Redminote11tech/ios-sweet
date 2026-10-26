/*
 * libdrm + panfrost DRM emulation for Mesa panfrost on iOS (no DRM here).
 * Structure after joranv1/ios-sailfish's msm emulation (tools/sweetgl).
 *
 * Mesa's panfrost talks to the kernel only through the libdrm calls in
 * include/xf86drm.h, one mmap (pan_kmod.h, hooked by mesa.patch to
 * sweet_drm_bo_map) and lseek on dma-buf fds (hooked to sweet_drm_fd_size).
 * This file answers them in user space on top of the SweetGPU user client
 * (xnu iokit/Kernel/arm/SweetGPU.cpp, patch 0094):
 *
 *   CREATE_BO            sel 10: wired buffer at a GPU VA, mapped into us
 *   GEM_CLOSE            sel 11
 *   MMAP_BO              handle << 32; sweet_drm_bo_map() turns it back
 *   GET_BO_OFFSET        the GPU VA
 *   SUBMIT               sel 13 (the kernel runs chains one at a time, in
 *                        order, so in_syncs are already satisfied); the bos
 *                        and the out_sync syncobj remember the sequence no.
 *   WAIT_BO              sel 14, then sel 16 (drop CPU cache lines: the
 *                        mapping is cached, the GPU's is not)
 *   GET_PARAM            sel 15 (GPU control registers)
 *   syncobjs, sync files emulated here over sequence numbers
 *   prime fds            /dev/null dups that name a GEM handle
 *
 * One device per process; the "fd" is a /dev/null dup.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <IOKit/IOKitLib.h>

#include "xf86drm.h"
#include "drm-uapi/panfrost_drm.h"
#include "sweet_drm.h"

#define SEL_BO_NEW      10
#define SEL_BO_FREE     11
#define SEL_BO_IMPORT   12
#define SEL_SUBMIT      13
#define SEL_WAIT        14
#define SEL_READ_REG    15
#define SEL_CPU_PREP    16

#define MAX_FDS         4096
#define BO_NOEXEC       2u      /* SweetGPU bo new flags */

struct sbo {
	uint64_t iova, map, size;
	uint32_t seq;           /* last submit listing it */
	int used;
};

struct ssync {
	int live;
	int signaled;           /* signaled without a fence */
	uint32_t seq;           /* fence: submit sequence number, 0 none */
};

enum { FD_NONE, FD_SYNCFILE, FD_SYNCOBJ, FD_DMABUF };
struct sfd {
	int kind;
	uint32_t value;         /* seq, syncobj handle or GEM handle */
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static io_connect_t conn;
static int dev_fd = -1;
static struct sbo *bos;
static uint32_t nbos;
static struct ssync *syncs;
static uint32_t nsyncs;
static struct sfd fds[MAX_FDS];

static uint64_t
reg(uint32_t off)
{
	uint64_t in = off, out = 0;
	uint32_t n = 1;
	return IOConnectCallScalarMethod(conn, SEL_READ_REG, &in, 1, &out, &n) == KERN_SUCCESS ? out : 0;
}

/* Lock held. The GPU comes up a while after boot: callers may retry. */
static void
connect_dev(void)
{
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetGPU"));
	if (svc == 0 || IOServiceOpen(svc, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
		if (svc) {
			IOObjectRelease(svc);
		}
		conn = 0;
		return;
	}
	IOObjectRelease(svc);
	if ((reg(0) >> 16) == 0) {      /* not ready yet: GPU_ID reads 0 */
		IOServiceClose(conn);
		conn = 0;
		return;
	}
	dev_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
}

int
sweet_drm_open(void)
{
	pthread_mutex_lock(&lock);
	if (!conn) {
		connect_dev();
	}
	int fd = conn && dev_fd >= 0 ? dup(dev_fd) : -1;
	pthread_mutex_unlock(&lock);
	return fd;
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid;
	(void)type;
	return name && strcmp(name, "panfrost") == 0 ? sweet_drm_open() : -1;
}

/* ------------------------------------------------------------------ bos */

/* Lock held: the bo for a handle, or NULL. */
static struct sbo *
sbo(uint32_t handle)
{
	return handle >= 1 && handle <= nbos && bos[handle - 1].size ? &bos[handle - 1] : NULL;
}

/* Lock held. */
static int
sbo_add(uint32_t handle, uint64_t iova, uint64_t map, uint64_t size)
{
	if (handle == 0) {
		return -EINVAL;
	}
	if (handle > nbos) {
		uint32_t n = nbos ? nbos : 256;
		while (n < handle) {
			n *= 2;
		}
		struct sbo *b = realloc(bos, n * sizeof(*b));
		if (b == NULL) {
			return -ENOMEM;
		}
		memset(b + nbos, 0, (n - nbos) * sizeof(*b));
		bos = b;
		nbos = n;
	}
	bos[handle - 1] = (struct sbo){ .iova = iova, .map = map, .size = size };
	return 0;
}

static int
fence_passed(uint32_t seq)
{
	uint64_t in[2] = {seq, 0};
	return seq == 0 || IOConnectCallScalarMethod(conn, SEL_WAIT, in, 2, NULL, NULL) == KERN_SUCCESS;
}

/* Wait until seq completed, at most ms milliseconds (UINT64_MAX: forever). */
static int
fence_wait(uint32_t seq, uint64_t ms)
{
	if (fence_passed(seq)) {
		return 0;
	}
	for (;;) {
		uint64_t chunk = ms > 5000 ? 5000 : ms;
		uint64_t in[2] = {seq, chunk};
		kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_WAIT, in, 2, NULL, NULL);
		if (kr == KERN_SUCCESS || fence_passed(seq)) {
			return 0;
		}
		if (kr != kIOReturnTimeout) {
			sweet_drm_log("sweetgl: wait for %u failed: %x", seq, kr);
			return -EIO;
		}
		if (ms != UINT64_MAX) {
			ms -= chunk;
			if (ms == 0) {
				return -ETIMEDOUT;
			}
		}
	}
}

/* DRM timeouts are absolute CLOCK_MONOTONIC nanoseconds; -> ms from now. */
static uint64_t
timeout_ms(int64_t abs_ns)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t now_ns = now.tv_sec * 1000000000ll + now.tv_nsec;
	if (abs_ns == INT64_MAX || abs_ns - now_ns > 3600ll * 1000000000ll) {
		return UINT64_MAX;
	}
	return abs_ns <= now_ns ? 0 : (uint64_t)((abs_ns - now_ns) / 1000000) + 1;
}

/* --------------------------------------------------------------- ioctls */

static int
get_param(struct drm_panfrost_get_param *p)
{
	uint32_t id = (uint32_t)reg(0x000);
	uint32_t i = p->param;
	switch (i) {
	case DRM_PANFROST_PARAM_GPU_PROD_ID:     p->value = id >> 16; return 0;
	case DRM_PANFROST_PARAM_GPU_REVISION:    p->value = id & 0xffff; return 0;
	case DRM_PANFROST_PARAM_SHADER_PRESENT:  p->value = reg(0x100) | reg(0x104) << 32; return 0;
	case DRM_PANFROST_PARAM_TILER_PRESENT:   p->value = reg(0x110) | reg(0x114) << 32; return 0;
	case DRM_PANFROST_PARAM_L2_PRESENT:      p->value = reg(0x120) | reg(0x124) << 32; return 0;
	case DRM_PANFROST_PARAM_STACK_PRESENT:   p->value = 0; return 0;
	case DRM_PANFROST_PARAM_AS_PRESENT:      p->value = reg(0x018); return 0;
	case DRM_PANFROST_PARAM_JS_PRESENT:      p->value = reg(0x01c); return 0;
	case DRM_PANFROST_PARAM_L2_FEATURES:     p->value = reg(0x004); return 0;
	case DRM_PANFROST_PARAM_CORE_FEATURES:   p->value = reg(0x008); return 0;
	case DRM_PANFROST_PARAM_TILER_FEATURES:  p->value = reg(0x00c); return 0;
	case DRM_PANFROST_PARAM_MEM_FEATURES:    p->value = reg(0x010); return 0;
	case DRM_PANFROST_PARAM_MMU_FEATURES:    p->value = reg(0x014); return 0;
	case DRM_PANFROST_PARAM_THREAD_FEATURES: p->value = reg(0x0ac); return 0;
	case DRM_PANFROST_PARAM_MAX_THREADS:     p->value = reg(0x0a0); return 0;
	case DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ: p->value = reg(0x0a4); return 0;
	case DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ:   p->value = reg(0x0a8); return 0;
	case DRM_PANFROST_PARAM_COHERENCY_FEATURES:      p->value = reg(0x300); return 0;
	case DRM_PANFROST_PARAM_NR_CORE_GROUPS:  p->value = 1; return 0;
	case DRM_PANFROST_PARAM_THREAD_TLS_ALLOC: p->value = reg(0x310); return 0;
	case DRM_PANFROST_PARAM_AFBC_FEATURES:   p->value = 0; return 0;
	default:
		if (i >= DRM_PANFROST_PARAM_TEXTURE_FEATURES0 && i <= DRM_PANFROST_PARAM_TEXTURE_FEATURES3) {
			p->value = reg(0x0b0 + 4 * (i - DRM_PANFROST_PARAM_TEXTURE_FEATURES0));
			return 0;
		}
		if (i >= DRM_PANFROST_PARAM_JS_FEATURES0 && i <= DRM_PANFROST_PARAM_JS_FEATURES15) {
			p->value = reg(0x0c0 + 4 * (i - DRM_PANFROST_PARAM_JS_FEATURES0));
			return 0;
		}
		return -EINVAL;
	}
}

static int
create_bo(struct drm_panfrost_create_bo *r)
{
	uint64_t size = r->size;
	if (r->flags & PANFROST_BO_HEAP) {
		/* no alloc-on-fault: back the whole heap (Mesa asks 128 MiB, mesa.patch makes it 32) */
		size = (size + 0x1fffff) & ~0x1fffffull;
	}
	uint64_t in[2] = {size, (r->flags & PANFROST_BO_NOEXEC) ? BO_NOEXEC : 0}, out[3];
	uint32_t n = 3;
	kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_BO_NEW, in, 2, out, &n);
	if (kr != KERN_SUCCESS) {
		sweet_drm_log("sweetgl: bo new %llu failed: %x", size, kr);
		return -ENOMEM;
	}
	pthread_mutex_lock(&lock);
	int ret = sbo_add((uint32_t)out[0], out[1], out[2], (size + 16383) & ~16383ull);
	pthread_mutex_unlock(&lock);
	if (ret) {
		IOConnectCallScalarMethod(conn, SEL_BO_FREE, out, 1, NULL, NULL);
		return ret;
	}
	r->handle = (uint32_t)out[0];
	r->offset = out[1];
	return 0;
}

static int
gem_close(uint32_t handle)
{
	pthread_mutex_lock(&lock);
	struct sbo *b = sbo(handle);
	if (b) {
		memset(b, 0, sizeof(*b));
	}
	pthread_mutex_unlock(&lock);
	uint64_t in = handle;
	return b && IOConnectCallScalarMethod(conn, SEL_BO_FREE, &in, 1, NULL, NULL) == KERN_SUCCESS ? 0 : -EINVAL;
}

static int
wait_bo(struct drm_panfrost_wait_bo *r)
{
	pthread_mutex_lock(&lock);
	struct sbo *b = sbo(r->handle);
	uint32_t seq = b ? b->seq : 0;
	int used = b && b->used;
	pthread_mutex_unlock(&lock);
	if (!b) {
		return -ENOENT;
	}
	if (used && !fence_passed(seq)) {
		uint64_t ms = r->timeout_ns <= 0 ? 0 : timeout_ms(r->timeout_ns);
		if (ms == 0) {
			return -EBUSY;
		}
		int e = fence_wait(seq, ms);
		if (e) {
			return e == -ETIMEDOUT ? -ETIMEDOUT : e;
		}
	}
	/* the bo is CPU-cached: drop stale lines over what the GPU wrote */
	uint64_t in = r->handle;
	return !used || IOConnectCallScalarMethod(conn, SEL_CPU_PREP, &in, 1, NULL, NULL) == KERN_SUCCESS ? 0 : -EIO;
}

/* Lock held. */
static struct ssync *
ssync(uint32_t h)
{
	return h >= 1 && h <= nsyncs && syncs[h - 1].live ? &syncs[h - 1] : NULL;
}

static int
submit(struct drm_panfrost_submit *r)
{
	const uint32_t *in_h = (const uint32_t *)(uintptr_t)r->in_syncs;
	/* chains run in submission order: waiting on our own syncobjs is a no-op;
	 * imported sync files are our own sequence numbers as well */
	for (uint32_t i = 0; i < r->in_sync_count; i++) {
		pthread_mutex_lock(&lock);
		struct ssync *s = ssync(in_h[i]);
		uint32_t seq = s ? s->seq : 0;
		pthread_mutex_unlock(&lock);
		fence_wait(seq, 2000);
	}
	const uint32_t *bh = (const uint32_t *)(uintptr_t)r->bo_handles;
	size_t len = 16 + r->bo_handle_count * 4;
	uint8_t *buf = malloc(len);
	if (buf == NULL) {
		return -ENOMEM;
	}
	*(uint64_t *)buf = r->jc;
	((uint32_t *)buf)[2] = r->requirements & PANFROST_JD_REQ_FS;
	((uint32_t *)buf)[3] = r->bo_handle_count;
	memcpy(buf + 16, bh, r->bo_handle_count * 4);
	uint64_t seq = 0;
	uint32_t n = 1;
	kern_return_t kr = IOConnectCallMethod(conn, SEL_SUBMIT, NULL, 0, buf, len, &seq, &n, NULL, NULL);
	free(buf);
	if (kr != KERN_SUCCESS) {
		sweet_drm_log("sweetgl: submit jc %#llx (%u bos) failed: %x", r->jc, r->bo_handle_count, kr);
		return kr == kIOReturnNotReady ? -ENODEV : -EINVAL;
	}
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < r->bo_handle_count; i++) {
		struct sbo *b = sbo(bh[i]);
		if (b) {
			b->seq = (uint32_t)seq;
			b->used = 1;
		}
	}
	struct ssync *s = r->out_sync ? ssync(r->out_sync) : NULL;
	if (s) {
		s->seq = (uint32_t)seq;
		s->signaled = 0;
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

static int
command(unsigned long index, void *data)
{
	if (!conn) {
		return -ENODEV;
	}
	switch (index) {
	case DRM_PANFROST_GET_PARAM:
		return get_param(data);
	case DRM_PANFROST_CREATE_BO:
		return create_bo(data);
	case DRM_PANFROST_MMAP_BO: {
		struct drm_panfrost_mmap_bo *r = data;
		pthread_mutex_lock(&lock);
		int ok = sbo(r->handle) != NULL;
		pthread_mutex_unlock(&lock);
		r->offset = (uint64_t)r->handle << 32;
		return ok ? 0 : -ENOENT;
	}
	case DRM_PANFROST_GET_BO_OFFSET: {
		struct drm_panfrost_get_bo_offset *r = data;
		pthread_mutex_lock(&lock);
		struct sbo *b = sbo(r->handle);
		r->offset = b ? b->iova : 0;
		pthread_mutex_unlock(&lock);
		return b ? 0 : -ENOENT;
	}
	case DRM_PANFROST_SUBMIT:
		return submit(data);
	case DRM_PANFROST_WAIT_BO:
		return wait_bo(data);
	case DRM_PANFROST_MADVISE:
		((struct drm_panfrost_madvise *)data)->retained = 1;
		return 0;
	default:
		sweet_drm_log("sweetgl: unhandled panfrost ioctl %lu", index);
		return -EINVAL;
	}
}

/* libdrm: 0, or -1 with errno set */
int
drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	int ret;
	if (request == DRM_IOCTL_GEM_CLOSE) {
		ret = gem_close(((struct drm_gem_close *)arg)->handle);
	} else if (((request >> 8) & 0xff) == DRM_IOCTL_BASE && (request & 0xff) >= DRM_COMMAND_BASE &&
	    (request & 0xff) < DRM_COMMAND_END) {
		ret = command((request & 0xff) - DRM_COMMAND_BASE, arg);
	} else {
		sweet_drm_log("sweetgl: unhandled drm ioctl %lx", request);
		ret = -EINVAL;
	}
	if (ret) {
		errno = -ret;
		return -1;
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	(void)size;
	int ret = command(index, data);
	if (ret) {
		errno = -ret;
	}
	return ret;
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	return drmCommandWrite(fd, index, data, size);
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	(void)fd;
	int ret = gem_close(handle);
	if (ret) {
		errno = -ret;
		return -1;
	}
	return 0;
}

void *
sweet_drm_bo_map(int fd, uint64_t offset)
{
	(void)fd;
	pthread_mutex_lock(&lock);
	struct sbo *b = sbo((uint32_t)(offset >> 32));
	uint64_t off = offset & 0xffffffffull;
	void *p = b && b->map && off < b->size ? (void *)(uintptr_t)(b->map + off) : (void *)-1;
	pthread_mutex_unlock(&lock);
	return p;
}

/* ---------------------------------------------------------- fds, prime */

static int
new_fd(int kind, uint32_t value)
{
	int fd = dev_fd >= 0 ? dup(dev_fd) : -1;
	if (fd < 0 || fd >= MAX_FDS) {
		if (fd >= 0) {
			close(fd);
		}
		errno = EMFILE;
		return -1;
	}
	pthread_mutex_lock(&lock);
	fds[fd] = (struct sfd){ .kind = kind, .value = value };
	pthread_mutex_unlock(&lock);
	return fd;
}

/* Lock held. */
static struct sfd *
sfd(int fd, int kind)
{
	return fd >= 0 && fd < MAX_FDS && fds[fd].kind == kind ? &fds[fd] : NULL;
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd;
	(void)flags;
	pthread_mutex_lock(&lock);
	int ok = sbo(handle) != NULL;
	pthread_mutex_unlock(&lock);
	if (!ok) {
		errno = ENOENT;
		return -1;
	}
	*prime_fd = new_fd(FD_DMABUF, handle);
	return *prime_fd < 0 ? -1 : 0;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	(void)fd;
	pthread_mutex_lock(&lock);
	struct sfd *f = sfd(prime_fd, FD_DMABUF);
	uint32_t h = f ? f->value : 0;
	pthread_mutex_unlock(&lock);
	if (!h) {
		errno = EINVAL;
		return -1;
	}
	*handle = h;
	return 0;
}

/* dma-buf size (lseek(fd, 0, SEEK_END) in pan_kmod.c, hooked by mesa.patch) */
int64_t
sweet_drm_fd_size(int fd)
{
	pthread_mutex_lock(&lock);
	struct sfd *f = sfd(fd, FD_DMABUF);
	struct sbo *b = f ? sbo(f->value) : NULL;
	int64_t size = b ? (int64_t)b->size : -1;
	pthread_mutex_unlock(&lock);
	return size;
}

int
sweet_drm_import_iosurface(uint32_t surface_id, uint64_t map, int *prime_fd, uint64_t *size)
{
	if (!conn) {
		return -ENODEV;
	}
	uint64_t in = surface_id, out[3];
	uint32_t n = 3;
	if (IOConnectCallScalarMethod(conn, SEL_BO_IMPORT, &in, 1, out, &n) != KERN_SUCCESS) {
		return -ENOENT;
	}
	pthread_mutex_lock(&lock);
	int ret = sbo_add((uint32_t)out[0], out[1], map, out[2]);
	pthread_mutex_unlock(&lock);
	if (ret) {
		IOConnectCallScalarMethod(conn, SEL_BO_FREE, out, 1, NULL, NULL);
		return ret;
	}
	*prime_fd = new_fd(FD_DMABUF, (uint32_t)out[0]);
	*size = out[2];
	return *prime_fd < 0 ? -EMFILE : 0;
}

/* -------------------------------------------------------------- syncobjs */

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	pthread_mutex_lock(&lock);
	uint32_t h = 0;
	for (uint32_t i = 0; i < nsyncs && !h; i++) {
		if (!syncs[i].live) {
			h = i + 1;
		}
	}
	if (!h) {
		uint32_t n = nsyncs ? nsyncs * 2 : 64;
		struct ssync *s = realloc(syncs, n * sizeof(*s));
		if (s == NULL) {
			pthread_mutex_unlock(&lock);
			errno = ENOMEM;
			return -1;
		}
		memset(s + nsyncs, 0, (n - nsyncs) * sizeof(*s));
		syncs = s;
		h = nsyncs + 1;
		nsyncs = n;
	}
	syncs[h - 1] = (struct ssync){ .live = 1, .signaled = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0 };
	pthread_mutex_unlock(&lock);
	*handle = h;
	return 0;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	pthread_mutex_lock(&lock);
	struct ssync *s = ssync(handle);
	if (s) {
		s->live = 0;
	}
	pthread_mutex_unlock(&lock);
	return s ? 0 : -EINVAL;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < count; i++) {
		struct ssync *s = ssync(handles[i]);
		if (s) {
			s->seq = 0;
			s->signaled = 0;
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
	(void)fd;
	uint64_t ms = timeout_ms(timeout_nsec);
	for (unsigned i = 0; i < count; i++) {
		pthread_mutex_lock(&lock);
		struct ssync *s = ssync(handles[i]);
		int live = s != NULL, signaled = s && s->signaled;
		uint32_t seq = s ? s->seq : 0;
		pthread_mutex_unlock(&lock);
		if (!live) {
			errno = EINVAL;
			return -EINVAL;
		}
		if (seq == 0 && !signaled) {
			if (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) {
				/* nothing will be submitted while we block this thread */
				errno = ETIME;
				return -ETIME;
			}
			errno = EINVAL;
			return -EINVAL;
		}
		int e = fence_wait(seq, ms);
		if (e) {
			errno = e == -ETIMEDOUT ? ETIME : -e;
			return -errno;
		}
		if (!(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL)) {
			if (first_signaled) {
				*first_signaled = i;
			}
			return 0;
		}
	}
	if (first_signaled) {
		*first_signaled = 0;
	}
	return 0;
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	(void)points;
	return drmSyncobjWait(fd, handles, count, timeout_nsec, flags, first_signaled);
}

int
drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dst_point, uint32_t src, uint64_t src_point, uint32_t flags)
{
	(void)fd;
	(void)dst_point;
	(void)src_point;
	(void)flags;
	pthread_mutex_lock(&lock);
	struct ssync *d = ssync(dst), *s = ssync(src);
	if (d && s) {
		d->seq = s->seq;
		d->signaled = s->signaled;
	}
	pthread_mutex_unlock(&lock);
	return d && s ? 0 : -EINVAL;
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	(void)fd;
	pthread_mutex_lock(&lock);
	struct ssync *s = ssync(handle);
	uint32_t seq = s ? s->seq : 0;
	pthread_mutex_unlock(&lock);
	if (!s) {
		return -EINVAL;
	}
	*sync_file_fd = new_fd(FD_SYNCFILE, seq);
	return *sync_file_fd < 0 ? -EMFILE : 0;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd;
	pthread_mutex_lock(&lock);
	struct ssync *s = ssync(handle);
	struct sfd *f = sfd(sync_file_fd, FD_SYNCFILE);
	if (s) {
		s->seq = f ? f->value : 0;
		s->signaled = s->seq == 0;
	}
	pthread_mutex_unlock(&lock);
	return s ? 0 : -EINVAL;
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	(void)fd;
	*obj_fd = new_fd(FD_SYNCOBJ, handle);
	return *obj_fd < 0 ? -EMFILE : 0;
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)fd;
	pthread_mutex_lock(&lock);
	struct sfd *f = sfd(obj_fd, FD_SYNCOBJ);
	uint32_t h = f ? f->value : 0;
	pthread_mutex_unlock(&lock);
	if (!h) {
		return -EINVAL;
	}
	*handle = h;
	return 0;
}

/* --------------------------------------------------------------- version */

static drmVersion version = {
	/* 1.1: NOEXEC/HEAP bo flags; no timestamp queries (1.3) */
	.version_major = 1, .version_minor = 1, .version_patchlevel = 0,
	.name_len = 8, .name = "panfrost", .date_len = 0, .date = "", .desc_len = 0, .desc = "",
};

drmVersionPtr
drmGetVersion(int fd)
{
	(void)fd;
	return conn ? &version : NULL;
}

void
drmFreeVersion(drmVersionPtr v)
{
	(void)v;
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return -EINVAL;
}
