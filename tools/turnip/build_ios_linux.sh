#!/bin/bash
# tools/turnip/build_ios.sh for Linux (WSL): cross-builds Turnip and vktest without Xcode, from an iOS 16 SDK
# (headers and .tbd stubs only), clang 18 with ld64.lld, meson from a venv with UniXNU's pinned requirements,
# and rcodesign (apple-codesign) for the ad-hoc signatures.
#
#   SDK=/path/iPhoneOS16.1.sdk MESON=/path/meson tools/turnip/build_ios_linux.sh MESA_DIR NATIVEADRENO_DIR OUT_DIR
#
# Arguments as in build_ios.sh. A build directory left in OUT_DIR/build is reused (ninja rebuilds what changed).
set -euo pipefail
MESA=$(cd "$1" && pwd); ABI=$(cd "$2" && pwd); OUT=$3
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
: "${SDK:?SDK must name an iPhoneOS SDK}"
MESON=${MESON:-meson}
LLVM=${LLVM:-/usr/lib/llvm-18/bin}
CC=$LLVM/clang; CXX=$LLVM/clang++
FLAGS="['-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK', '-fstack-protector-strong']"
LINK="['-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK', '-fstack-protector-strong', '-fuse-ld=$LLVM/ld64.lld', '-mlinker-version=907']"
B=$OUT/build
mkdir -p "$B"
cat > "$B/ios.cross" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
objc = '$CC'
ar = '$LLVM/llvm-ar'
strip = '$LLVM/llvm-strip'
pkg-config = '/usr/bin/false'
[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = $FLAGS
cpp_args = $FLAGS
objc_args = $FLAGS
c_link_args = $LINK
cpp_link_args = $LINK
objc_link_args = $LINK
EOF
OPTIONS=(-Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=darwin "-Dtu-darwin-abi-dir=$ABI"
	-Dplatforms=macos -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled
	-Dgles2=disabled -Dllvm=disabled -Dshared-glapi=disabled -Dxmlconfig=disabled -Dshader-cache=disabled
	-Dtools= -Dbuild-tests=false -Dvideo-codecs= -Dvulkan-layers= -Dlibunwind=disabled -Dlmsensors=disabled
	-Dvalgrind=disabled -Dexpat=disabled -Dzstd=disabled -Dbuildtype=release -Db_ndebug=true
	-Dtu-builtin-source-id=pandora -Dtu-builtin-pack-header=)
SETUP=(); [ -f "$B/tu/build.ninja" ] && SETUP=(--reconfigure)
# The build machine's own compiler (meson needs one for generators) is the system cc.
env CC="$CC" CXX="$CXX" CFLAGS= CXXFLAGS= CPPFLAGS= LDFLAGS= CC_FOR_BUILD=cc CXX_FOR_BUILD=c++ \
	"$MESON" setup "${SETUP[@]}" "$B/tu" "$MESA" --cross-file "$B/ios.cross" --wrap-mode=nodownload "${OPTIONS[@]}"
ninja -C "$B/tu" src/freedreno/vulkan/libvulkan_freedreno.dylib
LIB=$OUT/libvulkan_freedreno.dylib
cp "$B/tu/src/freedreno/vulkan/libvulkan_freedreno.dylib" "$LIB"
"$LLVM/llvm-install-name-tool" -id /usr/lib/libvulkan_freedreno.dylib "$LIB"
# data-a-live: a copy with its local symbols, for tools/prof.py to name the phone's samples (never installed;
# strip -x moves no code, so its addresses are the installed library's).
cp "$LIB" "$OUT/libvulkan_freedreno.symbols"
"$LLVM/llvm-strip" -x "$LIB"
"$LLVM/llvm-nm" -g --defined-only "$LIB" | grep -q ' T _vk_icdGetInstanceProcAddr' || { echo "no vk_icdGetInstanceProcAddr"; exit 1; }
glslangValidator -V --target-env vulkan1.1 --vn fill_spv -o "$B/fill_spv.h" "$ROOT/tools/vktest/fill.comp"
glslangValidator -V --target-env vulkan1.1 --vn tri_vert_spv -o "$B/tri_vert_spv.h" "$ROOT/tools/vktest/tri.vert"
glslangValidator -V --target-env vulkan1.1 --vn tri_frag_spv -o "$B/tri_frag_spv.h" "$ROOT/tools/vktest/tri.frag"
"$CC" -target arm64-apple-ios16.0 -isysroot "$SDK" -fuse-ld="$LLVM/ld64.lld" -mlinker-version=907 -O2 -Wall \
	-I "$MESA/include" -I "$B" "$ROOT/tools/vktest/vktest.c" -o "$OUT/vktest"
# The name tool and strip change the file after the linker's signature: sign again, ad hoc as codesign -s -.
for f in "$LIB" "$OUT/vktest"; do rcodesign sign "$f" 2>/dev/null; done
"$LLVM/llvm-otool" -L "$LIB" "$OUT/vktest"
(cd "$OUT" && sha256sum libvulkan_freedreno.dylib vktest > SHA256SUMS && cat SHA256SUMS)
ls -l "$LIB" "$OUT/vktest"
