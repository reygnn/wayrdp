#!/bin/bash
# A client that stops in the middle of the TLS handshake. FreeRDP accepts
# TLS (and NLA) blocking, without a timeout, on wayrdp's main loop; the
# login deadline (src/rdp/deadline.c) must end it, or everything stands:
# the session, new logins and the login timer.
#
#  - mwc "A" headless, wayrdp -a nla on it, WAYRDP_LOGIN_TIMEOUT=3
#  - test/x224-hold.py sends the X.224 request, reads the answer and then
#    nothing; xfreerdp3 +auth-only must still log on, within 10 s, and the
#    held connection must be closed for that, as stuck in the handshake
#  - the same for -a pam (TLS only): the held connection is closed after
#    the timeout and wayrdp answers a new one; a client that goes on its
#    own during its password check isn't taken for a stuck one
#
#   test/stuck-handshake.sh [build/wayrdp]
set -u

WAYRDP=${1:-build/wayrdp}
. "$(dirname "$0")/mwc.sh"
HOLD=$(dirname "$0")/x224-hold.py
for tool in "$MWC" xfreerdp3 sdl-freerdp3 python3; do
	command -v "$tool" >/dev/null || { echo "FAIL: $tool not installed"; exit 1; }
done
[ -x "$WAYRDP" ] || { echo "FAIL: $WAYRDP not built"; exit 1; }

dir=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/wayrdp-hs.XXXXXX") || exit 1
pids=()
cleanup() {
	for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
	wait 2>/dev/null
	if [ "${KEEP:-}" ]; then echo "logs kept in $dir"; else rm -rf "$dir"; fi
}
trap cleanup EXIT
fail() {
	echo "FAIL: $1"
	for f in "$dir"/*.log; do echo "--- $f"; tail -15 "$f"; done
	exit 1
}

mkdir -p "$dir/run" "$dir/config/mwc"
chmod 700 "$dir/run"
printf 'Mwm*HEADLESS-1*outputMode: 1024x768\n' >"$dir/config/mwc/mwmrc"
XDG_RUNTIME_DIR=$dir/run XDG_CONFIG_HOME=$dir/config \
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
MWC_VIRTUAL_INPUT=1 "$MWC" -s "sleep 600" >"$dir/mwc.log" 2>&1 &
pids+=($!)
for ((i = 0; i < 50; i++)); do
	[ -S "$dir/run/wayland-0" ] && break
	sleep 0.2
done
[ -S "$dir/run/wayland-0" ] || fail "mwc has no Wayland socket"

USER_NAME=$(id -un)
PASSWORD=wayrdp-test-$RANDOM
echo "$PASSWORD" | XDG_CONFIG_HOME=$dir/wcfg "$WAYRDP" --set-password >/dev/null 2>&1 \
	|| fail "--set-password"

# start_wayrdp <log> <port> <options…>
start_wayrdp() {
	local log=$1 port=$2; shift 2
	XDG_RUNTIME_DIR=$dir/run WAYLAND_DISPLAY=wayland-0 XDG_CONFIG_HOME=$dir/wcfg \
	WAYRDP_LOGIN_TIMEOUT=3 ${WAYRDP_WRAP:-} "$WAYRDP" -L debug "$@" 127.0.0.1 "$port" \
		>"$log" 2>&1 &
	pids+=($!)
	for ((i = 0; i < 100; i++)); do
		grep -q "Listening on" "$log" && return 0
		sleep 0.2
	done
	fail "wayrdp doesn't listen on $port"
}
# hold <port> <protocols>: a connection stuck in the handshake, held 60 s
hold() {
	coproc HOLDER { python3 -I "$HOLD" "$1" 60 "$2"; }
	pids+=($HOLDER_PID)
	local answer
	read -r -t 10 answer <&"${HOLDER[0]}"
	[ "$answer" = held ] || fail "the held connection got no answer"
}
# wait_log <pattern> <log> <what>
wait_log() {
	for ((i = 0; i < 100; i++)); do
		grep -q "$1" "$2" && return 0
		sleep 0.1
	done
	fail "$3"
}

# ---- -a nla -----------------------------------------------------------------
PORT=$((20000 + RANDOM % 10000))
start_wayrdp "$dir/wayrdp.log" "$PORT"
hold "$PORT" 3
start=$(date +%s)
timeout 10 xfreerdp3 /v:127.0.0.1:$PORT /u:$USER_NAME /p:"$PASSWORD" /cert:ignore \
	+auth-only >"$dir/auth.log" 2>&1 \
	|| fail "no login while a connection is stuck in the handshake"
echo "ok: a login gets in after $(($(date +%s) - start)) s while a connection is stuck in the handshake"
wait_log "stuck in the handshake" "$dir/wayrdp.log" "the stuck connection wasn't closed as such"
echo "ok: the stuck connection is closed after the login timeout"
kill "$HOLDER_PID" 2>/dev/null; wait "$HOLDER_PID" 2>/dev/null

# ---- -a pam ------------------------------------------------------------------
PORT2=$((PORT + 1))
start_wayrdp "$dir/wayrdp-pam.log" "$PORT2" -a pam
hold "$PORT2" 1
wait_log "stuck in the handshake" "$dir/wayrdp-pam.log" "-a pam: the stuck connection wasn't closed"
python3 -I "$HOLD" "$PORT2" 0 1 >/dev/null || fail "-a pam: wayrdp doesn't answer after the stuck connection"
echo "ok: -a pam: the stuck connection is closed, wayrdp answers again"

# A client that goes on its own before it logged on (closed while its
# password is checked) is no stuck handshake, though FreeRDP sets its
# abort event then too.
n_stuck=$(grep -c "stuck in the handshake" "$dir/wayrdp-pam.log")
n_sent=$(grep -c "client wants" "$dir/wayrdp-pam.log")
n_closed=$(grep -c "Connection from 127.0.0.1 closed" "$dir/wayrdp-pam.log")
XDG_RUNTIME_DIR=$dir/run WAYLAND_DISPLAY=wayland-0 sdl-freerdp3 /v:127.0.0.1:$PORT2 \
	/sec:tls /cert:ignore /size:1024x768 /u:$USER_NAME /p:wrong /log-level:OFF \
	>"$dir/client.log" 2>&1 &
client=$!
pids+=($client)
for ((i = 0; i < 100; i++)); do
	[ "$(grep -c "client wants" "$dir/wayrdp-pam.log")" -gt "$n_sent" ] && break
	sleep 0.1
done
kill -TERM "$client"
# The stuck message would come in the same call, before the close.
for ((i = 0; i < 100; i++)); do
	[ "$(grep -c "Connection from 127.0.0.1 closed" "$dir/wayrdp-pam.log")" -gt "$n_closed" ] && break
	sleep 0.1
done
[ "$(grep -c "Connection from 127.0.0.1 closed" "$dir/wayrdp-pam.log")" -gt "$n_closed" ] \
	|| fail "-a pam: the client that went wasn't closed"
[ "$(grep -c "stuck in the handshake" "$dir/wayrdp-pam.log")" = "$n_stuck" ] \
	|| fail "-a pam: a client that went on its own was logged as stuck in the handshake"
echo "ok: -a pam: a client that goes on its own isn't taken for a stuck handshake"
echo "PASS"
