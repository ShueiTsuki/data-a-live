#!/bin/bash
# Download and unpack the Swift 6.4.1 toolchain for Ubuntu 24.04 into /opt/swift.
set -e
V=6.4.1
F=swift-$V-RELEASE-ubuntu24.04.tar.gz
URL=https://download.swift.org/swift-$V-release/ubuntu2404/swift-$V-RELEASE/$F
mkdir -p /opt/dl && cd /opt/dl
[ -s $F ] || curl -L --fail -m 1800 -o $F "$URL"
ls -l $F
mkdir -p /opt/swift && tar -xzf $F -C /opt/swift --strip-components=1
/opt/swift/usr/bin/clang --version | head -2
ls /opt/swift/usr/bin | grep -E 'ld|lld|mig' | head
