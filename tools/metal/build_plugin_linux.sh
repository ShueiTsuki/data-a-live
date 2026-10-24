#!/bin/bash
# tools/metal/build_plugin.sh for Linux (WSL): builds NativeAdrenoMetal.bundle without Xcode, from an iOS 16 SDK
# (headers and .tbd stubs only), clang 18 and ld64.lld. mtltest is not built: its shaders need Apple's metal compiler.
#
#   SDK=/path/iPhoneOS16.1.sdk tools/metal/build_plugin_linux.sh UNIXNU NATIVEMETAL NATIVEADRENO METAL2VULKAN MESA OUT
#
# Arguments as in build_plugin.sh. NATIVEMETAL and METAL2VULKAN are patched in place: pass fresh copies.
set -euo pipefail
U=$(cd "$1" && pwd); NM=$(cd "$2" && pwd); NA=$(cd "$3" && pwd); M2V=$(cd "$4" && pwd); MESA=$(cd "$5" && pwd)
OUT=$6; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=$OUT/build; mkdir -p "$B/gen"
: "${SDK:?SDK must name an iPhoneOS SDK}"
LLVM=${LLVM:-/usr/lib/llvm-18/bin}
LD=$LLVM/ld64.lld
# The fixtures use LLVM 21+ syntax (captures(none)); Rust's llvm-tools component has a new enough llvm-as.
LLVM_AS=${LLVM_AS:-$(rustup run 1.98.0 rustc --print sysroot)/lib/rustlib/x86_64-unknown-linux-gnu/bin/llvm-as}
BASE=(-target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -g -fstack-protector-strong)
LINK=(-fuse-ld="$LD" -mlinker-version=907)

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
for p in "$NM"/bridge/patches/000*.patch; do
	echo "   $(basename "$p")"
	git -C "$M2V" apply "$p"
done
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
RID=$( (git -C "$M2V" rev-parse HEAD; git -C "$M2V" diff HEAD) | sha256sum | cut -c1-32)
# C code inside crates (cc-rs) is compiled for the phone with clang against the SDK; nothing is linked here.
NMB_RUNTIME_ID=$RID NMB_FIXTURE_AIR=$B/runtime_examples.air NMB_FC_AIR=$B/function_constant.air \
	IPHONEOS_DEPLOYMENT_TARGET=16.0 SDKROOT="$SDK" \
	CC_aarch64_apple_ios=clang CXX_aarch64_apple_ios=clang++ AR_aarch64_apple_ios="$LLVM/llvm-ar" \
	CFLAGS_aarch64_apple_ios="-target arm64-apple-ios16.0 -isysroot $SDK" \
	cargo +1.98.0 build --release --manifest-path "$B/Cargo.toml" --target aarch64-apple-ios --target-dir "$B/target"
BRIDGE=$B/target/aarch64-apple-ios/release/libnative_metal_bridge.a
ls -l "$BRIDGE"

echo "== plugin bundle (UniXNU build-adreno-metal-plugin.py)"
BUNDLE=$OUT/NativeAdrenoMetal.bundle
rm -rf "$BUNDLE"; mkdir -p "$BUNDLE"
clang "${BASE[@]}" -std=c11 -c "$U/kits/AdrenoPM4/AdrenoPM4.c" -o "$B/AdrenoPM4.o"
# A method the self-checks call that an older SDK's MTLSharedEvent does not declare (the plugin's event implements
# it): a copy of the header with it, found before the SDK's.
COMPAT=()
EVENT_H=$SDK/System/Library/Frameworks/Metal.framework/Headers/MTLEvent.h
if ! grep -q "waitUntilSignaledValue" "$EVENT_H"; then
	mkdir -p "$B/gen/compat/Metal"
	python3 - "$EVENT_H" "$B/gen/compat/Metal/MTLEvent.h" <<'PY'
import re, sys
t = open(sys.argv[1]).read()
m = re.search(r'@protocol MTLSharedEvent <MTLEvent>.*?\n@end', t, re.S)
assert m, 'no MTLSharedEvent protocol'
decl = '- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds;\n'
open(sys.argv[2], 'w').write(t[:m.end() - 4] + decl + t[m.end() - 4:])
PY
	COMPAT=(-I "$B/gen/compat")
fi
# -x drops local symbols (strip -x); lld signs ad hoc as it writes, after that.
clang++ "${BASE[@]}" "${LINK[@]}" "${COMPAT[@]}" -std=c++17 -fobjc-arc -Wno-deprecated-declarations -bundle \
	-I "$NA" -I "$U/kits/AdrenoPM4" -I "$MESA/include" -I "$B/gen" \
	-x objective-c++ "$NM/NativeAdrenoMetalPlugin.mm" "$NM/NativeBridgeMetalFrontend.mm" -x none \
	"$B/AdrenoPM4.o" "$BRIDGE" -Wl,-dead_strip -Wl,-x -Wl,-adhoc_codesign -lobjc \
	-framework Foundation -framework Metal -framework IOSurface \
	-o "$BUNDLE/NativeAdrenoMetal"
rm -rf "$BUNDLE/NativeAdrenoMetal.dSYM"
python3 - "$BUNDLE/Info.plist" <<'EOF'
import plistlib, sys
plistlib.dump({k: v for k, v in (kv.split('=', 1) for kv in (
	'CFBundleIdentifier=org.blu-sharky.NativeAdrenoMetal', 'CFBundleExecutable=NativeAdrenoMetal',
	'CFBundlePackageType=BNDL', 'CFBundleVersion=1', 'CFBundleShortVersionString=1',
	'NSPrincipalClass=NativeAdrenoMetalDevice', 'MinimumOSVersion=16.0'))}, open(sys.argv[1], 'wb'))
EOF

"$LLVM/llvm-otool" -L "$BUNDLE/NativeAdrenoMetal"
(cd "$OUT" && sha256sum NativeAdrenoMetal.bundle/NativeAdrenoMetal NativeAdrenoMetal.bundle/Info.plist > SHA256SUMS && cat SHA256SUMS)
ls -l "$BUNDLE"
