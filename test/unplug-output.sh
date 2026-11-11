#!/bin/bash
# The captured output is unplugged while a client looks (mwc's test hook:
# the output's name in <state file>.unplug, then SIGUSR2; mwc from 6db6c6a).
#
#  - two outputs: wayrdp goes over to the other one; the client follows its
#    size, gets the pipeline again and new frames keep coming
#  - one output: wayrdp waits for one, stays alive and takes logins
#
#   MWC=/path/to/mwc test/unplug-output.sh [build/wayrdp]
set -u

WAYRDP=${1:-build/wayrdp}
MWC=${MWC:-mwc}
for tool in "$MWC" foot sdl-freerdp3 xfreerdp3; do
	command -v "$tool" >/dev/null || { echo "FAIL: $tool not installed"; exit 1; }
done
[ -x "$WAYRDP" ] || { echo "FAIL: $WAYRDP not built"; exit 1; }

USER_NAME=$(id -un)
dir=
pids=()
cleanup() {
	for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
	wait 2>/dev/null
	pids=()
	[ -n "$dir" ] || return
	local p line
	for p in /proc/[0-9]*; do
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
fail() { echo "FAIL: $1"; tail -20 "$dir/wayrdp.log"; exit 1; }

start_mwc() {
	mkdir -p "$dir/$1/run" "$dir/$1/config/mwc"
	chmod 700 "$dir/$1/run"
	printf 'Mwm*rootBackground: #ff00ff\nMwm*outputManagement: true\n' \
		>"$dir/$1/config/mwc/mwmrc"
	env $3 MWC_TEST_STATE=$dir/$1.state XDG_RUNTIME_DIR=$dir/$1/run \
		XDG_CONFIG_HOME=$dir/$1/config WLR_BACKENDS=headless \
		WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman MWC_VIRTUAL_INPUT=1 \
		"$MWC" -s "$2" >"$dir/mwc-$1.log" 2>&1 &
	pids+=($!)
	for ((i = 0; i < 50; i++)); do
		[ -S "$dir/$1/run/wayland-0" ] && return 0
		sleep 0.2
	done
	fail "mwc $1 has no Wayland socket"
}

# wait_log <pattern> <what>
wait_log() {
	for ((i = 0; i < 100; i++)); do
		grep -q "$1" "$dir/wayrdp.log" && return 0
		sleep 0.2
	done
	fail "$2"
}

# setup <outputs>: mwc A with a clock, wayrdp on it, sdl-freerdp3 in B.
setup() {
	dir=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/wayrdp-unplug.XXXXXX") || exit 1
	export WAYRDP_TEST_DIR=$dir
	start_mwc a "foot sh -c 'while :; do date +%T.%N; sleep 0.1; done'" \
		"WLR_HEADLESS_OUTPUTS=$1"
	mwc_a=${pids[0]}
	local cfg=$dir/wayrdp-config
	mkdir -p "$cfg"
	PASSWORD=wayrdp-test-$RANDOM
	PORT=$((20000 + RANDOM % 20000))
	echo "$PASSWORD" | XDG_CONFIG_HOME=$cfg "$WAYRDP" --set-password 2>/dev/null \
		|| fail "--set-password"
	XDG_RUNTIME_DIR=$dir/a/run WAYLAND_DISPLAY=wayland-0 XDG_CONFIG_HOME=$cfg \
		"$WAYRDP" -L debug 127.0.0.1 "$PORT" >"$dir/wayrdp.log" 2>&1 &
	wayrdp=$!
	pids+=($wayrdp)
	wait_log "Listening on" "wayrdp doesn't listen"
	captured=$(grep -o "Capturing output [A-Z0-9-]*" "$dir/wayrdp.log" | head -1 | awk '{print $3}')
	[ -n "$captured" ] || fail "wayrdp names no output"
	start_mwc b "sdl-freerdp3 /v:127.0.0.1:$PORT /u:$USER_NAME /p:$PASSWORD /cert:ignore /size:1024x768 /log-level:OFF >$dir/client.log 2>&1" ""
	wait_log "active, 1024x768, GFX" "the client never became active"
	sleep 1
}

unplug() {
	echo "$1" >"$dir/a.state.unplug"
	kill -USR2 "$mwc_a"
}

login_works() {
	timeout 10 xfreerdp3 /v:127.0.0.1:$PORT /u:$USER_NAME /p:"$PASSWORD" \
		/cert:ignore +auth-only >"$dir/auth.log" 2>&1
}

# ---- two outputs: over to the other ----------------------------------------
# The headless backend makes its outputs 1280x720; the client asks for
# 1024x768, which the captured one takes (output management).
setup 2
other=HEADLESS-2
[ "$captured" = HEADLESS-2 ] && other=HEADLESS-1
unplug "$captured"
wait_log "Captured output $captured went away" "no word of the output going"
wait_log "Capturing output $other" "wayrdp didn't go over to $other"
wait_log "Desktop is 1280x720" "no frame of $other (1280x720)"
wait_log "active, 1280x720, GFX" "the client didn't follow to 1280x720"
echo "ok: $captured unplugged, wayrdp went over to $other, the client follows (1280x720, GFX)"
# New frames keep coming: the next stats line counts them (every 10 s).
n=$(grep -c "fps, " "$dir/wayrdp.log")
for ((i = 0; i < 60; i++)); do
	[ "$(grep -c "fps, " "$dir/wayrdp.log")" -gt "$n" ] && break
	sleep 0.25
done
fps=$(grep "fps, " "$dir/wayrdp.log" | tail -1 | grep -o "[0-9.]* fps" | cut -d' ' -f1)
awk "BEGIN { exit !(${fps:-0} > 0) }" || fail "no frames after the switch (${fps:-0} fps)"
echo "ok: frames keep coming from $other ($fps fps)"
login_works || fail "no login after the switch"
echo "ok: a new login gets in"
cleanup

# ---- one output: waiting for one --------------------------------------------
setup 1
unplug "$captured"
wait_log "No output left to capture. Waiting for one" "no word of waiting for an output"
echo "ok: the only output unplugged, wayrdp waits for one"
sleep 1
kill -0 "$wayrdp" 2>/dev/null || fail "wayrdp died"
login_works || fail "no login while waiting for an output"
echo "ok: wayrdp lives on and takes logins"
echo PASS
