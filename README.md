# wayrdp

An RDP server for wlroots-based Wayland compositors, made for
[mwc](https://github.com/reygnn/mwc) on a headless server: Windows' Remote
Desktop (`mstsc`) connects through an SSH tunnel straight to wayrdp, without
xrdp and wayvnc in between.

wayrdp is a fork of [wayvnc](https://github.com/any1/wayvnc) 0.10.2. The
Wayland side (capture, virtual keyboard and pointer, clipboard, output
management) is wayvnc's; the VNC side (neatvnc) is replaced by FreeRDP 3. The
design, milestones and open questions are in
[docs/wayrdp-Entwurf.md](docs/wayrdp-Entwurf.md).

## Status

**M0 – fork and build.** neatvnc is gone, wayrdp builds against FreeRDP 3 and
connects to mwc: it finds the output, captures frames and hands them to the
RDP side. There is no RDP listener yet, so `mstsc` can't connect. That is M1.

| Milestone | Content | State |
|---|---|---|
| M0 | Fork, neatvnc out, `wv_log()`, builds against FreeRDP 3, connects to mwc | done |
| M1 | Listener, peer, TLS, NLA, RemoteFX, flow control | |
| M2 | Keyboard, mouse, wheel, Synchronize, Unicode | |
| M3 | Clipboard in both directions | |
| M4 | Size follows the mstsc window, pointer images | |
| M5 | Hardening, `pam` mode, docs; replaces xrdp and wayvnc | |

## Building

Ubuntu 26.04 has everything (FreeRDP 3.32, wlroots 0.19 for mwc). The
development box brings the packages:

```sh
distrobox assemble create --replace --file distrobox.ini
distrobox enter wayrdp-dev
meson setup build
ninja -C build
```

aml 1.0 is not packaged; meson fetches it as a subproject
(`subprojects/aml.wrap`).

## Tests

```sh
meson test -C build                       # unit tests
MWC=/path/to/mwc test/mwc-probe.sh        # against mwc on a headless output
```

`test/mwc-probe.sh` starts mwc headless with a terminal that keeps drawing,
runs `wayrdp --probe=3` against it and checks size and frame count.
`--probe=<n>` captures n frames without a client, prints size, format, frame
count and damage, and exits.

## Running

mwc must offer the virtual input protocols:

```sh
MWC_VIRTUAL_INPUT=1 WLR_BACKENDS=headless WLR_RENDERER=pixman mwc &
wayrdp -k de -o HEADLESS-1 127.0.0.1 3390
```

Options and config keys are listed in `wayrdp -h` and in the design document.
The config file is `~/.config/wayrdp/config` (`key = value` lines).

## License

ISC, like wayvnc. See [COPYING](COPYING).
