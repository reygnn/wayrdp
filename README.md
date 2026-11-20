# wayrdp

An RDP server for wlroots-based Wayland compositors, made for
[mwc](https://github.com/reygnn/mwc) on a headless server: Windows' Remote Desktop
(`mstsc`) connects through an SSH tunnel straight to wayrdp, without xrdp and wayvnc in
between.

wayrdp is a fork of [wayvnc](https://github.com/any1/wayvnc) 0.10.2. The Wayland side
(capture, virtual keyboard and pointer, clipboard, output management) is wayvnc's; the
VNC side (neatvnc) is replaced by FreeRDP 3. The design (in German) is in
[docs/wayrdp-Entwurf.md](docs/wayrdp-Entwurf.md), measurements and where the code
differs from it in [docs/notes.md](docs/notes.md), known limits in
[CAVEATS.md](CAVEATS.md), open work in [TODO](TODO).


## Status

Runs day to day at home: mstsc (a Windows 11 VM, through Bitvise) gets the picture over
the graphics pipeline (RemoteFX Progressive), and keyboard, mouse, clipboard both ways,
pointer images and reconnecting work; so do Remmina and FreeRDP's own clients. mstsc
takes the size it connects with but doesn't follow its window afterwards (see CAVEATS).

Next is the server at work, straight on 3389 in place of xrdp and wayvnc:
[docs/upgrade-to-wayrdp.md](docs/upgrade-to-wayrdp.md).


## What it does

- Login: NLA (CredSSP/NTLM) against `~/.config/wayrdp/sam`, written by
  `wayrdp --set-password`; or, with `-a pam`, the Linux password over TLS, checked with
  PAM (see [Login](#login)). Only the user wayrdp runs as gets in.
- TLS: a self-signed certificate is made on the first start (`~/.config/wayrdp/tls.crt`,
  `tls.key`, mode 0600). Its SHA-1 thumbprint is in the log, to compare with what mstsc
  shows.
- One session: a new login replaces the old connection; a failed one throws no one out.
  The capture runs only while a client looks (not while mstsc is minimised).
- Picture: over the graphics pipeline (MS-RDPEGFX 8.0 to 10.7) as RemoteFX Progressive;
  for clients without the pipeline, RemoteFX as surface bits, else Planar. 64×64 tiles,
  only the damaged ones, encoded in a worker thread, at most two frames unacknowledged.
  A client that stops reading gets no new frames and never blocks the main loop; a
  connection whose network is gone ends after 20 s.
- Keyboard: physical scancodes, turned into Linux key codes; the server's layout
  (`-k de`) says what they mean. Unicode events are typed as keysyms.
- Synchronize: when the mstsc window gets the focus back, every key still held is
  released and Caps, Num and Scroll Lock are set to the client's.
- Mouse: absolute position, five buttons, vertical and horizontal wheel in 120ths of a
  notch.
- Clipboard: text both ways (UTF-16 with CRLF on the RDP side, UTF-8 with LF in the
  session), fetched only when pasted; Wayland and, through Xwayland, X programs.
- Size: with `Mwm*outputManagement: true` the output takes the size the client asks for
  when it connects, and follows its window with clients that send layouts (Remmina,
  `xfreerdp3 /dynamic-resolution`). Sizes stay within 200–8192 a side, the width even,
  at most 8192×4320 pixels. Without output management the client gets the output's
  size.
- Pointer: from ext-image-copy-capture's cursor session as a 32-bit pointer with alpha,
  drawn by the client at its own mouse position, cached there. `-r` draws it into the
  picture instead, and so does wayrdp when the compositor has no cursor session.

Options and files are in the man page, `wayrdp.scd`.


## Setting it up on the server

This follows mwc's `docs/WAYLAND-RDP.md`: mwc runs headless as a user service, reached
through a Bitvise SSH tunnel. wayrdp takes RDP's port 3389 in place of xrdp and wayvnc;
there is no phase next to them, so it goes on the server only once it runs stable. mwc's
`docs/wayland-rdp-upgrade.sh` does these steps. A server that already runs mwc with xrdp
and a self-built wayvnc: [docs/upgrade-to-wayrdp.md](docs/upgrade-to-wayrdp.md).

### 1. Build and install

Ubuntu 26.04 has everything (FreeRDP 3.32, wlroots 0.19 for mwc):

```sh
sudo apt install --no-install-recommends build-essential meson ninja-build \
    pkg-config git freerdp3-dev libwinpr3-dev libwayland-dev \
    wayland-protocols libxkbcommon-dev libpixman-1-dev libdrm-dev libpam0g-dev
git clone https://github.com/reygnn/wayrdp.git
cd wayrdp
make
sudo make install                # /usr/local/bin/wayrdp, /etc/pam.d/wayrdp
```

The Makefile only wraps meson and ninja; its header lists the targets and variables.
It sets up `build` as a release build with `--sysconfdir=/etc`, so the PAM service
lands where PAM looks (meson's own default would be `/usr/local/etc`). These options
apply when `build` is set up; for an existing one, `make reconfigure`.
`sudo make uninstall` removes what was installed; it reads `build`'s install log, so
it has to come before `make distclean`. aml 1.0 is not packaged; meson fetches it as a subproject
(`subprojects/aml.wrap`).

### 1a. xrdp and wayvnc out

Only once wayrdp has built, so a failed build leaves the old way in place. Nothing else
may listen on 3389 afterwards (`ss -ltn | grep 3389`).

```sh
systemctl --user disable --now wayvnc
sudo systemctl disable --now xrdp xrdp-sesman
sudo apt purge xrdp wayvnc
```

### 2. mwc

`~/.config/systemd/user/mwc.service` needs `MWC_VIRTUAL_INPUT=1` (it is already there
for wayvnc). For the output to take the size the client connects with, add to
`~/.config/mwc/mwmrc` and restart mwc (the restart closes every window of the session):

```
Mwm*outputManagement: true
```

**Then nothing else may use wlr-output-management**: no wlr-randr, kanshi or wdisplays.
A second client of the protocol crashes the mwc session; see CAVEATS.

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

Bitvise's RDP tab works as it did with xrdp, no rule of its own. Log in with your Linux
user name and the RDP password. mstsc warns about the certificate on the first
connection: compare the thumbprint with the log and accept it permanently. For the
clipboard, tick it under Local Resources.


## Login

| `-a` | How | For | Against |
|---|---|---|---|
| `nla` (default) | CredSSP/NTLM against the SAM file from `--set-password` | mstsc's default; no login screen needed | a password of its own; the file holds an unsalted NT hash |
| `pam` | TLS only; the credentials mstsc sends along are checked with PAM (service `wayrdp`) | the Linux password, as with xrdp | mstsc warns about the missing NLA; it must have the credentials saved, or the connection is refused: wayrdp has no login screen |

For `-a pam`, remove `NoNewPrivileges=yes` from the service: PAM checks the password
with the setgid helper `unix_chkpwd`.

Either way, three failed logins in a row lock new connections out for 30 s, at most
four connections may wait for their logon at a time, and one that hasn't logged on
after 30 s is closed. Only a password that was checked and refused counts as a failed
login; mstsc drops two connections per login before that (at its certificate prompt and
to show its login dialog), and they count for nothing.


## Security

- wayrdp listens on localhost only unless told otherwise, and warns then: the SSH
  tunnel is the first lock, the RDP login the second.
- It refuses to start when the SAM file or the TLS key can be read by others.
- FreeRDP parses what comes from the network in wayrdp's process, not in mwc.
- Virtual input, capture and output management are open to every program in the
  session, and the systemd sandbox is thin in a user service; see CAVEATS.


## How this is built

wayrdp is developed with Claude (Anthropic) as a coding assistant. I set the scope and
the design and review the changes; Claude writes most of the code and the tests. The
checks:

* `meson test` pins the scancode table, the wheel, UTF-8/UTF-16 with CRLF, the size
  limits, the pointer conversion, the flow control, the Planar encoder, the buffers and
  `-v`, under ASan and UBSan.
* The scripts in `test/` run wayrdp against a headless mwc with real FreeRDP clients in
  further headless mwc instances: login with right and wrong passwords, the picture in a
  screenshot, typing and clicking through a simulated keyboard and mouse, the clipboard
  both ways, resizing, session takeover, the lockout, a stuck client, an unplugged
  output.
* Every fix comes with a test that fails without it, checked by taking the fix out.
* What is left open is written down rather than hidden: see CAVEATS and TODO.


## Testing

The development box (rootless Podman and Distrobox) brings everything the tests need:

```sh
distrobox assemble create --replace --file distrobox.ini
distrobox enter wayrdp-dev
make                                    # release build in build/
make test                               # unit tests
make e2e                                # every script in test/, one after another
make e2e-mwc-rdp                        # one of them, here -a nla
read -rs PAM_PASSWORD; export PAM_PASSWORD
make e2e-pam-rdp                        # end to end, -a pam
```

`pam-rdp` needs a password of the user in the box: PAM there checks the box's own
`/etc/shadow` (`sudo passwd $USER` in the box). Give it through the environment, as
above: on make's command line it would show in `ps`.

The scripts run the mwc installed on the host (`/run/host/usr/local/bin/mwc` in the box),
or `MWC=/path/to/mwc`. It must be the newest: built from `main` in the mwc repository
(`../mwc`, or `MWC_REPO`) or a later commit, as `mwc -v` names it; an older one, or one
that doesn't name its commit, fails the test.

`make BUILDDIR=build-debug BUILDTYPE=debug` keeps wayrdp's asserts; the tests then take
the same `BUILDDIR`. The scripts can also be run directly, `test/mwc-rdp.sh`. Besides
`mwc-rdp` and `pam-rdp` there are `pam-hangup` (wrong passwords, no `PAM_PASSWORD`
needed), `mwc-probe` (capture only), `clipboard-shutdown`,
`stuck-client` and `unplug-output`. `make e2e` counts a skipped script (exit 77, such as
`pam-rdp` without `PAM_PASSWORD`) as no failure and stops at the first one that fails. Each script's header says what it checks and
which variables pick the client, codec or a valgrind wrapper; wayrdp warns at its start
when one of its `WAYRDP_` test switches is on. `pam-rdp.sh` needs `/etc/pam.d/wayrdp`
and the password of the user running it.

By hand, without VM or tunnel: Remmina in the box, against mwc headless and wayrdp on
127.0.0.1:3389. It takes the graphics pipeline and, with dynamic resolution on, the
output follows its window.


## License

ISC, like wayvnc. See [COPYING](COPYING).
