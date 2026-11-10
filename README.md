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

**M3 – clipboard.** wayrdp logs in with NLA, sends the desktop as RemoteFX
(Planar for clients without it), takes keyboard, mouse and wheel, and shares
the clipboard both ways with full Unicode. Next is M4: the size following
the mstsc window, and pointer images.

Tested against xfreerdp3 and wlfreerdp3 3.32 (see Tests). **Not yet tested
with mstsc**, which is gate 1 of the design: does Windows' Remote Desktop take
RemoteFX over surface bits smoothly?

| Milestone | Content | State |
|---|---|---|
| M0 | Fork, neatvnc out, `wv_log()`, builds against FreeRDP 3, connects to mwc | done |
| M1 | Listener, peer, TLS, NLA, RemoteFX, flow control | done, mstsc test open |
| M2 | Keyboard, mouse, wheel, Synchronize, Unicode | done, mstsc test open |
| M3 | Clipboard in both directions | done, mstsc test open |
| M4 | Size follows the mstsc window, pointer images | |
| M5 | Hardening, `pam` mode, docs; replaces xrdp and wayvnc | |

What M1 to M3 do:

- **Login:** NLA (CredSSP/NTLM) against `~/.config/wayrdp/sam`, written by
  `wayrdp --set-password`. Only the user wayrdp runs as gets in. Three failed
  logins in a row lock new connections out for 30 s.
- **TLS:** a self-signed certificate is made on the first start
  (`~/.config/wayrdp/tls.crt`, `tls.key`, mode 0600). Its SHA-1 thumbprint is
  in the log, to compare with what mstsc shows.
- **One session:** a new login replaces the old connection; a failed one
  throws no one out. The capture runs only while a client looks (not while
  mstsc is minimised).
- **Size:** the desktop has the size of mwc's output (`outputMode` in mwmrc).
  A client that asks for another size is told that one; when the output
  changes size, the client follows. The size following the mstsc window is
  M4.
- **Picture:** RemoteFX in 64×64 tiles, only the damaged ones, encoded in a
  worker thread; at most two frames unacknowledged (clients that never
  acknowledge are detected and served without waiting).
- **Keyboard:** the client sends physical scancodes; wayrdp turns them into
  Linux key codes (its own table in `src/rdp/keymap.c`: FreeRDP's
  `freerdp_keyboard_get_x11_keycode_from_rdp_scancode()` is deprecated and
  needs an X11 keyboard set up), and the layout on the server (`-k de`) says
  what they mean. Pause, AltGr, the numeric keypad and the media keys are in
  the table. Unicode events (on-screen keyboard, IME) are typed as keysyms;
  characters the layout lacks are dropped.
- **Synchronize:** when the mstsc window gets the focus back, every key still
  held is released (nothing sticks after Alt-Tab on the notebook) and Caps,
  Num and Scroll Lock are set to the notebook's.
- **Mouse:** absolute position on the output, left/right/middle and the two
  side buttons, vertical and horizontal wheel in 120ths of a notch (high
  resolution wheels scroll smoothly). Keys and buttons are let go when the
  client leaves or is replaced.
- **Clipboard:** text as `CF_UNICODETEXT` (UTF-16, CRLF) on the RDP side and
  UTF-8 with LF in the session, so umlauts, `€` and emoji survive both
  ways. Nothing is copied ahead: a copy on one side only announces text,
  the other side fetches it when something is pasted. Through
  ext-data-control (or wlr-data-control), so it works with Wayland and,
  through Xwayland, X programs. The primary selection (middle click) stays
  in the session; RDP has none. At most 16 MiB; a side that doesn't answer
  within 5 s gets nothing. Images and files are not part of v1.

Measured at 1920×1080 on an 8-core machine: idle 0 %, a terminal with a clock
23 % of one core at 9 fps, a full-screen terminal scrolling without pause 88 %
of one core at 30 fps, 2.2 MB/s, 8 ms encoding per frame.

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
meson test -C build                     # unit tests
MWC=/path/to/mwc test/mwc-probe.sh      # M0: capture from mwc, no client
MWC=/path/to/mwc test/mwc-rdp.sh        # M1: end to end with xfreerdp3
```

`test/mwc-rdp.sh` starts mwc headless with a magenta desktop and two
terminals, serves it with wayrdp, and checks:

- NLA with the right and a wrong password (`xfreerdp3 +auth-only`)
- a second mwc runs wlfreerdp3, and a screenshot of it shows the magenta
  desktop
- `build/test/vinput` moves and clicks in the second mwc, and types physical
  keys there: A's pointer lands on the pixel, the click focuses a terminal in
  A, and what it gets is "Hallo Welt äöüß@€/zyAb" with `-k de` (umlauts,
  AltGr, Shift, Y/Z, Caps Lock); a wheel notch arrives
- a third mwc runs xfreerdp3 and takes the session over; `wl-copy` on one
  side, `wl-paste` on the other, both ways, with umlauts, emoji, CJK and
  several lines, and no echo (wlfreerdp3's clipboard turns text into
  `\u` escapes, so it is xfreerdp3 here)
- the client follows when the output changes size
- a second login replaces the session
- three wrong passwords lock the next connection out

Variants: `CLIENT=xfreerdp3` (through Xwayland; picture only, it passes the
pointer on once and then no more), `CLIENT_SIZE=800x600` (the client is
resized to the output),
`WAYRDP_FORCE_PLANAR=1 EXPECT_CODEC=Planar` (the fallback; xfreerdp3 always
offers RemoteFX), `WAYRDP_WRAP="valgrind --log-file=…"`.

## Running

```sh
wayrdp --set-password                   # once: the RDP password
MWC_VIRTUAL_INPUT=1 WLR_BACKENDS=headless WLR_RENDERER=pixman mwc &
wayrdp -k de -o HEADLESS-1 127.0.0.1 3390
```

mwc must run with `MWC_VIRTUAL_INPUT=1`. As a user service next to mwc:
[contrib/wayrdp.service](contrib/wayrdp.service), on port 3390 while xrdp
still has 3389.

**With Bitvise:** a C2S rule `127.0.0.1:13390 → localhost:3390`, then
`mstsc /v:127.0.0.1:13390`. Log in with your Linux user name and the RDP
password. mstsc warns about the certificate on the first connection; compare
the thumbprint with wayrdp's log (`journalctl --user -u wayrdp`) and accept it
permanently.

The SAM file holds the unsalted NT hash of the RDP password, which is why it
should be a password of its own and the file stays 0600. wayrdp listens on
localhost only unless told otherwise, and warns then.

Options and config keys: `wayrdp -h`, `man wayrdp`, and the design document.
The config file is `~/.config/wayrdp/config` (`key = value` lines).

## License

ISC, like wayvnc. See [COPYING](COPYING).
