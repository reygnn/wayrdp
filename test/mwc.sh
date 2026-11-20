# Sourced by the end-to-end scripts: the mwc they run, and a refusal of an
# old one. wayrdp is tested against the newest mwc, the version on main in
# the mwc repository; an older one installed fails the test.
#
# MWC       the mwc to run (default: the one installed on the host,
#           /run/host/usr/local/bin/mwc in the box, else mwc on PATH)
# MWC_REPO  the mwc repository (default: mwc next to this repository)
#
# Only the version is compared: commits after the last version bump on
# main are not seen.
MWC=${MWC:-$(command -v /run/host/usr/local/bin/mwc || echo mwc)}
MWC_REPO=${MWC_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/mwc}
mwc_have=$("$MWC" -v 2>/dev/null | awk '$1 == "mwc" { print $2 }')
mwc_want=$(git -C "$MWC_REPO" show main:src/version.h 2>/dev/null \
	| sed -n 's/.*MWC_VERSION "\(.*\)".*/\1/p')
[ -n "$mwc_have" ] || { echo "FAIL: no mwc at $MWC (set MWC=/path/to/mwc)"; exit 1; }
[ -n "$mwc_want" ] || { echo "FAIL: no mwc repository at $MWC_REPO (set MWC_REPO)"; exit 1; }
if [ "$(printf '%s\n' "$mwc_have" "$mwc_want" | sort -V | head -1)" != "$mwc_want" ]; then
	echo "FAIL: $MWC is mwc $mwc_have, main in $MWC_REPO is $mwc_want: install the newest mwc"
	exit 1
fi
