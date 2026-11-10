#!/bin/bash
# M0 check against mwc: start mwc on a headless output with a terminal that
# keeps drawing, let wayrdp connect, capture a few frames and check their size.
# The first frame is the whole screen; the others need something to change,
# hence the clock in foot.
#
#   MWC=/path/to/mwc test/mwc-probe.sh [build/wayrdp]
#
# Needs mwc built with wlroots 0.19 (https://github.com/reygnn/mwc).
set -u

WAYRDP=${1:-build/wayrdp}
MWC=${MWC:-mwc}
command -v "$MWC" >/dev/null || [ -x "$MWC" ] || { echo "FAIL: mwc not found, set MWC"; exit 1; }
[ -x "$WAYRDP" ] || { echo "FAIL: $WAYRDP not built"; exit 1; }

command -v foot >/dev/null || { echo "FAIL: foot not installed"; exit 1; }

dir=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/wayrdp.XXXXXX") || exit 1
export WAYRDP_PROBE_DIR=$dir
mwc_pid=
cleanup() {
	if [ -n "$mwc_pid" ]; then
		kill "$mwc_pid" 2>/dev/null
		wait "$mwc_pid" 2>/dev/null
	fi
	# foot runs in a session of its own and outlives mwc. Everything
	# started here has WAYRDP_PROBE_DIR in its environment.
	local p line
	for p in /proc/[0-9]*; do
		[ "${p#/proc/}" = "$$" ] && continue
		while IFS= read -r -d '' line; do
			if [ "$line" = "WAYRDP_PROBE_DIR=$dir" ]; then
				kill "${p#/proc/}" 2>/dev/null
				break
			fi
		done 2>/dev/null <"$p/environ"
	done
	rm -rf "$dir"
}
trap cleanup EXIT

mkdir -p "$dir/config/mwc"
cat >"$dir/config/mwc/mwmrc" <<'RC'
Mwm*HEADLESS-1*outputMode: 1024x768
RC

XDG_RUNTIME_DIR=$dir XDG_CONFIG_HOME=$dir/config \
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
MWC_VIRTUAL_INPUT=1 "$MWC" -s "foot sh -c 'while :; do date +%T.%N; sleep 0.1; done'" \
	>"$dir/mwc.log" 2>&1 &
mwc_pid=$!

for ((i = 0; i < 50; i++)); do
	[ -S "$dir/wayland-0" ] && break
	kill -0 "$mwc_pid" 2>/dev/null || { cat "$dir/mwc.log"; echo "FAIL: mwc died"; exit 1; }
	sleep 0.2
done
[ -S "$dir/wayland-0" ] || { echo "FAIL: no Wayland socket"; exit 1; }

out=$(XDG_RUNTIME_DIR=$dir WAYLAND_DISPLAY=wayland-0 \
	timeout 20 "$WAYRDP" -L ${WAYRDP_LOG:-info} -k de -o HEADLESS-1 --probe=3 2>"$dir/wayrdp.log")
rc=$?
cat "$dir/wayrdp.log"
[ $rc -eq 0 ] || { echo "FAIL: wayrdp exited with $rc"; exit 1; }

echo "$out"
case $out in
1024x768\ *frames=3\ *) echo "PASS" ;;
*) echo "FAIL: unexpected probe output"; exit 1 ;;
esac
