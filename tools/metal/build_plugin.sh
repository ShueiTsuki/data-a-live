#!/bin/bash
# Build UniXNU's NativeMetal plugin (NativeAdrenoMetal.bundle) for iOS 16 arm64, adapted by
# adapt_ios16.py, and tools/metal/mtltest. Runs on macOS with Xcode, after tools/turnip/build_ios.sh
# (CI: .github/workflows/turnip.yml). Follows UniXNU scripts/build-native-metal-bridge.py and
# build-adreno-metal-plugin.py, without the probe daemon, the A740 replay and the 4 KiB signing.
#
#   tools/metal/build_plugin.sh UNIXNU NATIVEMETAL NATIVEADRENO METAL2VULKAN MESA OUT
#
# UNIXNU: UniXNU at its pin (kits/AdrenoPM4). NATIVEMETAL: NativeMetal at UniXNU's pin c6d29ca.
# NATIVEADRENO: NativeAdreno (NativeAdrenoABI.h). METAL2VULKAN: steelbrain/metal2vulkan at
# 720ef1ce (scripts/data/gpu-metal-vulkan-inputs.json). MESA: the patched Mesa tree (Vulkan headers).
# OUT gets NativeAdrenoMetal.bundle (executable + Info.plist), mtltest and SHA256SUMS.
set -euo pipefail
U=$(cd "$1" && pwd); NM=$(cd "$2" && pwd); NA=$(cd "$3" && pwd); M2V=$(cd "$4" && pwd); MESA=$(cd "$5" && pwd)
OUT=$6; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=$OUT/build; mkdir -p "$B/gen"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
BASE=(-target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -g -fstack-protector-strong)

echo "== NativeMetal for iOS 16"
python3 "$ROOT/tools/metal/adapt_ios16.py" "$NM"

echo "== smoke shaders (UniXNU gen-turnip-smoke-shaders.py)"
{
	echo '// Generated from NativeMetal/shaders by tools/metal/build_plugin.sh; do not edit.'
	echo '#pragma once'
	echo '#include <stdint.h>'
	for s in "$NM"/shaders/smoke_*; do
		n=$(basename "$s")
		glslc --target-env=vulkan1.1 -O "$s" -o "$B/$n.spv"
		spirv-val --target-env vulkan1.1 "$B/$n.spv"
		python3 -c 'import struct,sys; d=open(sys.argv[1],"rb").read(); w=struct.unpack("<%dI" % (len(d)//4), d)
print("static const uint32_t kSpv_%s[] = {" % sys.argv[2].replace(".", "_"))
for i in range(0, len(w), 8): print("    " + ", ".join("0x%08x" % x for x in w[i:i+8]) + ",")
print("};")' "$B/$n.spv" "$n"
	done
} > "$B/gen/NativeTurnipShaders.h"

echo "== metal2vulkan bridge (UniXNU build-native-metal-bridge.py)"
# The pinned NativeMetal has patches 0001-0007; UniXNU's script also lists 0008, which is not in it.
for p in "$NM"/bridge/patches/000*.patch; do
	echo "   $(basename "$p")"
	git -C "$M2V" apply "$p"
done
LLVM_AS=$(brew --prefix llvm)/bin/llvm-as
"$LLVM_AS" -o "$B/runtime_examples.air" "$M2V/crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll"
"$LLVM_AS" -o "$B/function_constant.air" "$M2V/crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll"
cat > "$B/Cargo.toml" <<EOF
[package]
name = "native-metal-bridge"
version = "0.1.0"
edition = "2024"
rust-version = "1.98"
[workspace]
[lib]
crate-type = ["staticlib"]
path = "$NM/bridge/NativeMetalBridge.rs"
[dependencies]
metal2vulkan = { path = "$M2V/crates/metal2vulkan", features = ["ash"] }
metal2vulkan-runtime = { path = "$M2V/crates/metal2vulkan-runtime" }
libc = "0.2"
sha2 = { version = "0.10", features = ["asm"] }
[profile.release]
panic = "unwind"
debug = 1
EOF
RID=$( (git -C "$M2V" rev-parse HEAD; git -C "$M2V" diff HEAD) | shasum -a 256 | cut -c1-32)
NMB_RUNTIME_ID=$RID NMB_FIXTURE_AIR=$B/runtime_examples.air NMB_FC_AIR=$B/function_constant.air \
	IPHONEOS_DEPLOYMENT_TARGET=16.0 \
	cargo +1.98.0 build --release --manifest-path "$B/Cargo.toml" --target aarch64-apple-ios --target-dir "$B/target"
BRIDGE=$B/target/aarch64-apple-ios/release/libnative_metal_bridge.a
ls -l "$BRIDGE"

echo "== plugin bundle (UniXNU build-adreno-metal-plugin.py)"
BUNDLE=$OUT/NativeAdrenoMetal.bundle
rm -rf "$BUNDLE"; mkdir -p "$BUNDLE"
xcrun clang "${BASE[@]}" -std=c11 -c "$U/kits/AdrenoPM4/AdrenoPM4.c" -o "$B/AdrenoPM4.o"
xcrun clang++ "${BASE[@]}" -std=c++17 -fobjc-arc -Wno-deprecated-declarations -bundle \
	-I "$NA" -I "$U/kits/AdrenoPM4" -I "$MESA/include" -I "$B/gen" \
	-x objective-c++ "$NM/NativeAdrenoMetalPlugin.mm" "$NM/NativeBridgeMetalFrontend.mm" -x none \
	"$B/AdrenoPM4.o" "$BRIDGE" -Wl,-dead_strip -lobjc -framework Foundation -framework Metal -framework IOSurface \
	-o "$BUNDLE/NativeAdrenoMetal"
strip -x "$BUNDLE/NativeAdrenoMetal"
rm -rf "$BUNDLE/NativeAdrenoMetal.dSYM"
plutil -create xml1 "$BUNDLE/Info.plist"
for kv in CFBundleIdentifier=org.blu-sharky.NativeAdrenoMetal CFBundleExecutable=NativeAdrenoMetal \
	CFBundlePackageType=BNDL CFBundleVersion=1 CFBundleShortVersionString=1 \
	NSPrincipalClass=NativeAdrenoMetalDevice MinimumOSVersion=16.0; do
	plutil -insert "${kv%%=*}" -string "${kv#*=}" "$BUNDLE/Info.plist"
done
# Only the executable's own signature matters on the phone (its CDHash goes into the trust cache).
codesign -s - -f --identifier org.blu-sharky.NativeAdrenoMetal "$BUNDLE/NativeAdrenoMetal"

echo "== mtltest"
xcrun -sdk iphoneos metal -mios-version-min=16.0 -c "$ROOT/tools/metal/mtltest.metal" -o "$B/mtltest.air"
xcrun -sdk iphoneos metallib "$B/mtltest.air" -o "$B/mtltest.metallib"
(cd "$B" && xxd -i mtltest.metallib > gen/mtltest_metallib.h)
xcrun clang "${BASE[@]}" -fobjc-arc -Wall -I "$B/gen" "$ROOT/tools/metal/mtltest.m" \
	-framework Foundation -framework Metal -framework IOSurface -o "$OUT/mtltest"
codesign -s - -f "$OUT/mtltest"

otool -L "$BUNDLE/NativeAdrenoMetal" "$OUT/mtltest"
(cd "$OUT" && shasum -a 256 NativeAdrenoMetal.bundle/NativeAdrenoMetal NativeAdrenoMetal.bundle/Info.plist mtltest \
	> SHA256SUMS && cat SHA256SUMS)
ls -l "$BUNDLE" "$OUT/mtltest"
