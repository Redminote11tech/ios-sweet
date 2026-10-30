#!/bin/bash
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
M=$(cd "${1:?Mesa source}" && pwd)
mkdir -p "${2:?output}"
O=$(cd "$2" && pwd)
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CLANG=$(xcrun --sdk iphoneos -f clang)
CLANGXX=$(xcrun --sdk iphoneos -f clang++)
cat > "$O/ios.ini" <<CROSS
[binaries]
c = ['$CLANG', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
cpp = ['$CLANGXX', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
objc = ['$CLANG', '-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK']
ar = '$(xcrun -f ar)'
strip = '$(xcrun -f strip)'
pkg-config = 'false'
[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
CROSS
# Host shader generators are built separately from unmodified upstream Mesa.
meson setup "$O/build-ios" "$M" --cross-file "$O/ios.ini" \
 --buildtype=release -Db_ndebug=true -Db_lundef=false \
 -Dplatforms= -Dgallium-drivers= -Dvulkan-drivers=panfrost \
 -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
 -Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system \
 -Dpanfrost-rust=false -Dpanfrost-disassemble=false \
 -Dbuild-tests=false -Dtools= -Dshader-cache=disabled -Dxmlconfig=disabled \
 -Dzstd=disabled -Dexpat=disabled -Dlibunwind=disabled -Dvalgrind=disabled
# -k 0: report every failing translation unit in one CI run, not just the first.
ninja -C "$O/build-ios" -k 0 -j3 src/panfrost/vulkan/libvulkan_panfrost.dylib
cp "$O/build-ios/src/panfrost/vulkan/libvulkan_panfrost.dylib" "$O/"
"$CLANG" -target arm64-apple-ios16.0 -isysroot "$SDK" -I"$M/include" -O2 -Wall -Wextra -Werror -Wl,-fixup_chains "$H/vkprobe.c" -o "$O/vkprobe"
codesign -f -s - "$O/libvulkan_panfrost.dylib" "$O/vkprobe"
nm -m "$O/libvulkan_panfrost.dylib" > "$O/symbols.txt"
# Build-only artifact until every strong dynamic lookup is implemented and audited.
if grep '(undefined)' "$O/symbols.txt" | grep 'dynamically looked up' | grep -v 'weak external' > "$O/unresolved-strong.txt"; then
 echo 'NOT INSTALLABLE: unresolved strong symbols listed in unresolved-strong.txt' >&2
 exit 1
fi
shasum -a 256 "$O/libvulkan_panfrost.dylib" "$O/vkprobe" > "$O/SHA256SUMS"
