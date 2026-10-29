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
print('Darwin build integration applied; device enumeration/import integration still pending.')
