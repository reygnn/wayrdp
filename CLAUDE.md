# wayrdp

Project rules. The rules for all C projects, mwc as their reference, are in the global
CLAUDE.md.


## Build and test

Build and test inside the `wayrdp-dev` distrobox (`distrobox.ini`); the host lacks the
test tools. `distrobox enter wayrdp-dev -- bash -lc 'cd ~/data/C-Projects/wayrdp && ...'`.

The build is meson; the Makefile only wraps it (`make`, `make test`, `make e2e MWC=…`,
`make e2e-<script>`, `make install`, `make uninstall`, `make clean`, `make distclean`;
release with `--sysconfdir=/etc` by default; see its header):

| Command | |
|---|---|
| `meson setup build && ninja -C build` | wayrdp, `build/test/vinput` |
| `meson test -C build` | unit tests in `test/unit/` |
| `MWC=… test/mwc-rdp.sh` | end to end against a headless mwc, `-a nla` |
| `MWC=… PAM_PASSWORD=… test/pam-rdp.sh` | end to end, `-a pam`; needs `/etc/pam.d/wayrdp` |
| `MWC=… test/pam-hangup.sh` | `-a pam`, clients that hang up during the check; needs `/etc/pam.d/wayrdp`, no password |
| `MWC=… test/mwc-probe.sh` | capture from mwc, no client |
| `MWC=… test/clipboard-shutdown.sh` | exit with a clipboard read waiting, under valgrind |
| `MWC=… test/stuck-client.sh` | a client that stops reading |
| `MWC=… test/unplug-output.sh` | the captured output is unplugged |

Each script's header lists what it checks and its variables (`CLIENT`, `EXPECT_CODEC`,
`WAYRDP_WRAP`, …).

`MWC` is an mwc built from a copy, not inside the mwc repository:
`git -C ../mwc archive HEAD | tar -x -C <dir>`, then `make mwc` there in the box. The
tests need mwc d48c2b9 or later (`wp_fifo_v1`).


## Rules

- No workarounds for wlroots or FreeRDP bugs fixed upstream; wayrdp waits for the
  release. Bugs in the test clients (sdl-freerdp3, wlfreerdp3) are noted in TODO, not
  worked around in wayrdp.
- Nothing else may use wlr-output-management beside wayrdp (wlroots 0.19 and 0.20
  abort in `head_send_state` with a second client); tests and docs must not suggest
  wlr-randr, kanshi or wdisplays for a running session.
- The `WAYRDP_` test switches stay test-only; wayrdp warns at its start when one is on.
- TODO is in German, as it is; README, CAVEATS and the man page `wayrdp.scd` in English.
- Code taken from wayvnc keeps its ISC header.
