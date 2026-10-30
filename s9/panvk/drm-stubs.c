/*
 * libdrm entry points that the common Vulkan runtime references but that have
 * no meaning on XNU/SweetGPU (there are no DRM device nodes). Each answers
 * "unsupported" so a call can never be mistaken for success, and the dylib has
 * no strong undefined libdrm symbols. Native syncobj timelines are not
 * advertised (DRM_CAP_SYNCOBJ_TIMELINE = 0); PanVK uses vk_sync_timeline.
 *
 * Isolated PanVK adapter only; the working sweetgl GL bridge does not use it.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "xf86drm.h"

int
drmGetMagic(int fd, drm_magic_t *magic)
{
	(void)fd;
	if (magic)
		*magic = 0;
	errno = ENOSYS;
	return -ENOSYS;
}

char *
drmGetDeviceNameFromFd2(int fd)
{
	(void)fd;
	errno = ENODEV;
	return NULL;
}

char *
drmGetRenderDeviceNameFromFd(int fd)
{
	(void)fd;
	errno = ENODEV;
	return NULL;
}

int
drmGetNodeTypeFromFd(int fd)
{
	(void)fd;
	errno = ENODEV;
	return -1;
}

void
drmFreeDevice(drmDevicePtr *device)
{
	/* Nothing is ever allocated by the stubs below. */
	if (device)
		*device = NULL;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	(void)devices;
	(void)count;
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	(void)flags;
	if (device)
		*device = NULL;
	errno = ENODEV;
	return -ENODEV;
}

int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	/* No DRM devices: PanVK's Apple path enumerates SweetGPU directly. */
	(void)flags;
	(void)devices;
	(void)max_devices;
	return 0;
}

int
drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device)
{
	(void)dev_id;
	(void)flags;
	if (device)
		*device = NULL;
	errno = ENODEV;
	return -ENODEV;
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points,
			 uint32_t handle_count)
{
	(void)fd;
	(void)handles;
	(void)points;
	(void)handle_count;
	errno = ENOTSUP;
	return -ENOTSUP;
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points,
		 uint32_t handle_count, uint32_t flags)
{
	(void)fd;
	(void)handles;
	(void)points;
	(void)handle_count;
	(void)flags;
	errno = ENOTSUP;
	return -ENOTSUP;
}
