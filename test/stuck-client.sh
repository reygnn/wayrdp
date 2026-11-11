#!/bin/bash
# A client that stops reading (SIGSTOP) while the session sends a lot: the
# socket fills up. wayrdp must stay responsive (a new login gets in and
# replaces it) and must not pile frames up in memory.
#
#  - mwc "A" headless, a terminal scrolling without pause, wayrdp on it
#  - mwc "B" runs sdl-freerdp3, then gets stopped
#  - once the socket is full: xfreerdp3 +auth-only must log on within
#    5 s, the stuck client must be gone (replaced, or closed by the
#    kernel's TCP_USER_TIMEOUT), and wayrdp's memory may not have grown by
#    more than 64 MiB
#
#   MWC=/path/to/mwc test/stuck-client.sh [build/wayrdp]
set -u

WAYRDP=${1:-build/wayrdp}
MWC=${MWC:-mwc}
for tool in "$MWC" foot sdl-freerdp3 xfreerdp3; do
	command -v "$tool" >/dev/null || { echo "FAIL: $tool not installed"; exit 1; }
done
[ -x "$WAYRDP" ] || { echo "FAIL: $WAYRDP not built"; exit 1; }

dir=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/wayrdp-stuck.XXXXXX") || exit 1
export WAYRDP_TEST_DIR=$dir
pids=()
client=
cleanup() {
	[ -n "$client" ] && kill -CONT "$client" 2>/dev/null
	for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
	wait 2>/dev/null
	local p line
	for p in /proc/[0-9]*; do
		[ "${p#/proc/}" = "$$" ] && continue
		while IFS= read -r -d '' line; do
			if [ "$line" = "WAYRDP_TEST_DIR=$dir" ]; then
				kill -CONT "${p#/proc/}" 2>/dev/null
				kill "${p#/proc/}" 2>/dev/null
				break
			fi
		done 2>/dev/null <"$p/environ"
	done
	if [ "${KEEP:-}" ]; then echo "logs kept in $dir"; else rm -rf "$dir"; fi
}
trap cleanup EXIT
fail() { echo "FAIL: $1"; tail -15 "$dir/wayrdp.log"; exit 1; }

start_mwc() {
	mkdir -p "$dir/$1/run" "$dir/$1/config/mwc"
	chmod 700 "$dir/$1/run"
	printf 'Mwm*HEADLESS-1*outputMode: %s\n' "$2" >"$dir/$1/config/mwc/mwmrc"
	XDG_RUNTIME_DIR=$dir/$1/run XDG_CONFIG_HOME=$dir/$1/config \
	WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
	MWC_VIRTUAL_INPUT=1 "$MWC" -s "$3" >"$dir/mwc-$1.log" 2>&1 &
	pids+=($!)
	for ((i = 0; i < 50; i++)); do
		[ -S "$dir/$1/run/wayland-0" ] && return 0
		sleep 0.2
	done
	fail "mwc $1 has no Wayland socket"
}

start_mwc a 1280x800 "exec foot -W 150x45 sh -c 'while :; do ls -la /usr/bin /usr/lib; done'"
cfg=$dir/wayrdp-config
mkdir -p "$cfg"
PASSWORD=wayrdp-test-$RANDOM
PORT=$((20000 + RANDOM % 20000))
USER_NAME=$(id -un)
echo "$PASSWORD" | XDG_CONFIG_HOME=$cfg "$WAYRDP" --set-password 2>/dev/null \
	|| fail "--set-password"
XDG_RUNTIME_DIR=$dir/a/run WAYLAND_DISPLAY=wayland-0 XDG_CONFIG_HOME=$cfg \
	"$WAYRDP" -L debug 127.0.0.1 "$PORT" >"$dir/wayrdp.log" 2>&1 &
wpid=$!
pids+=($wpid)
for ((i = 0; i < 100; i++)); do
	grep -q "Listening on" "$dir/wayrdp.log" && break
	sleep 0.2
done
grep -q "Listening on" "$dir/wayrdp.log" || fail "wayrdp doesn't listen"

start_mwc b 1400x1000 \
	"sdl-freerdp3 /v:127.0.0.1:$PORT /u:$USER_NAME /p:$PASSWORD /cert:ignore /size:1280x800 /log-level:OFF >$dir/client.log 2>&1"
for ((i = 0; i < 150; i++)); do
	grep -q "active, 1280x800, GFX" "$dir/wayrdp.log" && break
	sleep 0.2
done
grep -q "active, 1280x800, GFX" "$dir/wayrdp.log" || fail "the client never became active"
sleep 2

client=$(pgrep -f "^sdl-freerdp3 /v:127.0.0.1:$PORT" | head -1)
[ -n "$client" ] || fail "no client process"
rss() { awk '/^VmRSS/ {print $2}' "/proc/$wpid/status"; }
before=$(rss)
kill -STOP "$client"
echo "ok: client stopped, wayrdp at $((before / 1024)) MiB"
# Until the socket is full: its send queue stops growing (on loopback at
# about 2.6 MB after some 4 s), while the connection still stands.
sendq() {
	ss -tn state established "( sport = :$PORT )" | awk 'NR > 1 {print $2}' | sort -n | tail -1
}
last=0 same=0
for ((i = 0; i < 60; i++)); do
	sleep 0.25
	queued=$(sendq)
	if [ "${queued:-0}" -gt 500000 ] && [ "$queued" = "$last" ]; then
		((++same >= 4)) && break
	else
		same=0
	fi
	last=${queued:-0}
done
[ "$same" -ge 4 ] || fail "the socket didn't fill up (${queued:-0} bytes queued)"
echo "ok: the socket to the client is full (${queued} bytes queued)"

# The main loop must still work: a new login gets in and replaces it.
start=$(date +%s)
timeout 5 xfreerdp3 /v:127.0.0.1:$PORT /u:$USER_NAME /p:"$PASSWORD" /cert:ignore \
	+auth-only >"$dir/auth.log" 2>&1 \
	|| fail "no login while a client doesn't read (main loop blocked?)"
echo "ok: a new login got in after $(($(date +%s) - start)) s"
# Gone either way: replaced by the login, or closed before by the kernel
# (TCP_USER_TIMEOUT also ends a connection whose window stays at zero).
if grep -q "New connection replaces" "$dir/wayrdp.log"; then
	echo "ok: it replaced the stuck client"
elif grep -q "disconnected after" "$dir/wayrdp.log"; then
	echo "ok: the stuck client was closed before ($(grep -o 'disconnected after [0-9]* s' "$dir/wayrdp.log" | head -1))"
else
	fail "the stuck client is neither replaced nor closed"
fi

after=$(rss)
grow=$(((after - before) / 1024))
[ "$grow" -le 64 ] || fail "wayrdp grew by $grow MiB while the client didn't read"
echo "ok: wayrdp grew by $grow MiB while the client didn't read"
echo PASS
