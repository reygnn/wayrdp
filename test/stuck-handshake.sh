#!/bin/bash
# Clients that stop in the middle of the TLS handshake. FreeRDP accepts
# TLS (and NLA) blocking, without a timeout; each connection does that on
# a thread of its own until it is connected, so the main loop goes on:
# the session, new logins, the login timer.
#
#  - mwc "A" headless, wayrdp -a nla on it, WAYRDP_LOGIN_TIMEOUT=3
#  - test/x224-hold.py sends the X.224 request, reads the answer and then
#    nothing: while one is held, and while three are, xfreerdp3 +auth-only
#    logs on at once (in under 2 s, before the timeout); without a login
#    the held one is closed after the timeout, as still connecting; a
#    session that logged on (sdl-freerdp3) stays past the timeout; three
#    right passwords whose connections are closed between NLA and
#    PostConnect don't lock out
#  - -a pam (TLS only): wayrdp answers a new connection while one is held,
#    and closes the held one after the timeout; a client that goes on its
#    own during its password check isn't taken for one still connecting
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
# hold <port> <protocols> <name>: a connection stuck in the handshake,
# held 60 s, its process in held_<name>
hold() {
	python3 -I "$HOLD" "$1" 60 "$2" >"$dir/hold-$3.out" 2>&1 &
	pids+=($!)
	eval "held_$3=$!"
	for ((i = 0; i < 100; i++)); do
		grep -q held "$dir/hold-$3.out" && return 0
		sleep 0.1
	done
	fail "the held connection $3 got no answer"
}
# login_now <port> <what>: xfreerdp3 +auth-only logs on in under 2 s
login_now() {
	local t0=$(date +%s%N)
	timeout 10 xfreerdp3 /v:127.0.0.1:$1 /u:$USER_NAME /p:"$PASSWORD" /cert:ignore \
		+auth-only >"$dir/auth.log" 2>&1 || fail "no login $2"
	local ms=$((($(date +%s%N) - t0) / 1000000))
	[ "$ms" -lt 2000 ] || fail "the login $2 took $ms ms: the main loop waited"
	echo "ok: a login gets in after $ms ms $2"
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
# A login while one is held, and while three are: it gets in at once and
# replaces them, as any login replaces the connections before it.
hold "$PORT" 3 one
login_now "$PORT" "while a connection is stuck in the handshake"
hold "$PORT" 3 a; hold "$PORT" 3 b; hold "$PORT" 3 c
login_now "$PORT" "while three connections are stuck"
kill "$held_one" "$held_a" "$held_b" "$held_c" 2>/dev/null
wait "$held_one" "$held_a" "$held_b" "$held_c" 2>/dev/null
# Without a login it is the login timeout that closes one.
hold "$PORT" 3 late
wait_log "didn't log on in time (still connecting)" "$dir/wayrdp.log" \
	"the stuck connection wasn't closed after the timeout"
echo "ok: a stuck connection is closed after the login timeout"
kill "$held_late" 2>/dev/null; wait "$held_late" 2>/dev/null

# A session that logged on stays past the login timeout.
n_active=$(grep -c "active, " "$dir/wayrdp.log")
XDG_RUNTIME_DIR=$dir/run WAYLAND_DISPLAY=wayland-0 sdl-freerdp3 /v:127.0.0.1:$PORT \
	/u:$USER_NAME /p:"$PASSWORD" /cert:ignore /size:1024x768 /log-level:OFF \
	>"$dir/client-nla.log" 2>&1 &
session=$!
pids+=($session)
for ((i = 0; i < 100; i++)); do
	[ "$(grep -c "active, " "$dir/wayrdp.log")" -gt "$n_active" ] && break
	sleep 0.1
done
[ "$(grep -c "active, " "$dir/wayrdp.log")" -gt "$n_active" ] || fail "the NLA session never became active"
# Twice the login timeout, watching that it stays.
for ((i = 0; i < 60; i++)); do
	grep -q "disconnected after" "$dir/wayrdp.log" && break
	kill -0 "$session" 2>/dev/null || break
	sleep 0.1
done
grep -q "disconnected after" "$dir/wayrdp.log" \
	&& fail "the NLA session was cut after the login timeout"
kill -0 "$session" 2>/dev/null || fail "the NLA client is gone"
echo "ok: an NLA session stays past the login timeout"
kill "$session" 2>/dev/null; wait "$session" 2>/dev/null

# A right NLA password whose connection is closed before the main loop
# took it over is no failed login: three of them don't lock out.
# WAYRDP_HOLD_AFTER_LOGON keeps each on its thread after NLA, as a slow
# connection sequence would, until the login timeout closes it.
PORT3=$((PORT + 2))
WAYRDP_HOLD_AFTER_LOGON=1 start_wayrdp "$dir/wayrdp-hold.log" "$PORT3"
for n in 1 2 3; do
	timeout 10 xfreerdp3 /v:127.0.0.1:$PORT3 /u:$USER_NAME /p:"$PASSWORD" \
		/cert:ignore +auth-only >"$dir/auth-hold$n.log" 2>&1
	for ((i = 0; i < 100; i++)); do
		[ "$(grep -c "still connecting" "$dir/wayrdp-hold.log")" -ge $n ] && break
		sleep 0.1
	done
	[ "$(grep -c "logged on as" "$dir/wayrdp-hold.log")" -ge $n ] \
		&& [ "$(grep -c "still connecting" "$dir/wayrdp-hold.log")" -ge $n ] \
		|| fail "login $n wasn't held after NLA and closed by the timeout"
done
grep -q "failed logins" "$dir/wayrdp-hold.log" \
	&& fail "right NLA passwords closed before the handover locked out"
python3 -I "$HOLD" "$PORT3" 0 3 >/dev/null || fail "locked out after right NLA passwords"
echo "ok: right NLA passwords closed before the handover don't lock out"

# ---- -a pam ------------------------------------------------------------------
PORT2=$((PORT + 1))
start_wayrdp "$dir/wayrdp-pam.log" "$PORT2" -a pam
hold "$PORT2" 1 pam
# Answered while the other is held, not only once it is gone.
python3 -I "$HOLD" "$PORT2" 0 1 >/dev/null || fail "-a pam: wayrdp doesn't answer while a connection is stuck"
grep -q "still connecting" "$dir/wayrdp-pam.log" \
	&& fail "-a pam: the stuck connection was closed before the check, the test needs it held"
echo "ok: -a pam: wayrdp answers while a connection is stuck"
wait_log "didn't log on in time (still connecting)" "$dir/wayrdp-pam.log" \
	"-a pam: the stuck connection wasn't closed after the timeout"
echo "ok: -a pam: the stuck connection is closed after the timeout"
kill "$held_pam" 2>/dev/null; wait "$held_pam" 2>/dev/null

# A client that goes on its own before it logged on (closed while its
# password is checked) isn't taken for one still connecting.
n_stuck=$(grep -c "still connecting" "$dir/wayrdp-pam.log")
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
for ((i = 0; i < 100; i++)); do
	[ "$(grep -c "Connection from 127.0.0.1 closed" "$dir/wayrdp-pam.log")" -gt "$n_closed" ] && break
	sleep 0.1
done
[ "$(grep -c "Connection from 127.0.0.1 closed" "$dir/wayrdp-pam.log")" -gt "$n_closed" ] \
	|| fail "-a pam: the client that went wasn't closed"
[ "$(grep -c "still connecting" "$dir/wayrdp-pam.log")" = "$n_stuck" ] \
	|| fail "-a pam: a client that went on its own was logged as still connecting"
echo "ok: -a pam: a client that goes on its own isn't taken for one still connecting"
echo "PASS"
