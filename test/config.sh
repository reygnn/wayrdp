#!/bin/sh
# wayrdp's configuration and options before it connects anywhere (no
# compositor: WAYLAND_DISPLAY names none):
#  - without HOME and XDG_CONFIG_HOME there is no config file, which is
#    fine, and no config directory, which is not: no "Success" error, and
#    no sam, tls.crt or tls.key in the current directory
#  - a port or frame rate out of range in the config file is refused, as
#    on the command line; -f takes a number only
#
#   test/config.sh build/wayrdp
wayrdp=$(realpath "$1")
dir=$(mktemp -d "${TMPDIR:-/tmp}/wayrdp-config.XXXXXX") || exit 1
trap 'rm -rf "$dir"' EXIT
mkdir -p "$dir/run" "$dir/cfg/wayrdp" "$dir/cwd"
chmod 700 "$dir/run"
fail() { echo "FAIL: $1"; [ -f "$dir/out" ] && cat "$dir/out"; exit 1; }
# run [env…] -- [args…]: wayrdp's output in $dir/out, its exit code
run() {
	(cd "$dir/cwd" && XDG_RUNTIME_DIR="$dir/run" WAYLAND_DISPLAY=none env "$@" \
		>"$dir/out" 2>&1)
}

run -u HOME -u XDG_CONFIG_HOME "$wayrdp" && fail "started without a config directory"
grep -q "Success" "$dir/out" && fail "an error that says Success"
grep -q "No config directory" "$dir/out" || fail "no reason given"
ls "$dir/cwd" | grep -q . && fail "files in the current directory: $(ls "$dir/cwd")"
echo "ok: no HOME: no config directory, refused, nothing in the current directory"

run -u HOME -u XDG_CONFIG_HOME "$wayrdp" --set-password </dev/null && fail "--set-password without a config directory"
ls "$dir/cwd" | grep -q . && fail "--set-password wrote to the current directory"
echo "ok: no HOME: --set-password refused"

# expect <config line> <message> [options…]
expect() {
	printf '%s\n' "$1" >"$dir/cfg/wayrdp/config"
	local line=$1 msg=$2
	shift 2
	run XDG_CONFIG_HOME="$dir/cfg" "$wayrdp" "$@"
	grep -q "$msg" "$dir/out" || fail "'$line' $*: no '$msg'"
}
expect "port=70000" "Invalid port"
expect "port=-1" "Invalid port"
expect "max_rate=5000" "Invalid frame rate"
expect "" "Invalid frame rate" -f 12abc
expect "" "Invalid frame rate" -f 0
printf 'port=3390\nmax_rate=60\n' >"$dir/cfg/wayrdp/config"
run XDG_CONFIG_HOME="$dir/cfg" "$wayrdp"
grep -q "Invalid" "$dir/out" && fail "a valid port and rate were refused"
echo "ok: port and frame rate checked, from the config file too"
