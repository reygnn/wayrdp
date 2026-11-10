#!/bin/bash
# M1 check against mwc, end to end:
#  - mwc "A" on a headless output (magenta desktop, a terminal with a clock),
#    wayrdp serving it on localhost
#  - xfreerdp3 +auth-only with the right and a wrong password (NLA)
#  - mwc "B" on a second headless output, xfreerdp3 in it through Xwayland,
#    connected to wayrdp; a screenshot of B must show A's magenta desktop.
#
#   MWC=/path/to/mwc test/mwc-rdp.sh [build/wayrdp]
#
# CLIENT_SIZE (default 1024x768) is what the client asks for; wayrdp makes
# it 1024x768 either way. CLIENT_OPTS go to xfreerdp3. EXPECT_CODEC is what
# wayrdp must report (RemoteFX); for the Planar fallback set
# WAYRDP_FORCE_PLANAR=1 and EXPECT_CODEC=Planar, since xfreerdp3 always
# offers RemoteFX.
# Afterwards: the output changes size and the client follows, a second login
# replaces the session, and three wrong passwords lock the next connection
# out.
#
# Needs mwc, foot, Xwayland, xfreerdp3 and grim. WAYRDP_WRAP goes in front of
# wayrdp, e.g. "valgrind --log-file=/tmp/vg.log".
set -u

WAYRDP=${1:-build/wayrdp}
MWC=${MWC:-mwc}
PORT=${PORT:-$((20000 + RANDOM % 10000))}
CLIENT_SIZE=${CLIENT_SIZE:-1024x768}
CLIENT_OPTS=${CLIENT_OPTS:-}
EXPECT_CODEC=${EXPECT_CODEC:-RemoteFX}
for tool in foot xfreerdp3 grim Xwayland python3; do
	command -v $tool >/dev/null || { echo "FAIL: $tool not installed"; exit 1; }
done
[ -x "$WAYRDP" ] || { echo "FAIL: $WAYRDP not built"; exit 1; }

dir=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/wayrdp.XXXXXX") || exit 1
export WAYRDP_TEST_DIR=$dir
pids=()
cleanup() {
	for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
	for p in "${pids[@]}"; do wait "$p" 2>/dev/null; done
	# mwc's clients run in sessions of their own and outlive it.
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
	for f in "$dir"/*.log; do echo "--- $f"; tail -20 "$f"; done
	exit 1
}

# start_mwc <name> <size> <desktop colour> <startup command>
start_mwc() {
	local name=$1
	mkdir -p "$dir/$name/run" "$dir/$name/config/mwc"
	chmod 700 "$dir/$name/run"
	printf 'Mwm*HEADLESS-1*outputMode: %s\nMwm*rootBackground: %s\n' "$2" "$3" \
		>"$dir/$name/config/mwc/mwmrc"
	XDG_RUNTIME_DIR=$dir/$name/run XDG_CONFIG_HOME=$dir/$name/config \
	WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
	MWC_VIRTUAL_INPUT=1 "$MWC" -s "$4" >"$dir/mwc-$name.log" 2>&1 &
	pids+=($!)
	for ((i = 0; i < 50; i++)); do
		[ -S "$dir/$name/run/wayland-0" ] && return 0
		sleep 0.2
	done
	fail "mwc $name has no Wayland socket"
}

start_mwc a 1024x768 '#ff00ff' "foot sh -c 'while :; do date +%T.%N; sleep 0.1; done'"

cfg=$dir/wayrdp-config
mkdir -p "$cfg"
PASSWORD=wayrdp-test-$RANDOM
echo "$PASSWORD" | XDG_CONFIG_HOME=$cfg "$WAYRDP" --set-password 2>/dev/null \
	|| fail "--set-password"
[ "$(stat -c %a "$cfg/wayrdp/sam")" = 600 ] || fail "SAM file is not 0600"

XDG_RUNTIME_DIR=$dir/a/run WAYLAND_DISPLAY=wayland-0 XDG_CONFIG_HOME=$cfg \
	${WAYRDP_WRAP:-} "$WAYRDP" -L ${WAYRDP_LOG:-info} -k de 127.0.0.1 "$PORT" >"$dir/wayrdp.log" 2>&1 &
pids+=($!)
# Long enough for a first start under valgrind (RSA key generation).
for ((i = 0; i < 150; i++)); do
	grep -q "Listening on" "$dir/wayrdp.log" && break
	sleep 0.2
done
grep -q "Listening on" "$dir/wayrdp.log" || fail "wayrdp doesn't listen"
echo "ok: wayrdp listens on $PORT"
[ "$(stat -c %a "$cfg/wayrdp/tls.key")" = 600 ] || fail "tls.key is not 0600"

USER_NAME=$(id -un)
xfreerdp3 /v:127.0.0.1:$PORT /u:$USER_NAME /p:"$PASSWORD" /cert:ignore \
	+auth-only >"$dir/auth-good.log" 2>&1 || fail "NLA with the right password"
echo "ok: NLA with the right password"
if xfreerdp3 /v:127.0.0.1:$PORT /u:$USER_NAME /p:wrong /cert:ignore \
	+auth-only >"$dir/auth-bad.log" 2>&1; then
	fail "NLA with a wrong password got in"
fi
echo "ok: NLA with a wrong password refused"

start_mwc b 1400x1000 '#000000' \
	"xfreerdp3 /v:127.0.0.1:$PORT /u:$USER_NAME /p:$PASSWORD /cert:ignore /size:$CLIENT_SIZE $CLIENT_OPTS /log-level:WARN >$dir/xfreerdp.log 2>&1"

for ((i = 0; i < 150; i++)); do
	grep -q "active, 1024x768, $EXPECT_CODEC" "$dir/wayrdp.log" && break
	sleep 0.2
done
grep -q "active, 1024x768, $EXPECT_CODEC" "$dir/wayrdp.log" \
	|| fail "the client never became active with 1024x768 and $EXPECT_CODEC"
echo "ok: $(grep -o 'active, .*' "$dir/wayrdp.log" | tail -1)"

# A client that asked for another size is told 1024x768. xfreerdp3 under
# mwc keeps its window at the size it asked for (its fixed size hints), so
# only part of the desktop shows then.
min_share=40
if [ "$CLIENT_SIZE" != 1024x768 ]; then
	grep -q "desktop resized to 1024x768" "$dir/wayrdp.log" || fail "no desktop resize"
	echo "ok: client resized from $CLIENT_SIZE to 1024x768"
	min_share=10
fi

# Frames take a moment to arrive and be drawn.
ok=
for ((i = 0; i < 20; i++)); do
	sleep 0.5
	XDG_RUNTIME_DIR=$dir/b/run WAYLAND_DISPLAY=wayland-0 grim -t ppm "$dir/b.ppm" 2>/dev/null || continue
	share=$(python3 -I - "$dir/b.ppm" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
# P6\n<w> <h>\n255\n
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split())
px = parts[3]
n = 0
for i in range(0, w * h * 3, 3):
    r, g, b = px[i], px[i + 1], px[i + 2]
    if r > 200 and g < 60 and b > 200:
        n += 1
# Of the 1024x768 desktop, minus the terminal.
print(int(100 * n / (1024 * 768)))
PY
)
	if [ "${share:-0}" -ge $min_share ]; then ok=1; break; fi
done
[ "$ok" ] || fail "no magenta desktop in the client (${share:-0} %)"
echo "ok: client shows the desktop (${share} % magenta)"

# The output changes size in the session (mwmrc reloaded): the client follows.
sed -i 's/outputMode: .*/outputMode: 1280x720/' "$dir/a/config/mwc/mwmrc"
kill -HUP "${pids[0]}"
for ((i = 0; i < 75; i++)); do
	grep -q "active, 1280x720, $EXPECT_CODEC" "$dir/wayrdp.log" && break
	sleep 0.2
done
grep -q "active, 1280x720, $EXPECT_CODEC" "$dir/wayrdp.log" \
	|| fail "the client didn't follow the output to 1280x720"
echo "ok: the client follows the output to 1280x720"

# Another login takes over, as when mstsc reconnects.
xfreerdp3 /v:127.0.0.1:$PORT /u:$USER_NAME /p:"$PASSWORD" /cert:ignore \
	+auth-only >"$dir/auth-replace.log" 2>&1
grep -q "New connection replaces" "$dir/wayrdp.log" || fail "a second login didn't replace the session"
echo "ok: a second login replaces the session"

# Three wrong passwords in a row (each login above reset the count).
for i in 1 2 3; do
	xfreerdp3 /v:127.0.0.1:$PORT /u:$USER_NAME /p:wrong /cert:ignore \
		+auth-only >"$dir/auth-bad$i.log" 2>&1
done
if xfreerdp3 /v:127.0.0.1:$PORT /u:$USER_NAME /p:"$PASSWORD" /cert:ignore \
	+auth-only >"$dir/auth-locked.log" 2>&1; then
	fail "got in while locked out"
fi
grep -q "Connection refused: locked" "$dir/wayrdp.log" || fail "no lockout after three failures"
echo "ok: locked out after three failed logins"

echo "PASS"
