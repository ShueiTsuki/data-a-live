#!/bin/bash
# Host tests for kernel-side libraries (WSL: clang with address and undefined-behaviour checks).
#   tools/hosttest/run.sh [FW-DIR [REGDUMP]]   (vendor files and dumps stay outside the repository)
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/pandora-hosttest
mkdir -p "$OUT"
CC=$(command -v clang-18 || command -v clang)
$CC -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
	-o "$OUT/csfw_host" "$H/csfw_host.c" "$H/../../wsl/pandora_csfw.c"
ASAN_OPTIONS=detect_leaks=0 "$OUT/csfw_host" "$@"
