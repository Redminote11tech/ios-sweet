#!/usr/bin/env python3
"""Remove GPU-device linkage from the offline shader-generator build only."""
from pathlib import Path
import sys
root = Path(sys.argv[1])
assert (root / 'VERSION').read_text().strip() == '27.0.0-devel'
p = root / 'src/panfrost/lib/meson.build'
s = p.read_text()
for old, new in [
    ('link_with: [libpanfrost_pixel_format, libpanfrost_per_arch, libpankmod_lib],',
     'link_with: [libpanfrost_pixel_format, libpanfrost_per_arch],'),
    ('dependencies: [deps_for_libpanfrost, libpankmod_dep, idep_nir],',
     'dependencies: [deps_for_libpanfrost, idep_nir],'),
]:
    assert s.count(old) == 1, old
    s = s.replace(old, new)
p.write_text(s)
print('Offline native shader tools: GPU kernel-device dependency removed')
