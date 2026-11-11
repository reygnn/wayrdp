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

All milestones of the design are implemented and tested end to end against
FreeRDP's clients (xfreerdp3, sdl-freerdp3) in a headless mwc. **Not yet
tested with mstsc**, which is gate 1 of the design: does Windows' Remote
Desktop take RemoteFX over surface bits smoothly? It is tested at home until
it runs fully stable; only then does it go on the server, straight on 3389 in
place of xrdp and wayvnc.

| Milestone | Content | State |
|---|---|---|
| M0 | Fork, neatvnc out, `wv_log()`, builds against FreeRDP 3, connects to mwc | done |
| M1 | Listener, peer, TLS, NLA, RemoteFX, flow control | done, mstsc test open |
| M2 | Keyboard, mouse, wheel, Synchronize, Unicode | done, mstsc test open |
| M3 | Clipboard in both directions | done, mstsc test open |
| M4 | Size follows the mstsc window, pointer images | done, mstsc test open |
| M5 | Hardening, `pam` mode, docs | done |

## What it does

- **Login:** NLA (CredSSP/NTLM) against `~/.config/wayrdp/sam`, written by
  `wayrdp --set-password` – or, with `-a pam`, the Linux password over TLS,
  checked with PAM (see [Login](#login)). Only the user wayrdp runs as gets
  in.
- **TLS:** a self-signed certificate is made on the first start
  (`~/.config/wayrdp/tls.crt`, `tls.key`, mode 0600). Its SHA-1 thumbprint is
  in the log, to compare with what mstsc shows.
- **One session:** a new login replaces the old connection; a failed one
  throws no one out. The capture runs only while a client looks (not while
  mstsc is minimised).
- **Picture:** RemoteFX in 64×64 tiles, only the damaged ones, encoded in a
  worker thread; at most two frames unacknowledged (clients that never
  acknowledge are detected and served without waiting). Planar bitmaps for
  clients without RemoteFX.
- **Keyboard:** the client sends physical scancodes; wayrdp turns them into
  Linux key codes, and the layout on the server (`-k de`) says what they
  mean. Pause, AltGr, the numeric keypad and the media keys are in the
  table. Unicode events (on-screen keyboard, IME) are typed as keysyms;
  characters the layout lacks are dropped.
- **Synchronize:** when the mstsc window gets the focus back, every key still
  held is released (nothing sticks after Alt-Tab on the notebook) and Caps,
  Num and Scroll Lock are set to the notebook's.
- **Mouse:** absolute position on the output, left/right/middle and the two
  side buttons, vertical and horizontal wheel in 120ths of a notch (high
  resolution wheels scroll smoothly). Keys and buttons are let go when the
  client leaves or is replaced.
- **Clipboard:** text as `CF_UNICODETEXT` (UTF-16, CRLF) on the RDP side and
  UTF-8 with LF in the session, so umlauts, `€` and emoji survive both ways.
  Nothing is copied ahead: a copy on one side only announces text, the other
  side fetches it when something is pasted. Wayland programs and, through
  Xwayland, X programs. The primary selection (middle click) stays in the
  session; RDP has none. At most 16 MiB; a side that doesn't answer within
  5 s gets nothing. No images or files.
- **Size:** with `Mwm*outputManagement: true` in mwmrc the output takes the
  size the client asks for when it connects, and follows its window
  afterwards (display control: mstsc sends a new layout whenever its window
  changes; wayrdp waits 200 ms for it to settle). Sizes stay within
  200–8192 a side, the width even, at most 8192×4320 pixels; the client is
  told when its size had to change. Without output management (or if mwc
  refuses a size) the client gets the output's size (`outputMode`).
- **Pointer:** the pointer image comes from ext-image-copy-capture's cursor
  session and goes to the client as a 32-bit color pointer with alpha (up to
  384×384 where the client takes large pointers); the client draws it at its
  own mouse position, without the delay of a round trip. Images it has seen
  come from its pointer cache. `-r` draws the pointer into the picture
  instead, and so does wayrdp when the compositor has no cursor session.

Measured at 1920×1080 on an 8-core machine: idle 0 %, a terminal with a clock
23 % of one core at 9 fps, a full-screen terminal scrolling without pause 88 %
of one core at 30 fps, 2.2 MB/s, 8 ms encoding per frame.

## Setting it up on the server

This follows mwc's `docs/WAYLAND-RDP.md`: mwc runs headless as a user
service, reached through a Bitvise SSH tunnel. wayrdp takes RDP's port 3389
in place of xrdp and wayvnc; there is no phase next to them, so it goes on the
server only once it runs stable. mwc's `docs/wayland-rdp-upgrade.sh` does
these steps.

### 1. Build and install

Ubuntu 26.04 has everything (FreeRDP 3.32, wlroots 0.19 for mwc):

```sh
sudo apt install --no-install-recommends build-essential meson ninja-build \
    pkg-config git freerdp3-dev libwinpr3-dev libwayland-dev \
    wayland-protocols libxkbcommon-dev libpixman-1-dev libdrm-dev libpam0g-dev
git clone https://github.com/reygnn/wayrdp.git
cd wayrdp
meson setup build --buildtype=release --sysconfdir=/etc
ninja -C build
sudo ninja -C build install      # /usr/local/bin/wayrdp, /etc/pam.d/wayrdp
```

`--sysconfdir=/etc` puts the PAM service where PAM looks; meson would
otherwise use `/usr/local/etc`. aml 1.0 is not packaged; meson fetches it as a
subproject (`subprojects/aml.wrap`).

### 1a. xrdp and wayvnc out

Only once wayrdp has built, so a failed build leaves the old way in place.
Nothing else may listen on 3389 afterwards (`ss -ltn | grep 3389`).

```sh
systemctl --user disable --now wayvnc
sudo systemctl disable --now xrdp xrdp-sesman
sudo apt purge xrdp wayvnc
```

### 2. mwc

`~/.config/systemd/user/mwc.service` needs `MWC_VIRTUAL_INPUT=1` (it is
already there for wayvnc). For the size to follow the mstsc window, add to
`~/.config/mwc/mwmrc` and restart mwc (it reads this at its start):

```
Mwm*outputManagement: true
```

**Then nothing else may use wlr-output-management** – no wlr-randr, kanshi
or wdisplays: wlroots 0.19 and 0.20 abort in `head_send_state` when a second
client of the protocol is around, and take the whole mwc session with them
(fixed only on wlroots master, 40640950). That is why mwc offers the protocol
only when asked.

### 3. Password and service

```sh
wayrdp --set-password                # the RDP password, see Login
mkdir -p ~/.config/systemd/user
cp contrib/wayrdp.service ~/.config/systemd/user/
# adjust -k de to your notebook's layout
systemctl --user daemon-reload
systemctl --user enable --now wayrdp
journalctl --user -u wayrdp          # "Listening on 127.0.0.1:3389", the thumbprint
```

### 4. Connect through Bitvise

Bitvise's RDP tab works as it did with xrdp, no rule of its own. Log in with
your Linux user name and the RDP password. mstsc warns about the certificate
on the first connection: compare the thumbprint with the log and accept it
permanently. For the clipboard, tick it under *Local Resources*.

## Login

| `-a` | How | For | Against |
|---|---|---|---|
| `nla` (default) | CredSSP/NTLM against the SAM file from `--set-password` | mstsc's default; no login screen needed | a password of its own; the file holds an unsalted NT hash |
| `pam` | TLS only; the credentials mstsc sends along are checked with PAM (service `wayrdp`) | the Linux password, as with xrdp | mstsc warns about the missing NLA; it must have the credentials saved, or the connection is refused: wayrdp has no login screen |

For `-a pam`, remove `NoNewPrivileges=yes` from the service: PAM checks the
password with the setgid helper `unix_chkpwd`.

Either way, three failed logins in a row lock new connections out for 30 s,
at most four connections may wait for their logon at a time, and one that
hasn't logged on after 30 s is closed.

## Security

- wayrdp listens on localhost only unless told otherwise (and warns then):
  the SSH tunnel is the first lock, the RDP login the second.
- It refuses to start when the SAM file or the TLS key can be read by others
  (like ssh).
- FreeRDP parses what comes from the network in wayrdp's process, not in mwc.
- `MWC_VIRTUAL_INPUT=1` lets every program in the session type and click, as
  with wayvnc; the capture protocols and output management are open to every
  program of the session too (mwc has no security-context-v1).
- **The systemd sandbox is thin in a user service on Ubuntu.** What holds are
  the seccomp and prctl options in `contrib/wayrdp.service` (address
  families, `NoNewPrivileges`, `LockPersonality`, `RestrictNamespaces`, …).
  `ProtectSystem`, `PrivateTmp` and `ReadWritePaths` need a mount namespace,
  which a user service doesn't get where AppArmor restricts unprivileged user
  namespaces (Ubuntu since 23.10); systemd then skips them without a word.
  Checked on Ubuntu 26.04: with them set, the service could still write to
  the home directory. They stay in the file for systems where they work.

## Building and tests

The development box (rootless Podman + Distrobox) brings everything the tests
need too:

```sh
distrobox assemble create --replace --file distrobox.ini
distrobox enter wayrdp-dev
meson setup build && ninja -C build
meson test -C build                     # unit tests
MWC=/path/to/mwc test/mwc-probe.sh      # capture from mwc, no client
MWC=/path/to/mwc test/mwc-rdp.sh        # end to end, -a nla
MWC=/path/to/mwc PAM_PASSWORD=… test/pam-rdp.sh   # end to end, -a pam
```

The unit tests pin the scancode table, Pause, the wheel, UTF-8/UTF-16 with
CRLF, the size limits and the pointer conversion.

`test/mwc-rdp.sh` starts mwc headless with a magenta desktop and two
terminals, serves it with wayrdp, and checks:

- a SAM file others can read is refused
- NLA with the right and a wrong password (`xfreerdp3 +auth-only`)
- a second mwc runs sdl-freerdp3 (with `SDL_VIDEO_DRIVER=wayland`: without
  `wp_fifo_v1` in mwc, SDL3 would take Xwayland and lose the first wheel
  event), and a screenshot of it shows the magenta desktop
- `build/test/vinput` moves and clicks in the second mwc, and types physical
  keys there: A's pointer lands on the pixel, the click focuses a terminal in
  A, and what it gets is "Hallo Welt äöüß@€/zyAb" with `-k de` (umlauts,
  AltGr, Shift, Y/Z, Caps Lock); one wheel notch arrives as 120
- a third mwc runs xfreerdp3 and takes the session over; `wl-copy` on one
  side, `wl-paste` on the other, both ways, with umlauts, emoji, CJK and
  several lines, and no echo
- a pointer image goes out
- the client follows when the output changes size
- a fourth mwc runs `xfreerdp3 /dynamic-resolution` asking for 901×700: the
  output and the client get 900×700; `xdotool windowsize` makes the
  client's window 1100×800 and the output follows
- a second login replaces the session
- three wrong passwords lock the next connection out

Variants: `OUTPUT_MANAGEMENT=0` (mwc without it: the client gets the
output's size), `CLIENT_SIZE=800x600`, `WAYRDP_OPTS=-r` (pointer in the
picture), `CLIENT=xfreerdp3` (through Xwayland; no input part, it passes the
pointer on once and then no more), `CLIENT=wlfreerdp3` (deprecated; it now
and then hangs once pointer images come, and its clipboard turns text into
`\u` escapes), `WAYRDP_FORCE_PLANAR=1 EXPECT_CODEC=Planar` (the fallback;
the FreeRDP clients always offer RemoteFX), `WAYRDP_WRAP="valgrind …"`.

`test/pam-rdp.sh` needs `/etc/pam.d/wayrdp` and the password of the user
running it: the right password gets the picture, a wrong and an empty one
are refused, a silent connection is closed after the login timeout
(`WAYRDP_LOGIN_TIMEOUT=3`), the three failures lock new connections out, a
fifth connection waiting for its logon is refused, and the password is not
in the log.

## Where it differs from the design

- **Scancodes:** an own table (`src/rdp/keymap.c`) instead of
  `freerdp_keyboard_get_x11_keycode_from_rdp_scancode()`, which is
  deprecated since FreeRDP 3.11 and only filled by the X11 keyboard setup.
- **RemoteFX:** tiles go in groups through `rfx_encode_message()`, not
  `rfx_encode_messages()`, whose split leaks the message array in FreeRDP
  3.32 (about 300 bytes per frame).
- **data-control.c** is rewritten: wayvnc's read every selection at once,
  the primary one too.
- **drdynvc** is opened only after the connection sequence: FreeRDP sets it
  up once, and too early its first message lands in the licensing phase.
- **The pointer** goes out only after the first picture of an activation.
- **systemd:** see Security; the file-system options don't hold in a user
  service on Ubuntu.
- **Hardening beyond the design:** the login timeout, the limit of waiting
  connections and the check of the secrets' modes.

## License

ISC, like wayvnc. See [COPYING](COPYING).
