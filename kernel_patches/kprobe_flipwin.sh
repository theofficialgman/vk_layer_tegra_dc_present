#!/bin/sh
# Dynamic kprobe on tegra_dc_ext_flip() to capture per-window fields our
# static dev.c patch doesn't log yet: win->flags (gates TILED/BLOCKLINEAR/
# GLOBAL_ALPHA/etc, see TEGRA_DC_EXT_FLIP_FLAG_* in tegra_dc_ext.h),
# block_height_log2 (blocklinear tiling), x/y (source rect position),
# swap_interval. Needs CONFIG_KPROBES + CONFIG_KPROBE_EVENTS (confirmed
# enabled on the current build) but NOT a rebuild -- purely dynamic via
# debugfs tracing, gone on next reboot.
#
# Struct offsets below computed by hand from tegra_dc_ext.h's
# struct tegra_dc_ext_flip_windowattr (all fields up to the union are
# 4-byte, so no surprise padding on aarch64):
#   x=36 y=40 swap_interval=72 pre_syncpt_id=84 buff_id_u=92 buff_id_v=96
#   flags=100 global_alpha=104 block_height_log2=105
#
# Calling convention (tegra_dc_ext_flip's 2nd/3rd args, AAPCS64):
#   x1 = win (struct tegra_dc_ext_flip_windowattr *, win[0] since win_num
#        is almost always 1 in our captures so far)
#   w2 = win_num
#
# Usage: sudo sh kprobe_flipwin.sh {start|stop|read}

TR=/sys/kernel/debug/tracing

case "$1" in
start)
	set -e
	echo 0 > "$TR/tracing_on"
	# Remove any stale probe of the same name from a prior run.
	grep -q "^p:kprobes/flipwin " "$TR/kprobe_events" 2>/dev/null && \
		echo "-:flipwin" >> "$TR/kprobe_events"
	echo 'p:flipwin tegra_dc_ext_flip winnum=%x2:s32 flags=+100(%x1):u32 blockh=+105(%x1):u8 x=+36(%x1):u32 y=+40(%x1):u32 swapiv=+72(%x1):u32 presyncpt=+84(%x1):s32' \
		> "$TR/kprobe_events"
	echo 1 > "$TR/events/kprobes/flipwin/enable"
	: > "$TR/trace"
	echo 1 > "$TR/tracing_on"
	echo "kprobe armed. Run the fullscreen GL test now, then: sudo sh $0 read"
	;;
read)
	cat "$TR/trace"
	;;
stop)
	echo 0 > "$TR/events/kprobes/flipwin/enable" 2>/dev/null || true
	echo "-:flipwin" >> "$TR/kprobe_events" 2>/dev/null || true
	echo 0 > "$TR/tracing_on"
	echo "kprobe removed."
	;;
*)
	echo "usage: $0 {start|stop|read}"
	echo "if 'start' errors on the kprobe_events write, first check:"
	echo "  sudo grep -w tegra_dc_ext_flip /proc/kallsyms"
	echo "if that's empty, the function got inlined and this approach"
	echo "needs a different probe point -- report back before retrying."
	exit 1
	;;
esac
