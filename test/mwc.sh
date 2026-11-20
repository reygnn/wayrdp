# Sourced by the end-to-end scripts: the mwc they run, and a refusal of an
# old one. wayrdp is tested against the newest mwc: it must be built from
# main in the mwc repository or from a commit after it. mwc -v names the
# commit (0.5.0+DATE.TIME.HASH, from mwc d43bc95 on); an mwc that doesn't,
# or one built from an older commit, fails the test.
#
# MWC       the mwc to run (default: the one installed on the host,
#           /run/host/usr/local/bin/mwc in the box, else mwc on PATH)
# MWC_REPO  the mwc repository (default: mwc next to this repository)
MWC=${MWC:-$(command -v /run/host/usr/local/bin/mwc || echo mwc)}
MWC_REPO=${MWC_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/mwc}
mwc_have=$("$MWC" -v 2>/dev/null | awk '$1 == "mwc" { print $2 }')
[ -n "$mwc_have" ] || { echo "FAIL: no mwc at $MWC (set MWC=/path/to/mwc)"; exit 1; }
mwc_main=$(git -C "$MWC_REPO" rev-parse --verify -q main 2>/dev/null)
[ -n "$mwc_main" ] || { echo "FAIL: no mwc repository with main at $MWC_REPO (set MWC_REPO)"; exit 1; }
# 0.5.0+20261120.1909.d43bc95[.dirty]: the hash is the last field but .dirty.
mwc_hash=$(printf '%s\n' "$mwc_have" | sed -n 's/^[^+]*+[0-9]*\.[0-9]*\.\([0-9a-f]*\)\(\.dirty\)\{0,1\}$/\1/p')
[ -n "$mwc_hash" ] || {
	echo "FAIL: $MWC is mwc $mwc_have, without the commit it was built from: install the newest mwc"
	exit 1
}
git -C "$MWC_REPO" cat-file -e "$mwc_hash^{commit}" 2>/dev/null || {
	echo "FAIL: $MWC is mwc $mwc_have, from a commit $MWC_REPO doesn't have (git fetch there?)"
	exit 1
}
git -C "$MWC_REPO" merge-base --is-ancestor "$mwc_main" "$mwc_hash" 2>/dev/null || {
	echo "FAIL: $MWC is mwc $mwc_have, older than main in $MWC_REPO ($(git -C "$MWC_REPO" log -1 --format=%h main)): install the newest mwc"
	exit 1
}
