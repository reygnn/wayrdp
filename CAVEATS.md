# Caveats

Known limits of wayrdp that are left as they are, with what they mean in use.


## mstsc does not resize during a session

mstsc sends no monitor layout to wayrdp, neither for its window nor in full screen after
a change of the local resolution, whatever the .rdp file says. xrdp found the same
("MSTSC supports nearly everything, except resize-on-the-fly"); it seems to do it for
Windows hosts only. With mstsc the size is the one it connects with: full screen gives
the screen's resolution; after a change, reconnect (the session stays). Remmina and
`xfreerdp3 /dynamic-resolution` do follow their window.


## A second wlr-output-management client crashes the session

wlroots 0.19 and 0.20 abort in `head_send_state` when a second client of
wlr-output-management is around, and take the whole mwc session with them (fixed only
on wlroots master, 40640950). With `Mwm*outputManagement: true` nothing else may use
the protocol: no wlr-randr, kanshi or wdisplays. That is why mwc offers it only when
asked.


## Capture of an output that was off

On wlroots 0.19.2 a program that captures an output first while it is switched off
leaves the capture source at 0x0 without formats until the mode or format changes.
wayrdp then logs "Buffer dimensions: 0x0" and "No supported buffer formats" and shows
nothing. wayrdp itself switches the output on before each session; only another
program, such as grim or wayvnc, triggers this. Connecting with a different window size
(with output management) or restarting mwc clears it. Fixed in wlroots 0.20.2.


## The systemd sandbox is thin in a user service

What holds are the seccomp and prctl options in `contrib/wayrdp.service` (address
families, `NoNewPrivileges`, `LockPersonality`, `RestrictNamespaces`, …).
`ProtectSystem`, `PrivateTmp` and `ReadWritePaths` need a mount namespace, which a user
service doesn't get where AppArmor restricts unprivileged user namespaces (Ubuntu since
23.10); systemd then skips them without a word. Checked on Ubuntu 26.04: with them set,
the service could still write to the home directory. They stay in the file for systems
where they work.


## Unsalted NT hash

With `-a nla` the SAM file from `wayrdp --set-password` holds the password as an
unsalted NT hash, as NTLM requires. wayrdp refuses to start when others can read it; a
copy of the file is as good as the password for RDP. `-a pam` avoids it at the cost of
no NLA.


## Session input and capture are open to every program

`MWC_VIRTUAL_INPUT=1` lets every program in the session type and click, as with wayvnc.
The capture protocols and output management are open to every program of the session
too; mwc has no security-context-v1.


## No H.264

The picture is RemoteFX Progressive over the graphics pipeline, RemoteFX as surface
bits or Planar. Ubuntu's FreeRDP 3.32 is built without an H.264 encoder; AVC420 is in
TODO for when the tunnel at work asks for it.


## Clipboard

Text only, at most 16 MiB; no images or files. The primary selection (middle click)
stays in the session, since RDP has none. A side that doesn't answer within 5 s gets
nothing.


## Keyboard

Unicode events (on-screen keyboard, IME) are typed as keysyms; characters the server's
layout (`-k`) lacks are dropped.


## Test clients

These concern the scripts in `test/`, not wayrdp. mwc has no Xwayland, so xfreerdp3
only checks logins (`+auth-only`, no display) and sdl-freerdp3 is the client with a
window. sdl-freerdp3 takes Wayland only where the compositor has `wp_fifo_v1`, it
writes a clipboard text of some 200 kB with `\u` escapes and a NUL at its end, and the
FreeRDP clients offer RemoteFX even with `-rfx`, so the Planar test needs
`WAYRDP_FORCE_PLANAR=1`. wlfreerdp3 is deprecated: it now and then hangs once pointer
images come, its clipboard turns text into `\u` escapes, and it crashes on disconnect.
