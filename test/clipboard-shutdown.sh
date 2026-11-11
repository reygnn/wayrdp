#!/bin/bash
# wayrdp ends while a clipboard read waits for the selection owner: the
# read's callback must not touch the RDP server after it is freed.
#
#  - mwc "A" headless with wayrdp under valgrind
#  - mwc "C" runs xfreerdp3 (through Xwayland, for its clipboard)
#  - wl-copy in A owns the selection and is stopped (SIGSTOP), so it never
#    answers; wl-paste in C makes the client ask for the session clipboard
#  - wayrdp gets SIGTERM while the read waits; valgrind must find nothing
#
#   MWC=/path/to/mwc test/clipboard-shutdown.sh [build/wayrdp]
#
# Needs mwc, xfreerdp3, wl-clipboard and valgrind (all in the box).
set -u

WAYRDP=${1:-build/wayrdp}
MWC=${MWC:-mwc}
REPO=$(cd "$(dirname "$0")/.." && pwd)
for tool in "$MWC" xfreerdp3 wl-copy wl-paste valgrind; do
	command -v "$tool" >/dev/null || { echo "FAIL: $tool not installed"; exit 1; }
done
[ -x "$WAYRDP" ] || { echo "FAIL: $WAYRDP not built"; exit 1; }

dir=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/wayrdp-clip.XXXXXX") || exit 1
export WAYRDP_TEST_DIR=$dir
pids=()
copy=
cleanup() {
	[ -n "$copy" ] && kill -CONT "$copy" 2>/dev/null
	for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
	wait 2>/dev/null
	local p line
	for p in /proc/[0-9]*; do
		[ "${p#/proc/}" = "$$" ] && continue
		while IFS= read -r -d '' line; do
			if [ "$line" = "WAYRDP_TEST_DIR=$dir" ]; then
				kill "${p#/proc/}" 2>/dev/null
				break
			fi
		done 2>/dev/null <"$p/environ"
	done
	if [ "${KEEP:-}" ]; then echo "logs kept in $dir"; else rm -rf "$dir"; fi
}
trap cleanup EXIT
fail() { echo "FAIL: $1"; exit 1; }

start_mwc() {
	mkdir -p "$dir/$1/run" "$dir/$1/config/mwc"
	chmod 700 "$dir/$1/run"
	printf 'Mwm*HEADLESS-1*outputMode: %s\n' "$2" >"$dir/$1/config/mwc/mwmrc"
	XDG_RUNTIME_DIR=$dir/$1/run XDG_CONFIG_HOME=$dir/$1/config \
	WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
	MWC_VIRTUAL_INPUT=1 "$MWC" -s "${3:-true}" >"$dir/mwc-$1.log" 2>&1 &
	pids+=($!)
	for ((i = 0; i < 50; i++)); do
		[ -S "$dir/$1/run/wayland-0" ] && return 0
		sleep 0.2
	done
	fail "mwc $1 has no Wayland socket"
}

start_mwc a 1024x768
cfg=$dir/wayrdp-config
mkdir -p "$cfg"
PASSWORD=wayrdp-test-$RANDOM
PORT=$((20000 + RANDOM % 20000))
echo "$PASSWORD" | XDG_CONFIG_HOME=$cfg "$WAYRDP" --set-password 2>/dev/null \
	|| fail "--set-password"

XDG_RUNTIME_DIR=$dir/a/run WAYLAND_DISPLAY=wayland-0 XDG_CONFIG_HOME=$cfg \
	valgrind --suppressions="$REPO/util/valgrind.supp" --log-file="$dir/valgrind.log" \
	"$WAYRDP" -L debug 127.0.0.1 "$PORT" >"$dir/wayrdp.log" 2>&1 &
wpid=$!
for ((i = 0; i < 150; i++)); do
	grep -q "Listening on" "$dir/wayrdp.log" && break
	sleep 0.2
done
grep -q "Listening on" "$dir/wayrdp.log" || fail "wayrdp doesn't listen"

start_mwc c 1400x1000 \
	"xfreerdp3 /v:127.0.0.1:$PORT /u:$(id -un) /p:$PASSWORD /cert:ignore /size:1024x768 /log-level:OFF >$dir/client.log 2>&1"
for ((i = 0; i < 150; i++)); do
	grep -q "clipboard channel open" "$dir/wayrdp.log" && break
	sleep 0.2
done
grep -q "clipboard channel open" "$dir/wayrdp.log" || fail "no clipboard channel"
sleep 2

# A selection owner in the session that never answers.
XDG_RUNTIME_DIR=$dir/a/run WAYLAND_DISPLAY=wayland-0 wl-copy --foreground -- "stuck" &
copy=$!
pids+=($copy)
for ((i = 0; i < 25; i++)); do
	grep -q "session clipboard has text" "$dir/wayrdp.log" && break
	sleep 0.2
done
kill -STOP "$copy"
XDG_RUNTIME_DIR=$dir/c/run WAYLAND_DISPLAY=wayland-0 timeout 8 wl-paste -n >/dev/null 2>&1 &
pids+=($!)
for ((i = 0; i < 25; i++)); do
	grep -q "The client asks for the session clipboard" "$dir/wayrdp.log" && break
	sleep 0.2
done
grep -q "The client asks for the session clipboard" "$dir/wayrdp.log" \
	|| fail "the client never asked for the session clipboard"
echo "ok: a clipboard read waits for a selection owner that doesn't answer"

kill -TERM "$wpid"
wait "$wpid"
if grep -q "Invalid \(read\|write\)" "$dir/valgrind.log"; then
	grep -A8 -m1 "Invalid" "$dir/valgrind.log"
	fail "wayrdp touched freed memory on its way out"
fi
grep -q "ERROR SUMMARY: 0 errors" "$dir/valgrind.log" || {
	grep "ERROR SUMMARY" "$dir/valgrind.log"
	fail "valgrind found errors"
}
echo "ok: wayrdp ends cleanly while the read waits"
echo PASS
