# Notes

Measurements and where the implementation differs from the design in
[wayrdp-Entwurf.md](wayrdp-Entwurf.md).


## Measured

RemoteFX as surface bits at 1920×1080 on an 8-core machine (2026-11-10, before the
graphics pipeline): idle 0 %, a terminal with a clock 23 % of one core at 9 fps, a
full-screen terminal scrolling without pause 88 % of one core at 30 fps, 2.2 MB/s, 8 ms
encoding per frame.

A terminal scrolling without pause at 1280×800, 30 fps, sdl-freerdp3 on the same machine
(2026-11-11): GFX with RemoteFX Progressive 6.8 MB/s, 3.8 ms encoding per frame, wayrdp
68 % of a core; RemoteFX as surface bits 6.9 MB/s, 3.4 ms, 59 %; Planar 9.3 MB/s, 8.5 ms,
35 %. Text is the worst case for RemoteFX; mstsc took Planar before, since it offers
RemoteFX only over the pipeline.

mstsc (Windows 11 VM, through Bitvise, 1280×800), a window dragged around without pause:
with the graphics pipeline (10.6, RemoteFX Progressive) 29 fps, 890 kB/s, 1.7 ms encoding
per frame, smooth, text sharp; before, with Planar, 7.9 fps, 1863 kB/s, 7 ms (2026-11-10,
while Windows Update ran in the VM, so part of the frame rate may be that).


## Where it differs from the design

- Scancodes: an own table (`src/rdp/keymap.c`) instead of
  `freerdp_keyboard_get_x11_keycode_from_rdp_scancode()`, which is deprecated since
  FreeRDP 3.11 and only filled by the X11 keyboard setup.
- RemoteFX: tiles go in groups through `rfx_encode_message()`, not
  `rfx_encode_messages()`, whose split leaks the message array in FreeRDP 3.32 (about
  300 bytes per frame).
- `data-control.c` is rewritten: wayvnc's read every selection at once, the primary one
  too.
- drdynvc is opened only after the connection sequence: FreeRDP sets it up once, and too
  early its first message lands in the licensing phase.
- Graphics pipeline: planned as v2, done now, because mstsc offers RemoteFX only through
  it. The channel runs on the main loop (FreeRDP's external-thread mode), not in a
  thread of its own. On a size change the client reactivates as before, then gets
  ResetGraphics and a new surface.
- The pointer goes out only after the first picture of an activation.
- systemd: the file-system options don't hold in a user service on Ubuntu; see
  [CAVEATS](../CAVEATS.md).
- Hardening beyond the design: the login timeout, the limit of waiting connections and
  the check of the secrets' modes.
