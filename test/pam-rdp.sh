#!/bin/bash
# -a pam against mwc: TLS without NLA, the Linux password checked with PAM.
#
#   MWC=/path/to/mwc PAM_PASSWORD=<your password> test/pam-rdp.sh [build/wayrdp]
#
# Needs /etc/pam.d/wayrdp (meson install, or wayrdp.pam copied there) and the
# password of the user running the test. Checks: the right password gets
# the picture, a wrong one and an empty one are refused, a connection that
# doesn't log on is closed (WAYRDP_LOGIN_TIMEOUT=3), and the three
# failures lock new connections out. WAYRDP_WRAP goes in front of wayrdp,
# e.g. "valgrind --log-file=/tmp/vg.log".
set -u

WAYRDP=${1:-build/wayrdp}
MWC=${MWC:-mwc}
PORT=${PORT:-$((20000 + RANDOM % 10000))}
[ -n "${PAM_PASSWORD:-}" ] || { echo "SKIP: set PAM_PASSWORD"; exit 77; }
[ -e /etc/pam.d/wayrdp ] || { echo "SKIP: no /etc/pam.d/wayrdp"; exit 77; }
for tool in foot sdl-freerdp3 grim python3; do
	command -v $tool >/dev/null || { echo "FAIL: $tool not installed"; exit 1; }
done

dir=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/wayrdp.XXXXXX") || exit 1
export WAYRDP_TEST_DIR=$dir
pids=()
cleanup() {
	for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
	for p in "${pids[@]}"; do wait "$p" 2>/dev/null; done
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
fail() {
	echo "FAIL: $1"
	for f in "$dir"/*.log; do echo "--- $f"; tail -15 "$f"; done
	exit 1
}

start_mwc() {
	local name=$1
	mkdir -p "$dir/$name/run" "$dir/$name/config/mwc"
	chmod 700 "$dir/$name/run"
	printf 'Mwm*HEADLESS-1*outputMode: 1024x768\nMwm*rootBackground: %s\n' "$2" \
		>"$dir/$name/config/mwc/mwmrc"
	XDG_RUNTIME_DIR=$dir/$name/run XDG_CONFIG_HOME=$dir/$name/config \
	WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
	MWC_VIRTUAL_INPUT=1 "$MWC" -s "$3" >"$dir/mwc-$name.log" 2>&1 &
	pids+=($!)
	for ((i = 0; i < 50; i++)); do
		[ -S "$dir/$name/run/wayland-0" ] && return 0
		sleep 0.2
	done
	fail "mwc $name has no Wayland socket"
}

# wait_log <pattern> <what>
wait_log() {
	for ((i = 0; i < 150; i++)); do
		grep -q "$1" "$dir/wayrdp.log" && return 0
		sleep 0.2
	done
	fail "$2"
}

start_mwc a '#ff00ff' "foot sh -c 'while :; do date +%T.%N; sleep 0.1; done'"

mkdir -p "$dir/cfg"
XDG_RUNTIME_DIR=$dir/a/run WAYLAND_DISPLAY=wayland-0 XDG_CONFIG_HOME=$dir/cfg \
WAYRDP_LOGIN_TIMEOUT=3 \
	${WAYRDP_WRAP:-} "$WAYRDP" -L debug -a pam 127.0.0.1 "$PORT" >"$dir/wayrdp.log" 2>&1 &
pids+=($!)
wait_log "Listening on" "wayrdp doesn't listen"
echo "ok: wayrdp -a pam listens on $PORT, without a SAM file"

USER_NAME=$(id -un)
client() {	# client <mwc> <args…>: sdl-freerdp3 over TLS in a new mwc
	local name=$1; shift
	start_mwc "$name" '#000000' \
		"sdl-freerdp3 /v:127.0.0.1:$PORT /sec:tls /cert:ignore /size:1024x768 $* /log-level:WARN >$dir/client-$name.log 2>&1"
}

# The right password: the picture.
client d /u:$USER_NAME /p:"$PAM_PASSWORD"
wait_log "logged on as $USER_NAME (PAM)" "the right password wasn't taken"
wait_log "active, 1024x768" "no session with the right password"
share=0
for ((i = 0; i < 40; i++)); do
	sleep 0.5
	XDG_RUNTIME_DIR=$dir/d/run WAYLAND_DISPLAY=wayland-0 grim -t ppm "$dir/d.ppm" 2>/dev/null || continue
	share=$(python3 -I - "$dir/d.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split())
px = parts[3]
n = sum(1 for i in range(0, w * h * 3, 3)
        if px[i] > 200 and px[i + 1] < 60 and px[i + 2] > 200)
print(int(100 * n / (1024 * 768)))
PY
)
	[ "$share" -ge 40 ] && break
done
[ "$share" -ge 40 ] || fail "no picture with the right password ($share %)"
echo "ok: the right password gets the picture ($share % magenta)"


# A wrong password: PAM says no, the connection goes.
n_active=$(grep -c "active, " "$dir/wayrdp.log")
client b /u:$USER_NAME /p:wrong-password
wait_log "wrong password for" "a wrong password wasn't refused"
sleep 1
[ "$(grep -c "active, " "$dir/wayrdp.log")" = "$n_active" ] || fail "a wrong password got a session"
echo "ok: a wrong password is refused"

# An empty password sent along (sdl-freerdp3 without /p would ask on the
# terminal): refused, there is no login screen.
client c /u:$USER_NAME /p:
wait_log "no credentials sent along" "a client without credentials wasn't refused"
echo "ok: no credentials, refused"

# A connection that never logs on is closed.
exec 3<>/dev/tcp/127.0.0.1/$PORT
wait_log "didn't log on in time" "a silent connection stayed"
exec 3>&-
echo "ok: a silent connection is closed after the login timeout"

# Those were three failures in a row: locked out now.
client e /u:$USER_NAME /p:"$PAM_PASSWORD"
wait_log "Connection refused: locked" "no lockout after three failures"
echo "ok: locked out after three failures"

# Four connections may wait for their logon at a time, not five. A fresh
# wayrdp: the one above is locked out.
PORT2=$((PORT + 1))
XDG_RUNTIME_DIR=$dir/a/run WAYLAND_DISPLAY=wayland-0 XDG_CONFIG_HOME=$dir/cfg \
	"$WAYRDP" -L debug -a pam 127.0.0.1 "$PORT2" >"$dir/wayrdp2.log" 2>&1 &
pids+=($!)
for ((i = 0; i < 100; i++)); do
	grep -q "Listening on" "$dir/wayrdp2.log" && break
	sleep 0.2
done
for fd in 4 5 6 7 8; do
	eval "exec $fd<>/dev/tcp/127.0.0.1/$PORT2"
	sleep 0.3
done
for ((i = 0; i < 25; i++)); do
	grep -q "are logging on already" "$dir/wayrdp2.log" && break
	sleep 0.2
done
for fd in 4 5 6 7 8; do eval "exec $fd>&-"; done
grep -q "Connection refused: 4 are logging on already" "$dir/wayrdp2.log" \
	|| fail "a fifth waiting connection wasn't refused"
echo "ok: at most four connections wait for their logon"

grep -q "$PAM_PASSWORD" "$dir/wayrdp.log" && fail "the password is in the log"
echo "ok: the password is not in the log"
echo "PASS"
