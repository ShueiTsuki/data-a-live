#!/bin/bash
cd /opt/work/tree
grep -rn -A6 -E '^#if.*XNU_PLATFORM_MacOSX|^#if.*XNU_TARGET_OS_OSX' --include=*.c --include=*.h osfmk/vm osfmk/kern osfmk/ipc bsd/kern bsd/vm iokit/Kernel 2>/dev/null | grep -E '(<< *30|GiB|GB|0x[0-9a-f]{9,})' | head -30
