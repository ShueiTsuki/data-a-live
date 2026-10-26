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

# AudioReach commands; with SHENG_DIR (UniXNU-sheng's package/ directory) also against its encoder.
if [ -n "${SHENG_DIR:-}" ]; then
	R=""
	for n in ar_graph_command ar_map ar_unmap ar_write ar_read ar_prm_clock ar_prm_clock_attr ar_prm_vote ar_parse; do
		R="$R -D$n=sheng_$n"
	done
	$CC -std=c11 -O1 -g -w -fsanitize=address,undefined $R -I"$SHENG_DIR" -c -o "$OUT/sheng_ar.o" "$SHENG_DIR/ShengAudioReach.c"
	$CC -std=c11 -O1 -g -w -fsanitize=address,undefined -I"$SHENG_DIR" -c -o "$OUT/sheng_gpr.o" "$SHENG_DIR/ShengGpr.c"
	$CC -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -DSHENG -I"$SHENG_DIR" \
		-o "$OUT/ar_host" "$H/ar_host.c" "$H/../../wsl/pandora_ar.c" "$OUT/sheng_ar.o" "$OUT/sheng_gpr.o"
else
	$CC -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o "$OUT/ar_host" "$H/ar_host.c" "$H/../../wsl/pandora_ar.c"
fi
ASAN_OPTIONS=detect_leaks=0 "$OUT/ar_host"

# GPI DMA TRE and event encoders.
$CC -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
	-o "$OUT/gpi_host" "$H/gpi_host.c"
ASAN_OPTIONS=detect_leaks=0 "$OUT/gpi_host"
