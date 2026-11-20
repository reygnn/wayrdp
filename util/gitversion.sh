#!/bin/sh
# Prints the commit wayrdp is built from, for WAYRDP_GIT: +DATE.TIME.HASH,
# its commit time in UTC, and .dirty for changes not committed. Nothing
# without git. meson's vcs_tag runs it in the source directory.
v=$(TZ=UTC0 git log -1 --format=%cd.%h --date=format-local:%Y%m%d.%H%M 2>/dev/null) || exit 0
[ -n "$v" ] || exit 0
git diff --quiet HEAD 2>/dev/null || v=$v.dirty
printf '+%s\n' "$v"
