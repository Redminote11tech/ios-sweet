#!/bin/sh
# Build sweetgl.dylib for the Galaxy S9 (Mesa panfrost + GLES + our frontend,
# iOS 16 arm64) on macOS with Xcode. Used by .github/workflows/sweetgl.yml.
#
#   s9/sweetgl/build.sh MESA_SRC BUILD_DIR OUT_DIR
#
# MESA_SRC: a Mesa tree (branch mesa-src) with s9/sweetgl/mesa.patch applied.
# After joranv1/ios-sailfish tools/sweetgl/{mesa-setup,build}.sh.
set -e
H=$(cd "$(dirname "$0")" && pwd)
M=$(cd "$1" && pwd)
mkdir -p "$2" "$3"
B=$(cd "$2" && pwd)
O=$(cd "$3" && pwd)
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CLANG=$(xcrun --sdk iphoneos -f clang)
CLANGXX=$(xcrun --sdk iphoneos -f clang++)
AR=$(xcrun --sdk iphoneos -f ar)
STRIP=$(xcrun --sdk iphoneos -f strip)
ARCH="'-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0'"
cat > "$O/ios-cross.txt" <<EOF
[binaries]
c = ['$CLANG', $ARCH]
cpp = ['$CLANGXX', $ARCH]
objc = ['$CLANG', $ARCH]
ar = '$AR'
strip = '$STRIP'
pkg-config = 'false'

[built-in options]
c_args = ['-I$H/include']
cpp_args = ['-I$H/include']

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
EOF

if [ ! -f "$B/build.ninja" ]; then
	(cd "$M" && meson setup "$B" --cross-file "$O/ios-cross.txt" \
		--buildtype=release -Db_ndebug=true -Ddefault_library=static \
		-Dplatforms= -Degl=disabled -Dglx=disabled -Dgbm=disabled \
		-Dgallium-drivers=panfrost -Dvulkan-drivers= \
		-Dllvm=disabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dshared-glapi=enabled \
		-Dzstd=disabled -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled \
		-Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
		-Dbuild-tests=false -Dshader-cache=disabled -Dperfetto=false \
		-Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled \
		-Dgallium-nine=false -Dgallium-rusticl=false -Dgallium-opencl=disabled \
		-Dtools= -Dhtml-docs=disabled -Dteflon=false -Dosmesa=false -Dvideo-codecs=)
fi

# every static library the configuration defines, minus tests and the
# non-shared glapi variant
ninja -C "$B" -t targets all | sed -n 's/^\([^:]*\.a\): .*/\1/p' |
	grep -v -e test -e gtest -e glapi_static | sort -u > "$O/libs.txt"
echo "static libraries: $(wc -l < "$O/libs.txt")"
ninja -C "$B" $(cat "$O/libs.txt")

nm -g "$B/src/mapi/glapi/libglapi_bridge.a" > "$O/bridge.nm"
CC="$CLANG -arch arm64 -isysroot $SDK -miphoneos-version-min=16.0"
INC="-I$H/include -I$M/include -I$M/src -I$M/src/mesa -I$M/src/mapi -I$M/src/gallium/include
	-I$M/src/gallium/auxiliary -I$M/src/gallium/drivers -I$M/src/gallium/winsys
	-I$B/src -I$B/src/mesa -I$B/src/mapi"
# Mesa's configure-time defines (HAVE_PTHREAD, ...) as the state tracker gets them
python3 -I -c '
import json, shlex, sys
cc = json.load(open(sys.argv[1]))
e = [x for x in cc if x["file"].endswith("state_tracker/st_manager.c")][0]
args = e.get("arguments") or shlex.split(e["command"])
print("\n".join(shlex.quote(a) for a in args if a.startswith("-D")))
' "$B/compile_commands.json" > "$O/mesa-defines.rsp"
python3 -I "$H/gen-dispatch.py" "$H/ios-dispatch.txt" "$M/include" "$O/bridge.nm" "$O/dispatch.c"
for f in sweetgl sweet_drm eagl layerdump posterboard oslog "$O/dispatch"; do
	case $f in /*) src=$f.c ;; *) src=$H/$f.c ;; esac
	$CC -O2 -Wall -fvisibility=hidden @"$O/mesa-defines.rsp" $INC -I"$H" -c "$src" -o "$O/$(basename "$f").o"
done
printf '_sweetgl_init\n_sweetgl_create\n_sweetgl_make_current\n_sweetgl_destroy\n_sweetgl_selftest\n' > "$O/exports.txt"
$CLANGXX -arch arm64 -isysroot "$SDK" -miphoneos-version-min=16.0 -dynamiclib -dead_strip -Wl,-x -Wl,-not_for_dyld_shared_cache \
	-install_name /usr/lib/sweetgl.dylib -exported_symbols_list "$O/exports.txt" \
	-o "$O/sweetgl.dylib" "$O/sweetgl.o" "$O/sweet_drm.o" "$O/eagl.o" "$O/layerdump.o" "$O/posterboard.o" "$O/oslog.o" "$O/dispatch.o" \
	$(sed "s|^|$B/|" "$O/libs.txt") \
	-framework IOKit -framework CoreFoundation -framework IOSurface -framework OpenGLES \
	-framework CoreGraphics -lobjc -lc++ 2>&1 | tee "$O/link.log"
[ -f "$O/sweetgl.dylib" ]
codesign -f -s - "$O/sweetgl.dylib"
ls -la "$O/sweetgl.dylib"
