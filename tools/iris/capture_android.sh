#!/system/bin/sh
# capture_android.sh OUTDIR [CLIP [CODEC]]: on the phone, as root (su -c), how Android's video driver (msm_video,
# Iris4) powers the video core, loads its firmware and talks to it while vdectest decodes CLIP.
#
#  - msm_vidc_debug 0x10000037: errors, high, low (every register read and write), packets, bus, to the kernel log;
#  - ftrace: the driver's own events, clocks (prepare/enable/rate), power domains (genpd), interconnect bandwidth;
#  - the kernel log streamed to OUTDIR/dmesg.txt while vdectest runs (OUTDIR/vdectest.txt, OUTDIR/frame0.yuv), the
#    trace to OUTDIR/trace.txt.
# The driver keeps the firmware loaded for a while after a session; a first capture after the phone has been idle
# has the power-up and the firmware load. Every setting is put back as it was.
set -u
OUT=${1:?OUTDIR}
CLIP=${2:-/data/local/tmp/iris/test720.mp4}
CODEC=${3:-c2.qti.hevc.decoder}
BIN=$(dirname "$0")/vdectest
P=/sys/module/msm_video/parameters
T=/sys/kernel/tracing
mkdir -p "$OUT"

old_debug=$(cat $P/msm_vidc_debug)
old_fw=$(cat $P/msm_fw_debug)
old_on=$(cat $T/tracing_on)
old_buf=$(cat $T/buffer_size_kb)
EVENTS="msm_vidc_events clk/clk_prepare clk/clk_unprepare clk/clk_enable clk/clk_disable clk/clk_set_rate
power/power_domain_target interconnect/icc_set_bw rpm/rpm_resume rpm/rpm_suspend"
# REGMAP=1: also every register the clock controllers' drivers read and write (the power domains' GDSCs and the
# clocks' branch registers live there), filtered to devices named *clock-controller*.
REGMAP_EVENTS="regmap/regmap_reg_write regmap/regmap_reg_read"
if [ "${REGMAP:-0}" = 1 ]; then
	for e in $REGMAP_EVENTS; do
		echo 'name ~ "*clock-controller*"' > $T/events/$e/filter
	done
	EVENTS="$EVENTS $REGMAP_EVENTS"
fi

echo 0 > $T/tracing_on
echo > $T/trace
echo 16384 > $T/buffer_size_kb
for e in $EVENTS; do
	[ -e $T/events/$e/enable ] && echo 1 > $T/events/$e/enable || echo "no event $e" >> "$OUT/notes.txt"
done
echo 0x10000037 > $P/msm_vidc_debug

dmesg -w > "$OUT/dmesg.txt" &
DMESG=$!
echo 1 > $T/tracing_on
date +%s.%N > "$OUT/start.txt"
"$BIN" "$CLIP" "$CODEC" "$OUT/frame0.yuv" > "$OUT/vdectest.txt" 2>&1
echo "vdectest exit $?" >> "$OUT/vdectest.txt"
sleep 2
echo 0 > $T/tracing_on
kill $DMESG

echo "$old_debug" > $P/msm_vidc_debug
echo "$old_fw" > $P/msm_fw_debug
for e in $EVENTS; do
	[ -e $T/events/$e/enable ] && echo 0 > $T/events/$e/enable
done
for e in $REGMAP_EVENTS; do
	echo 0 > $T/events/$e/filter
done
cat $T/trace > "$OUT/trace.txt"
echo > $T/trace
echo "$old_buf" > $T/buffer_size_kb
echo "$old_on" > $T/tracing_on
echo "debug back to $(cat $P/msm_vidc_debug), fw $(cat $P/msm_fw_debug)" >> "$OUT/notes.txt"
ls -la "$OUT"
