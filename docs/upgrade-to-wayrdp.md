# Upgrading to wayrdp

From a server that already runs mwc (Wayland) and is reached through xrdp and a
self-built wayvnc, to wayrdp alone. Afterwards there is no xrdp, no wayvnc
(nor its neatvnc and aml), and no VNC port; mstsc connects through the
Bitvise tunnel straight to wayrdp on 127.0.0.1:3389, where xrdp was.

Assumed: Ubuntu 26.04, mwc as the user service `mwc.service`, the Windows
notebook with Bitvise and mstsc. mwc's `docs/wayland-rdp-upgrade.sh` covers
the move from X11 (mwm) and removes wayvnc only as a package; this page is
for the case where wayvnc was built by hand.

The order matters: wayrdp is built first, the old way goes only once that
worked, and the old sources go last, once wayrdp has done the job for a
while.

## 0. Before you start

- Work from a **plain SSH session**, not from the RDP desktop: stopping xrdp
  ends that one. Between section 3 and section 6 there is no RDP at all.
- Keep the SSH session open until section 7 has worked.
- The way back stays until section 9: a copy of `/etc/xrdp` (section 3) and
  the wayvnc build directories.

## 1. Look what is there

Nothing here changes anything. The output tells which of the steps below
apply.

```sh
# packages
dpkg -l | grep -E 'xrdp|wayvnc|neatvnc|libaml|tigervnc|xorgxrdp' | awk '{print $1, $2, $3}'
# files installed by hand
ls -la /usr/local/bin | grep -iE 'vnc|mwc|wayrdp'
find /usr/local/lib* /usr/local/include /usr/local/share -maxdepth 3 \
    \( -iname '*neatvnc*' -o -iname '*aml*' -o -iname '*wayvnc*' \) 2>/dev/null
# services
systemctl list-unit-files | grep -iE 'xrdp|vnc'
systemctl --user list-unit-files | grep -iE 'vnc|mwc|wayrdp'
ls ~/.config/systemd/user/
# configuration
ls -d ~/.config/wayvnc ~/.vnc /etc/xrdp 2>/dev/null
# the build directories, if still there
ls -d ~/wayvnc ~/neatvnc ~/aml ~/src/* 2>/dev/null
# who listens where
ss -ltnp | grep -E ':3389|:5900'
```

## 2. Build and install wayrdp

```sh
sudo apt install --no-install-recommends build-essential meson ninja-build \
    pkg-config git freerdp3-dev libwinpr3-dev libwayland-dev \
    wayland-protocols libxkbcommon-dev libpixman-1-dev libdrm-dev libpam0g-dev
git clone https://github.com/reygnn/wayrdp.git ~/wayrdp
cd ~/wayrdp
meson setup build --buildtype=release --sysconfdir=/etc
ninja -C build
sudo ninja -C build install      # /usr/local/bin/wayrdp, /etc/pam.d/wayrdp
```

meson fetches aml from github.com during `meson setup` (it is built into
wayrdp, not installed). Behind a proxy that blocks it: clone
`https://github.com/any1/aml.git` (tag v1.0.0) into `subprojects/aml` by hand.

Only if this worked, go on. If not, nothing has changed yet.

## 3. xrdp out

```sh
sudo cp -a /etc/xrdp ~/xrdp-backup          # the way back, see the end
sudo systemctl disable --now xrdp xrdp-sesman
sudo apt purge xrdp xorgxrdp
sudo rm -rf /etc/xrdp /var/log/xrdp*.log    # what purge leaves, if anything
```

`xorgxrdp` only if section 1 listed it; it may take Xorg packages with it,
apt shows the list. Then nothing may listen on 3389:

```sh
ss -ltnp | grep ':3389' || echo "3389 is free"
```

## 4. wayvnc out

Stop it first:

```sh
systemctl --user disable --now wayvnc
rm -f ~/.config/systemd/user/wayvnc.service
systemctl --user daemon-reload
```

**With the build directories** (the cleanest way: ninja removes exactly what
it installed). Order: wayvnc, then neatvnc, then aml, since each uses the
next.

```sh
sudo ninja -C ~/wayvnc/build uninstall
sudo ninja -C ~/neatvnc/build uninstall
sudo ninja -C ~/aml/build uninstall
sudo ldconfig
```

Use the paths section 1 found. A build directory made with another name
(`builddir`, …) works the same.

**Without them**, by hand, after section 1 listed the files (on x86_64 the
libraries are in `/usr/local/lib/x86_64-linux-gnu/`):

```sh
sudo rm -f /usr/local/bin/wayvnc /usr/local/bin/wayvncctl
sudo rm -f /usr/local/lib/x86_64-linux-gnu/libneatvnc.so* \
           /usr/local/lib/x86_64-linux-gnu/libaml.so*
sudo rm -f /usr/local/lib/x86_64-linux-gnu/pkgconfig/neatvnc.pc \
           /usr/local/lib/x86_64-linux-gnu/pkgconfig/aml.pc
sudo rm -f /usr/local/include/neatvnc.h /usr/local/include/aml.h
sudo rm -f /usr/local/share/man/man1/wayvnc.1 /usr/local/share/man/man1/wayvncctl.1
sudo ldconfig
```

Only what section 1 showed; a path that isn't there needs no `rm`. wayrdp
doesn't need any of them: it builds its own aml in.

If wayvnc came as a package instead: `sudo apt purge wayvnc` (and
`libneatvnc0`, `libaml0` if apt doesn't take them along with
`sudo apt autoremove`).

Its configuration goes too:

```sh
rm -rf ~/.config/wayvnc
```

Check that nothing is left that still needs it:

```sh
ldconfig -p | grep -E 'neatvnc|libaml' || echo "gone"
ss -ltnp | grep ':5900' || echo "no VNC port"
```

## 5. mwc

In `~/.config/systemd/user/mwc.service`, `MWC_VIRTUAL_INPUT=1` must stay (it
was there for wayvnc; wayrdp types and moves the pointer through the same
protocols):

```ini
Environment=MWC_VIRTUAL_INPUT=1
```

In `~/.config/mwc/mwmrc`, so that the output takes the size mstsc connects
with:

```
Mwm*outputManagement: true
```

Then nothing else may use wlr-output-management (no wlr-randr, kanshi or
wdisplays): wlroots 0.19 aborts when a second client of it comes, and takes
the session with it. mwc reads this switch only at its start:

```sh
systemctl --user restart mwc
```

## 6. wayrdp as a service

```sh
wayrdp --set-password                     # the RDP password, not the Linux one
mkdir -p ~/.config/systemd/user
cp ~/wayrdp/contrib/wayrdp.service ~/.config/systemd/user/
# in it: -k de -> the notebook's layout (de, ch, us, …)
sudo loginctl enable-linger "$USER"       # runs without anyone logged in
systemctl --user daemon-reload
systemctl --user enable --now wayrdp
journalctl --user -u wayrdp -n 20         # "Listening on 127.0.0.1:3389", the thumbprint
ss -ltn | grep '127.0.0.1:3389'
```

Only `127.0.0.1:3389` may show: wayrdp is reached through the SSH tunnel,
never from the network.

With the Linux password instead of an RDP password of its own: `-a pam` in
`ExecStart`, and the line `NoNewPrivileges=yes` out (PAM checks the password
with the setgid helper `unix_chkpwd`). mstsc then must have the credentials
saved; wayrdp has no login window.

## 7. The notebook

- **Bitvise:** the RDP tab as before, *Computer* = `localhost`. No rule of
  its own.
- **Saved credentials:** mstsc still has the ones for xrdp (the Linux
  password). With NLA they are wrong now and cost a failed login on every
  connection; three in a row lock out for 30 s. Delete them in a command
  prompt:

  ```
  cmdkey /list
  cmdkey /delete:TERMSRV/localhost
  ```

  (or in the Credential Manager, *Windows Credentials*), and save the RDP
  password on the next login.
- **Certificate:** the first connection warns. Compare its SHA-1 thumbprint
  with the one in `journalctl --user -u wayrdp`, then accept it for good.
- **Clipboard:** tick it under *Local Resources*.
- **Size:** mstsc gets the size it connects with; it doesn't follow its
  window during the session (mstsc does that for Windows hosts only). Full
  screen gives the screen's resolution. After a change, reconnect: the
  session stays.

## 8. Check

- [ ] the desktop shows, smooth (wayrdp's log: `GFX, RemoteFX Progressive`)
- [ ] typing with umlauts and AltGr, the wheel
- [ ] copy and paste both ways
- [ ] disconnect and reconnect: the session is still there
- [ ] `ss -ltnp` shows 3389 on 127.0.0.1 only, and no 5900

## 9. Last, once it has done the job for a while

- The old build directories: `rm -rf ~/wayvnc ~/neatvnc ~/aml` (the paths
  from section 1), and `~/xrdp-backup`. Until then they are the way back.
- Packages only xrdp and the VNC build needed: `sudo apt autoremove --purge`,
  and look at what apt lists before saying yes; `xwayland` must stay.
- Was the server on mwm (X11) before mwc: mwc's
  `docs/wayland-rdp-upgrade.sh --purge` removes the X11 rest.

## Back, if it doesn't work

Before section 3 nothing has changed. After it:

```sh
systemctl --user disable --now wayrdp
sudo apt install xrdp
# wayvnc: ninja -C ~/wayvnc/build install (and neatvnc, aml before it)
```

The xrdp configuration is in `~/xrdp-backup` (section 3): copy back what had
changes of your own, e.g. `sudo cp ~/xrdp-backup/xrdp.ini /etc/xrdp/`, then
`sudo systemctl restart xrdp`.
