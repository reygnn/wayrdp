#!/bin/sh
# wayrdp -v prints "wayrdp VERSION" with the version from include/version.h
# and, where git knows the source, the commit built: +DATE.TIME.HASH in
# UTC, .dirty for changes not committed. Worked out here with git itself,
# not with util/gitversion.sh, which the build uses.
#
#   test/version.sh build/wayrdp [source dir]
wayrdp=$1
src=${2:-$(dirname "$0")/..}
version=$(sed -n 's/^#define WAYRDP_VERSION "\([^"]*\)".*/\1/p' "$src/include/version.h")
[ -n "$version" ] || { echo "FAIL: no WAYRDP_VERSION in include/version.h"; exit 1; }
if commit=$(cd "$src" && TZ=UTC0 git log -1 --format=%cd.%h \
		--date=format-local:%Y%m%d.%H%M 2>/dev/null) && [ -n "$commit" ]; then
	(cd "$src" && git diff --quiet HEAD) || commit=$commit.dirty
	version=$version+$commit
fi
got=$("$wayrdp" -v) || { echo "FAIL: wayrdp -v failed"; exit 1; }
[ "$got" = "wayrdp $version" ] || { echo "FAIL: wayrdp -v printed '$got', not 'wayrdp $version'"; exit 1; }
echo "ok: $got"
