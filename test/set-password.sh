#!/bin/sh
# wayrdp --set-password from a pipe: a password is written to the SAM file
# with mode 0600; one too long for the line buffer (255 bytes and more) is
# refused, not cut short.
#
#   test/set-password.sh build/wayrdp
wayrdp=$1
dir=$(mktemp -d "${TMPDIR:-/tmp}/wayrdp-setpw.XXXXXX") || exit 1
trap 'rm -rf "$dir"' EXIT
sam=$dir/wayrdp/sam

echo "ein-passwort" | XDG_CONFIG_HOME=$dir "$wayrdp" --set-password >/dev/null 2>&1 \
	|| { echo "FAIL: a plain password was refused"; exit 1; }
[ "$(stat -c %a "$sam")" = 600 ] || { echo "FAIL: the SAM file isn't 0600"; exit 1; }
before=$(cat "$sam")
echo "ok: a password is set"

long=$(head -c 300 /dev/zero | tr '\0' x)
if echo "$long" | XDG_CONFIG_HOME=$dir "$wayrdp" --set-password >"$dir/out" 2>&1; then
	echo "FAIL: a 300-byte password was taken"
	exit 1
fi
grep -q "too long" "$dir/out" || { echo "FAIL: no reason given: $(cat "$dir/out")"; exit 1; }
[ "$(cat "$sam")" = "$before" ] || { echo "FAIL: the SAM file changed"; exit 1; }
echo "ok: a 300-byte password is refused"
