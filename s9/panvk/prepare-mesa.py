#!/usr/bin/env python3
"""Apply isolated Darwin build integration to pinned Mesa. No device installation."""
import pathlib,shutil,sys
m=pathlib.Path(sys.argv[1]);h=pathlib.Path(__file__).resolve().parent
assert (m/'VERSION').read_text().strip()=='27.0.0-devel'
def edit(path,old,new):
 p=m/path;s=p.read_text();assert s.count(old)==1,(path,s.count(old));p.write_text(s.replace(old,new))
d=m/'src/s9-drm';d.mkdir(exist_ok=True)
for name in ['sweet_drm.c','sweet_drm.h']:shutil.copy(h.parent/'sweetgl'/name,d/name)
for p in (h/'include').iterdir():shutil.copy(p,d/p.name)
(d/'log.c').write_text('''#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "sweet_drm.h"
void sweet_drm_log(const char *fmt, ...) {
 char buf[1024]; va_list ap; va_start(ap,fmt); int n=vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
 if(n<0)return; if(n>(int)sizeof(buf)-2)n=sizeof(buf)-2; buf[n++]='\\n';
 int fd=open("/dev/null",O_WRONLY); if(fd>=0){(void)write(fd,buf,n);close(fd);}
}
''')
edit('meson.build',"if dep_libdrm.found()\n  pre_args += '-DHAVE_LIBDRM'", """if host_machine.system() == 'darwin' and with_panfrost_vk
  dep_libdrm = declare_dependency(
    include_directories : include_directories('src/s9-drm'),
    version : '2.4.133')
endif
if dep_libdrm.found()
  pre_args += '-DHAVE_LIBDRM'""")
edit('src/panfrost/vulkan/meson.build',"libvulkan_panfrost = shared_library(","""s9_drm_sources = []
s9_drm_link_args = []
if host_machine.system() == 'darwin'
  s9_drm_sources = files('../../s9-drm/sweet_drm.c', '../../s9-drm/log.c')
  s9_drm_link_args = ['-framework', 'IOKit', '-framework', 'CoreFoundation', '-framework', 'IOSurface', '-Wl,-fixup_chains']
endif
libvulkan_panfrost = shared_library(""")
edit('src/panfrost/vulkan/meson.build','[libpanvk_files, panvk_entrypoints, panvk_tracepoints, panvk_drirc],','[libpanvk_files, panvk_entrypoints, panvk_tracepoints, panvk_drirc] + s9_drm_sources,')
edit('src/panfrost/vulkan/meson.build','link_args : [vulkan_icd_link_args, ld_args_bsymbolic, ld_args_gc_sections, ld_args_build_id],','link_args : [vulkan_icd_link_args, ld_args_bsymbolic, ld_args_gc_sections, ld_args_build_id] + s9_drm_link_args,')
print('Darwin build integration applied; host image import still pending.')

# PanVK-only synchronization corrections; do not alter the working GL bridge.
p=d/'sweet_drm.c';s=p.read_text()
a=s.index('int\ndrmSyncobjWait(');b=s.index('int\ndrmSyncobjTimelineWait(',a)
s=s[:a]+'#include "sync-wait.inc"\n\n'+s[b:]
s=s.replace('\t(void)points;\n\treturn drmSyncobjWait', '\tfor (unsigned i = 0; i < count; i++) {\n\t\tif (points[i]) { errno = ENOTSUP; return -ENOTSUP; }\n\t}\n\treturn drmSyncobjWait')
s=s.replace('\t(void)dst_point;\n\t(void)src_point;\n\t(void)flags;', '\tif (dst_point || src_point || flags) { errno = ENOTSUP; return -ENOTSUP; }')
s=s.replace('\t\tfence_wait(seq, 2000);','\t\tif (!s) return -EINVAL;\n\t\tint err = fence_wait(seq, 2000);\n\t\tif (err) return err;')
s=s.replace('\t(void)capability;\n\t*value = 0;\n\treturn -EINVAL;', '\tif (capability == DRM_CAP_SYNCOBJ) { *value = 1; return 0; }\n\tif (capability == DRM_CAP_SYNCOBJ_TIMELINE) { *value = 0; return 0; }\n\t*value = 0;\n\terrno = EINVAL; return -EINVAL;')
p.write_text(s);shutil.copy(h/'sync-wait.inc',d/'sync-wait.inc')

# Discover the real SweetGPU service instead of probing Linux DRM nodes.
edit('src/panfrost/vulkan/panvk_physical_device.c',
 '   int fd = open(path, O_RDWR | O_CLOEXEC);',
 '''#ifdef __APPLE__
   extern int sweet_drm_open(void);
   int fd = sweet_drm_open();
#else
   int fd = open(path, O_RDWR | O_CLOEXEC);
#endif''')
edit('src/panfrost/vulkan/panvk_instance.c',
 'static void\npanvk_destroy_physical_device',
 '''#ifdef __APPLE__
static VkResult
s9_enumerate_physical_devices(struct vk_instance *instance)
{
   char *nodes[DRM_NODE_MAX] = { NULL, NULL, "/dev/null" };
   drmDevice drm_device = {
      .nodes = nodes, .available_nodes = 1 << DRM_NODE_RENDER,
      .bustype = DRM_BUS_PLATFORM,
   };
   struct vk_physical_device *device = NULL;
   VkResult result = panvk_physical_device_try_create(instance, &drm_device, &device);
   if (result == VK_ERROR_INCOMPATIBLE_DRIVER) return VK_SUCCESS;
   if (result != VK_SUCCESS) return result;
   list_addtail(&device->link, &instance->physical_devices.list);
   return VK_SUCCESS;
}
#endif

static void
panvk_destroy_physical_device''')
edit('src/panfrost/vulkan/panvk_instance.c',
 '   instance->vk.physical_devices.try_create_for_drm =\n      panvk_physical_device_try_create;',
 '''#ifdef __APPLE__
   instance->vk.physical_devices.enumerate = s9_enumerate_physical_devices;
#else
   instance->vk.physical_devices.try_create_for_drm =
      panvk_physical_device_try_create;
#endif''')
edit('src/panfrost/lib/kmod/panfrost_kmod.c',
 '   return os_mmap(host_addr, bo->size, prot, flags, bo->dev->fd, mmap_offset);',
 '''#ifdef __APPLE__
   extern void *sweet_drm_bo_map(int fd, uint64_t offset);
   if (host_addr || (flags & MAP_FIXED)) { errno = ENOTSUP; return MAP_FAILED; }
   return sweet_drm_bo_map(bo->dev->fd, mmap_offset);
#else
   return os_mmap(host_addr, bo->size, prot, flags, bo->dev->fd, mmap_offset);
#endif''')
edit('src/panfrost/lib/kmod/panfrost_kmod.c',
 '        size_t size = lseek(fd, 0, SEEK_END);',
 '''#ifdef __APPLE__
        extern int64_t sweet_drm_fd_size(int fd);
        size_t size = sweet_drm_fd_size(fd);
#else
        size_t size = lseek(fd, 0, SEEK_END);
#endif''')
# GEM-close owns XNU mappings; unmapping directly would invalidate the adapter's
# retained CPU pointer. Fixed-address mappings and external fd APIs are disabled.
edit('src/panfrost/vulkan/panvk_device_memory.c',
 '#include "panvk_device_memory.h"',
 '''#include "panvk_device_memory.h"
#ifdef __APPLE__
static int s9_bo_unmap(void *ptr, size_t size) { (void)ptr; (void)size; return 0; }
#define os_munmap s9_bo_unmap
#endif''')
p=m/'src/panfrost/vulkan/panvk_vX_physical_device.c';s=p.read_text()
for name in ['KHR_external_fence_fd','KHR_external_memory_fd','KHR_external_semaphore_fd','EXT_external_memory_dma_buf','EXT_map_memory_placed','EXT_physical_device_drm']:
 old='      .'+name+' = true,';assert s.count(old)==1,name
 s=s.replace(old,'#ifndef __APPLE__\n'+old+'\n#endif')
p.write_text(s)
print('SweetGPU discovery, BO mappings, binary synchronization and unsupported-extension guards applied.')

p=m/'src/panfrost/vulkan/panvk_vX_physical_device.c';s=p.read_text()
for name in ['memoryMapPlaced','memoryUnmapReserve']:
 old='      .'+name+' = true,';assert s.count(old)==1,name
 s=s.replace(old,'#ifndef __APPLE__\n'+old+'\n#endif')
p.write_text(s)
