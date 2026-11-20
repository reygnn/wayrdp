#!/bin/bash
# -a pam against mwc: wrong guesses that try to get around the lockout,
# by hanging up while PAM still checks, or with other user names.
#
#   test/pam-hangup.sh [build/wayrdp]
#
# Needs /etc/pam.d/wayrdp, but not the password: every guess is wrong. It
# skips where PAM counts failed logins (pam_faillock, pam_tally), which
# six wrong passwords could lock the account with. Checks:
#  - three wrong passwords whose clients hung up before PAM answered still
#    lock new connections out
#  - checks that go on after their client left count toward the four that
#    may wait at a time, added to the connections waiting themselves
#    (pam_unix delays a failure by about two seconds, so they pile up)
#  - a user name that isn't the account is refused no sooner than a wrong
#    password, and three of them lock out
# WAYRDP_WRAP goes in front of wayrdp, e.g. "valgrind --log-file=/tmp/vg.log".
set -u

WAYRDP=${1:-build/wayrdp}
. "$(dirname "$0")/mwc.sh"
PORT=${PORT:-$((20000 + RANDOM % 10000))}
[ -e /etc/pam.d/wayrdp ] || { echo "SKIP: no /etc/pam.d/wayrdp"; exit 77; }
# Six wrong passwords for the user running the test: where PAM counts
# them, they could lock the account.
if grep -qsE 'pam_(faillock|tally2?)\.so' /etc/pam.d/wayrdp /etc/pam.d/common-auth \
		/etc/pam.d/common-account; then
	echo "SKIP: PAM counts failed logins here (pam_faillock or pam_tally)"
	exit 77
fi
for tool in sdl-freerdp3; do
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

start_mwc() {	# start_mwc <name> <command>
	local name=$1
	mkdir -p "$dir/$name/run" "$dir/$name/config/mwc"
	chmod 700 "$dir/$name/run"
	printf 'Mwm*HEADLESS-1*outputMode: 1024x768\n' >"$dir/$name/config/mwc/mwmrc"
	XDG_RUNTIME_DIR=$dir/$name/run XDG_CONFIG_HOME=$dir/$name/config \
	WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
	MWC_VIRTUAL_INPUT=1 "$MWC" -s "$2" >"$dir/mwc-$name.log" 2>&1 &
	pids+=($!)
	for ((i = 0; i < 50; i++)); do
		[ -S "$dir/$name/run/wayland-0" ] && return 0
		sleep 0.2
	done
	fail "mwc $name has no Wayland socket"
}

# count <pattern> [log]
count() { grep -c "$1" "${2:-$dir/wayrdp.log}"; }

# wait_count <pattern> <n> <what> [log]: until the log has n lines of it
wait_count() {
	for ((i = 0; i < 150; i++)); do
		[ "$(count "$1" "${4:-}")" -ge "$2" ] && return 0
		sleep 0.1
	done
	fail "$3"
}

start_wayrdp() {	# start_wayrdp <port> <log>
	XDG_RUNTIME_DIR=$dir/a/run WAYLAND_DISPLAY=wayland-0 XDG_CONFIG_HOME=$dir/cfg \
		${WAYRDP_WRAP:-} "$WAYRDP" -L debug -a pam 127.0.0.1 "$1" >"$2" 2>&1 &
	pids+=($!)
	wait_count "Listening on" 1 "wayrdp doesn't listen on $1" "$2"
}

USER_NAME=$(id -un)
# client <name> <port>: sdl-freerdp3 with a wrong password, in its own mwc
client() {
	start_mwc "$1" "sdl-freerdp3 /v:127.0.0.1:$2 /sec:tls /cert:ignore /size:1024x768 /u:$USER_NAME /p:wrong-$1 /t:$1 /log-level:WARN >$dir/client-$1.log 2>&1"
}
# hang_up <name>: the client goes, its socket closes (by its title: FreeRDP
# blanks the password in its argv)
hang_up() { pkill -KILL -f "^sdl-freerdp3 .*/t:$1 " 2>/dev/null; }

start_mwc a "foot sh -c 'while :; do date +%T; sleep 1; done'"
mkdir -p "$dir/cfg"
start_wayrdp "$PORT" "$dir/wayrdp.log"
echo "ok: wayrdp -a pam listens on $PORT"

# Three guesses, each client gone before PAM answers.
for n in 1 2 3; do
	sent=$(count "client wants")
	client h$n "$PORT"
	wait_count "client wants" $((sent + 1)) "client h$n sent no credentials"
	hang_up h$n
	# Counted when the answer is handled, after the time a refusal takes.
	wait_count "A client that is gone: wrong password" $n "PAM never answered for h$n"
	[ "$(count "127.0.0.1: wrong password for $USER_NAME")" = 0 ] \
		|| fail "client h$n was still there when PAM answered; the test needs it gone"
done
echo "ok: three clients hung up while PAM checked"

exec 3<>/dev/tcp/127.0.0.1/$PORT
wait_count "Connection refused: locked" 1 "no lockout after three guesses whose clients hung up"
exec 3>&-
echo "ok: their guesses count, locked out"

# Checks whose clients left still wait for the worker and count toward
# the four, beside connections that wait for their own. A fresh wayrdp,
# the one above is locked out. Three at once in one mwc, so all three
# checks are queued before the first is answered (the one worker takes
# about two seconds a check).
PORT2=$((PORT + 1))
start_wayrdp "$PORT2" "$dir/wayrdp2.log"
cmd=
for n in 1 2 3; do
	cmd+="sdl-freerdp3 /v:127.0.0.1:$PORT2 /sec:tls /cert:ignore /size:1024x768 /u:$USER_NAME /p:wrong-q$n /t:q$n /log-level:WARN >$dir/client-q$n.log 2>&1 & "
done
start_mwc q "sh -c '$cmd wait'"
wait_count "client wants" 3 "the three clients sent no credentials" "$dir/wayrdp2.log"
for n in 1 2 3; do hang_up q$n; done
# Gone for wayrdp too, not only killed: else they would count as waiting
# connections themselves.
wait_count "Connection from .* closed" 3 "wayrdp didn't see the three go" "$dir/wayrdp2.log"
# Three checks and one waiting connection make four: a fifth is refused.
exec 3<>/dev/tcp/127.0.0.1/$PORT2
for ((i = 0; i < 20; i++)); do
	[ "$(count "Connection from [0-9.]*$" "$dir/wayrdp2.log")" -ge 4 ] && break
	sleep 0.1
done
[ "$(count "Connection from [0-9.]*$" "$dir/wayrdp2.log")" -ge 4 ] \
	|| fail "the fourth connection wasn't accepted beside three checks"
# Before the fifth: with a check already answered (slow handshakes, or
# under valgrind) three would be left, and the fifth rightly accepted.
[ "$(count "PAM: Authentication failure" "$dir/wayrdp2.log")" = 0 ] \
	|| fail "a check was answered before the fifth came; the test needs all three waiting"
exec 4<>/dev/tcp/127.0.0.1/$PORT2
wait_count "Connection refused: 4 are logging on already" 1 \
	"a fifth connection wasn't refused beside three checks and one waiting" "$dir/wayrdp2.log"
exec 3>&- 4>&-
echo "ok: checks of clients that left count toward the four, beside waiting connections"
# A user name that isn't the account is a guess too: refused no sooner
# than a wrong password, so the time doesn't tell, and three of them lock
# out. A fresh wayrdp; the clients stay until they are refused.
PORT3=$((PORT + 2))
start_wayrdp "$PORT3" "$dir/wayrdp3.log"
for n in 1 2 3; do
	client_as() {
		start_mwc "$1" "sdl-freerdp3 /v:127.0.0.1:$PORT3 /sec:tls /cert:ignore /size:1024x768 /u:not-$USER_NAME-$n /p:wrong-$1 /t:$1 /log-level:WARN >$dir/client-$1.log 2>&1"
	}
	sent=$(count "client wants" "$dir/wayrdp3.log")
	client_as u$n
	wait_count "client wants" $((sent + 1)) "client u$n sent no credentials" "$dir/wayrdp3.log"
	t0=$(date +%s%N)
	wait_count "user not-$USER_NAME-$n refused" 1 "the user name of u$n wasn't refused" "$dir/wayrdp3.log"
	ms=$((($(date +%s%N) - t0) / 1000000))
	[ "$ms" -ge 1500 ] || fail "user name $n refused after $ms ms, sooner than a wrong password"
	hang_up u$n
done
exec 3<>/dev/tcp/127.0.0.1/$PORT3
wait_count "Connection refused: locked" 1 "no lockout after three wrong user names" "$dir/wayrdp3.log"
exec 3>&-
echo "ok: wrong user names take as long as wrong passwords and lock out"
echo "PASS"
