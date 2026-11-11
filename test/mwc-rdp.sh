#!/bin/bash
# M1 check against mwc, end to end:
#  - mwc "A" on a headless output (magenta desktop, a terminal with a clock),
#    wayrdp serving it on localhost
#  - xfreerdp3 +auth-only with the right and a wrong password (NLA)
#  - mwc "B" on a second headless output, an RDP client in it
#    (sdl-freerdp3 forced onto Wayland, or CLIENT=xfreerdp3 through
#    Xwayland, or wlfreerdp3),
#    connected to wayrdp; a screenshot of B must show A's magenta desktop.
#  - test/vinput moves, clicks and types in B, on the client window; A's
#    pointer, focus and a terminal running cat must show it, German layout
#    on wayrdp's side (-k de).
#  - mwc "C" runs xfreerdp3, which replaces B's session; wl-copy in one,
#    wl-paste in the other, both ways, Unicode and several lines; no echo.
#  - a pointer image goes to the client.
#  - mwc "D" runs xfreerdp3 /dynamic-resolution asking for 901x700: the
#    output becomes 900x700 (even width), and when D's window is resized to
#    1100x800 the output follows (display control).
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
# Needs mwc, foot, Xwayland, xfreerdp3, sdl-freerdp3, grim, wl-clipboard
# and xdotool, and build/test/vinput. WAYRDP_WRAP goes in front of
# wayrdp, e.g. "valgrind --log-file=/tmp/vg.log".
set -u

WAYRDP=${1:-build/wayrdp}
MWC=${MWC:-mwc}
PORT=${PORT:-$((20000 + RANDOM % 10000))}
CLIENT_SIZE=${CLIENT_SIZE:-1024x768}
CLIENT_OPTS=${CLIENT_OPTS:-}
EXPECT_CODEC=${EXPECT_CODEC:-RemoteFX}
# The client in B: sdl-freerdp3, a Wayland client, passes the input on.
# xfreerdp3 under Xwayland in mwc sends the pointer once and then no more,
# so the input part doesn't run with it. wlfreerdp3 (deprecated upstream)
# works too, but now and then hangs once pointer images come.
CLIENT=${CLIENT:-sdl-freerdp3}
# With output management (Mwm*outputManagement: true in A) the output takes
# the client's size; without it the client gets the output's.
OUTPUT_MANAGEMENT=${OUTPUT_MANAGEMENT:-1}
if [ "$OUTPUT_MANAGEMENT" = 1 ]; then SIZE=$CLIENT_SIZE; else SIZE=1024x768; fi
for tool in foot xfreerdp3 "$CLIENT" grim Xwayland python3 wl-copy wl-paste xdotool; do
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
# Both use the German layout; A writes its state (pointer, focus) to
# $dir/a.state on SIGUSR1.
start_mwc() {
	local name=$1
	mkdir -p "$dir/$name/run" "$dir/$name/config/mwc"
	chmod 700 "$dir/$name/run"
	printf 'Mwm*HEADLESS-1*outputMode: %s\nMwm*rootBackground: %s\nMwm*keyboardLayout: de\n' \
		"$2" "$3" >"$dir/$name/config/mwc/mwmrc"
	if [ "$name" = a ] && [ "$OUTPUT_MANAGEMENT" = 1 ]; then
		echo 'Mwm*outputManagement: true' >>"$dir/$name/config/mwc/mwmrc"
	fi
	MWC_TEST_STATE=$dir/$name.state \
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

# A clock that keeps the frames coming, and a small terminal that writes
# what is typed into it to a file.
start_mwc a 1024x768 '#ff00ff' "foot sh -c 'while :; do date +%T.%N; sleep 0.1; done' & exec foot -a typer -W 40x4 sh -c 'stty -echo; exec cat > $dir/typed'"

cfg=$dir/wayrdp-config
mkdir -p "$cfg"
PASSWORD=wayrdp-test-$RANDOM
echo "$PASSWORD" | XDG_CONFIG_HOME=$cfg "$WAYRDP" --set-password 2>/dev/null \
	|| fail "--set-password"
[ "$(stat -c %a "$cfg/wayrdp/sam")" = 600 ] || fail "SAM file is not 0600"

# A SAM file others can read: wayrdp won't start.
chmod 644 "$cfg/wayrdp/sam"
if XDG_RUNTIME_DIR=$dir/a/run WAYLAND_DISPLAY=wayland-0 XDG_CONFIG_HOME=$cfg \
		timeout 20 "$WAYRDP" 127.0.0.1 "$PORT" >"$dir/perm.log" 2>&1; then
	fail "wayrdp started with a SAM file others can read"
fi
grep -q "can be read by others" "$dir/perm.log" || fail "no reason given for the SAM file"
chmod 600 "$cfg/wayrdp/sam"
echo "ok: a SAM file others can read is refused"

XDG_RUNTIME_DIR=$dir/a/run WAYLAND_DISPLAY=wayland-0 XDG_CONFIG_HOME=$cfg \
	${WAYRDP_WRAP:-} "$WAYRDP" -L ${WAYRDP_LOG:-debug} -k de ${WAYRDP_OPTS:-} 127.0.0.1 "$PORT" >"$dir/wayrdp.log" 2>&1 &
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

# Left alone, SDL3 takes X11 (Xwayland) in mwc, most likely because mwc
# offers no wp_fifo_v1; through Xwayland the first wheel event gets lost.
# Other clients ignore the variable.
start_mwc b 1400x1000 '#000000' \
	"SDL_VIDEO_DRIVER=wayland $CLIENT /v:127.0.0.1:$PORT /u:$USER_NAME /p:$PASSWORD /cert:ignore /size:$CLIENT_SIZE $CLIENT_OPTS /log-level:WARN >$dir/client.log 2>&1"
mwc_b=${pids[-1]}

for ((i = 0; i < 150; i++)); do
	grep -q "active, $SIZE, $EXPECT_CODEC" "$dir/wayrdp.log" && break
	sleep 0.2
done
grep -q "active, $SIZE, $EXPECT_CODEC" "$dir/wayrdp.log" \
	|| fail "the client never became active with $SIZE and $EXPECT_CODEC"
echo "ok: $(grep -o 'active, .*' "$dir/wayrdp.log" | tail -1)"

# A client that asked for another size: with output management the output
# takes it; without, the client is told 1024x768 and keeps its window at
# the size it asked for, so only part of the desktop shows.
min_share=40
w=${SIZE%x*}; h=${SIZE#*x}
if [ "$CLIENT_SIZE" != 1024x768 ]; then
	if [ "$OUTPUT_MANAGEMENT" = 1 ]; then
		grep -q "output follows the client: $CLIENT_SIZE" "$dir/wayrdp.log" \
			|| fail "the output didn't take the client's size"
		echo "ok: the output takes the client's size, $CLIENT_SIZE"
		# A smaller desktop: the terminals cover more of it.
		min_share=10
	else
		grep -q "desktop resized to 1024x768" "$dir/wayrdp.log" || fail "no desktop resize"
		echo "ok: client resized from $CLIENT_SIZE to 1024x768"
		min_share=10
	fi
fi

# share_magenta <mwc> <width> <height>: how much of a w x h desktop shows
# A's magenta in a screenshot of the mwc, in percent.
share_magenta() {
	XDG_RUNTIME_DIR=$dir/$1/run WAYLAND_DISPLAY=wayland-0 \
		grim -t ppm "$dir/$1.ppm" 2>/dev/null || { echo 0; return; }
	python3 -I - "$dir/$1.ppm" "$2" "$3" <<'PY'
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
# Of the desktop, minus the terminals.
print(int(100 * n / (int(sys.argv[2]) * int(sys.argv[3]))))
PY
}

# expect_desktop <mwc> <width> <height> <min %>: frames take a moment to
# arrive and be drawn (longer under valgrind).
expect_desktop() {
	share=0
	for ((i = 0; i < 40; i++)); do
		sleep 0.5
		share=$(share_magenta "$1" "$2" "$3")
		[ "${share:-0}" -ge "$4" ] && return 0
	done
	return 1
}

if expect_desktop b "$w" "$h" "$min_share"; then
	echo "ok: client shows the desktop (${share} % magenta)"
else
	fail "no magenta desktop in the client (${share:-0} %)"
fi

# ---- input ---------------------------------------------------------------

# state <mwc> <kind> <field> [app]: a value from mwc's test state.
state() {
	local name=$1 kind=$2 key=$3 app=${4:-}
	rm -f "$dir/$name.state"
	local p=${pids[0]}
	[ "$name" = b ] && p=$mwc_b
	kill -USR1 "$p"
	for ((j = 0; j < 100; j++)); do
		[ -e "$dir/$name.state" ] && break
		sleep 0.02
	done
	awk -v kind="$kind" -v app="app=$app" -v key="$key=" '
		$1 == kind && (kind != "view" || app == "app=*" || $2 == app) {
			for (i = 2; i <= NF; i++)
				if (index($i, key) == 1) { print substr($i, length(key) + 1); exit }
		}' "$dir/$name.state"
}

VINPUT=${VINPUT:-$(dirname "$WAYRDP")/test/vinput}
if [ "$CLIENT_SIZE" = 1024x768 ] && [ "$CLIENT" != xfreerdp3 ] && [ -x "$VINPUT" ]; then
	coproc VIN { XDG_RUNTIME_DIR=$dir/b/run WAYLAND_DISPLAY=wayland-0 \
		"$VINPUT" 1400 1000 2>"$dir/vinput.log"; }
	pids+=($VIN_PID)
	vin() {
		local answer
		echo "$*" >&"${VIN[1]}"
		read -r answer <&"${VIN[0]}" || fail "vinput is gone ($*)"
		[ "$answer" = ok ] || fail "vinput: $answer ($*)"
	}
	# key CODE…: press and release, holding the ones before the last.
	keys() {
		local k
		for k in "$@"; do vin key "$k" 1; done
		for k in $(printf '%s\n' "$@" | tac); do vin key "$k" 0; done
		sleep 0.03
	}

	# content <mwc> <app> <x|y>: where a window's content starts. mwc's
	# x and y are the frame's corner: the border is bw, the frame above
	# the content fh - h - bw high.
	content() {
		local x=$(state $1 view $3 "$2") bw=$(state $1 view bw "$2")
		if [ "$3" = x ]; then
			echo $((x + bw))
		else
			local fh=$(state $1 view fh "$2") h=$(state $1 view h "$2")
			echo $((x + fh - h - bw))
		fi
	}

	# The client's window in B, the only one there.
	bx=$(content b '*' x); by=$(content b '*' y)
	[ -n "$bx" ] || fail "no client window in B"

	# The left end of A's small terminal: the clock's frame covers the rest.
	tx=$(content a typer x); ty=$(content a typer y)
	th=$(state a view h typer)
	[ -n "$tx" ] || fail "no typer window in A"
	cx=$((tx + 10)); cy=$((ty + th / 2))

	vin move $((bx + cx)) $((by + cy))
	ok=
	for ((i = 0; i < 25; i++)); do
		sleep 0.2
		ax=$(state a cursor x); ay=$(state a cursor y)
		if [ $((ax - cx)) -ge -1 ] && [ $((ax - cx)) -le 1 ] \
				&& [ $((ay - cy)) -ge -1 ] && [ $((ay - cy)) -le 1 ]; then
			ok=1; break
		fi
	done
	[ "$ok" ] || fail "pointer in A at $ax,$ay, wanted $cx,$cy"
	echo "ok: the pointer moves ($ax,$ay)"

	vin button 272 1
	vin button 272 0
	for ((i = 0; i < 25; i++)); do
		sleep 0.2
		[ "$(state a view kbfocus typer)" = 1 ] && break
	done
	[ "$(state a view kbfocus typer)" = 1 ] || fail "the click didn't focus the terminal in A"
	echo "ok: a click focuses the terminal"

	# Physical keys; wayrdp -k de makes German of them:
	# H a l l o  W e l t  ä ö ü ß  AltGr+Q AltGr+E  Shift+7  Y Z  Return
	sleep 0.3
	SHIFT=42 ALTGR=100
	keys $SHIFT 35; keys 30; keys 38; keys 38; keys 24; keys 57
	keys $SHIFT 17; keys 18; keys 38; keys 20; keys 57
	keys 40; keys 39; keys 26; keys 12
	keys $ALTGR 16; keys $ALTGR 18; keys $SHIFT 8
	keys 21; keys 44
	# Caps Lock on, a, Caps Lock off, b
	keys 58; keys 30; keys 58; keys 48; keys 28
	want="Hallo Welt äöüß@€/zyAb"
	for ((i = 0; i < 25; i++)); do
		sleep 0.2
		[ "$(cat "$dir/typed" 2>/dev/null)" = "$want" ] && break
	done
	got=$(cat "$dir/typed" 2>/dev/null)
	[ "$got" = "$want" ] || fail "typed '$got', wanted '$want'"
	echo "ok: typing arrives with the German layout: $got"

	# One notch down: RDP says negative (towards the user), Wayland
	# positive; a notch is 120.
	vin wheel 1
	for ((i = 0; i < 25; i++)); do
		sleep 0.2
		grep -qE "Wheel vertical -[0-9]+" "$dir/wayrdp.log" && break
	done
	if grep -q "^DEBUG" "$dir/wayrdp.log"; then
		wheel=$(grep -oE "Wheel vertical -[0-9]+" "$dir/wayrdp.log" | tr '\n' ' ')
		[ "$wheel" = "Wheel vertical -120 " ] || fail "one notch gave '$wheel'"
		echo "ok: one notch arrives as $wheel"
	fi

	eval "exec ${VIN[1]}>&-"

fi

# ---- clipboard ------------------------------------------------------------
# With xfreerdp3 in a third mwc, C, whose login replaces B's session:
# wlfreerdp3's clipboard turns text into \u escapes.

n_active=$(grep -c "active, 1024x768" "$dir/wayrdp.log")
n_clip=$(grep -c "clipboard channel open" "$dir/wayrdp.log")
start_mwc c 1400x1000 '#000000' \
	"xfreerdp3 /v:127.0.0.1:$PORT /u:$USER_NAME /p:$PASSWORD /cert:ignore /size:1024x768 /log-level:WARN >$dir/client-c.log 2>&1"
for ((i = 0; i < 150; i++)); do
	[ "$(grep -c "active, 1024x768" "$dir/wayrdp.log")" -gt $n_active ] && break
	sleep 0.2
done
[ "$(grep -c "active, 1024x768" "$dir/wayrdp.log")" -gt $n_active ] || fail "the client in C never became active"
for ((i = 0; i < 50; i++)); do
	[ "$(grep -c "clipboard channel open" "$dir/wayrdp.log")" -gt $n_clip ] && break
	sleep 0.2
done
[ "$(grep -c "clipboard channel open" "$dir/wayrdp.log")" -gt $n_clip ] || fail "no clipboard channel for C"
echo "ok: C's client replaces B's"
expect_desktop c 1024 768 40 || fail "no magenta desktop in C's client (${share:-0} %)"
echo "ok: C's client shows the desktop (${share} % magenta)"

# wl_copy <mwc> <text>, wl_paste <mwc>
wl_copy() { XDG_RUNTIME_DIR=$dir/$1/run WAYLAND_DISPLAY=wayland-0 wl-copy -- "$2"; }
wl_paste() { XDG_RUNTIME_DIR=$dir/$1/run WAYLAND_DISPLAY=wayland-0 \
	timeout 10 wl-paste -n 2>/dev/null; }
# expect_paste <mwc> <text> <what>
expect_paste() {
	local got=
	for ((i = 0; i < 25; i++)); do
		got=$(wl_paste $1)
		[ "$got" = "$2" ] && break
		sleep 0.2
	done
	[ "$got" = "$2" ] || fail "$3: pasted '$got', wanted '$2'"
	echo "ok: $3"
}

text_a="Grüße aus A – 😀 €"
wl_copy a "$text_a"
expect_paste c "$text_a" "copied in the session, pasted on the client"

text_c="Hallo von C: äöü ß @ – 日本"
wl_copy c "$text_c"
expect_paste a "$text_c" "copied on the client, pasted in the session"

# Several lines: LF in the session, CRLF on the wire, LF again.
lines=$'Zeile 1\nZeile 2\n\nZeile 4'
wl_copy a "$lines"
expect_paste c "$lines" "several lines from the session"
wl_copy c "$lines C"
expect_paste a "$lines C" "several lines from the client"

# No echo: one copy, one format list, not a loop.
n=$(grep -c "Telling the client: session clipboard has text" "$dir/wayrdp.log")
sleep 1
[ "$(grep -c "Telling the client: session clipboard has text" "$dir/wayrdp.log")" = "$n" ] \
	|| fail "the clipboard echoes"
echo "ok: no clipboard echo"

# The pointer image goes to the client (an arrow at least), unless it is
# drawn into the picture (-r).
case " ${WAYRDP_OPTS:-} " in *" -r "*) ;; *)
grep -q "Pointer [0-9]*x[0-9]*, hotspot" "$dir/wayrdp.log" || fail "no pointer image sent"
echo "ok: pointer image sent ($(grep -o 'Pointer [0-9]*x[0-9]*, hotspot [0-9,]*' "$dir/wayrdp.log" | sort -u | tr '\n' ';'))"
;; esac

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

# ---- the size follows the client's window (display control) ----------------
if [ "$OUTPUT_MANAGEMENT" = 1 ]; then
	start_mwc d 1400x1000 '#000000' \
		"xfreerdp3 /v:127.0.0.1:$PORT /u:$USER_NAME /p:$PASSWORD /cert:ignore /size:901x700 /dynamic-resolution /log-level:WARN >$dir/client-d.log 2>&1"
	for ((i = 0; i < 150; i++)); do
		grep -q "active, 900x700" "$dir/wayrdp.log" && break
		sleep 0.2
	done
	grep -q "active, 900x700" "$dir/wayrdp.log" || fail "D never became active at 900x700"
	[ "$(state a output_HEADLESS-1 w)" = 900 ] && [ "$(state a output_HEADLESS-1 h)" = 700 ] \
		|| fail "A's output isn't 900x700"
	grep -q "desktop resized to 900x700" "$dir/wayrdp.log" || fail "D wasn't told the even width"
	echo "ok: D asks for 901x700, the output and D get 900x700 (even width)"

	for ((i = 0; i < 50; i++)); do
		grep -q "display control open" "$dir/wayrdp.log" && break
		sleep 0.2
	done
	grep -q "display control open" "$dir/wayrdp.log" || fail "no display control"
	sleep 1
	DISPLAY=$(sed -n 's/.*XWayland on DISPLAY=\(:[0-9]*\).*/\1/p' "$dir/mwc-d.log" | head -1) \
		xdotool search --name FreeRDP windowsize 1100 800 >/dev/null 2>&1
	for ((i = 0; i < 50; i++)); do
		grep -q "active, 1100x800" "$dir/wayrdp.log" && break
		sleep 0.2
	done
	grep -q "active, 1100x800" "$dir/wayrdp.log" || fail "the output didn't follow D's window to 1100x800"
	[ "$(state a output_HEADLESS-1 w)" = 1100 ] || fail "A's output isn't 1100 wide"
	echo "ok: the output follows D's window, 1100x800"
fi

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
