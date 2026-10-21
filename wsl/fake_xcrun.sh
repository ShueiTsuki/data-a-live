#!/bin/bash
# Stand-in SDK for the staging scripts: the only SDK file they copy (TargetConditionals.h)
# comes from CI run 1's build tree. Not a real SDK.
set -e
S=/opt/work/fakesdk; mkdir -p $S/usr/include
f=$(find /opt/work/run1/bt/xnu -name TargetConditionals.h | head -1); echo "source: $f"
cp "$f" $S/usr/include/
cat > /usr/local/bin/xcrun <<'X'
#!/bin/bash
# minimal xcrun: only --show-sdk-path is supported
for a in "$@"; do [ "$a" = --show-sdk-path ] && { echo /opt/work/fakesdk; exit 0; }; done
echo "fake xcrun: unsupported: $*" >&2; exit 1
X
chmod +x /usr/local/bin/xcrun; xcrun --sdk macosx --show-sdk-path
