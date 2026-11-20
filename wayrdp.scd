wayrdp(1)

# NAME

wayrdp - An RDP server for wlroots-based Wayland compositors.

# SYNOPSIS

*wayrdp* [options] [address [port]]

# DESCRIPTION

wayrdp captures an output of the compositor in $WAYLAND_DISPLAY and serves it
over RDP. The default address is 127.0.0.1, the default port 3389.

Logins use NLA against a password set with *--set-password*, or with *-a
pam* the Linux password over TLS. The client's
keys arrive as physical scancodes; *-k* sets the layout that gives them
meaning.

# OPTIONS

*-a, --auth=nla|pam*
	The login. *nla* (default): CredSSP against the password from
	*--set-password*. *pam*: TLS without NLA; the credentials the client
	sends along (saved in mstsc) are checked with PAM, service *wayrdp*.
	There is no login screen: without saved credentials the connection
	is refused.

*-C, --config=<path>*
	Select a config file. Default: ~/.config/wayrdp/config

*-f, --max-fps=<fps>*
	Set the highest frame rate (default 30).

*-h, --help*
	Get help.

*-k, --keyboard=<layout>[-<variant>]*
	Keyboard layout, e.g. de or de-nodeadkeys.

*-L, --log-level=<level>*
	error, warning, info, debug, trace or quiet. Default: warning.

*-o, --output=<name>*
	The output to capture, e.g. HEADLESS-1. Default: the first one.

*--probe=<frames>*
	Capture this many frames without a client, print size, format, frame
	count and damage, and exit.

*-r, --render-cursor*
	Draw the pointer into the picture.

*--set-password*
	Ask for the RDP password (twice on a terminal, one line from a pipe),
	write its NT hash to the SAM file and exit.

*-v*
	Print the version and exit: the one set in include/version.h and the
	commit built, as 0.5.4+DATE.TIME.HASH with its commit time in UTC and
	.dirty for changes not committed (nothing after the version without git).

# CONFIGURATION

Lines of *key = value* in the config file. Keys: address, port, auth,
sam_file, certificate_file, private_key_file, keyboard, output, max_rate.
Options on the command line win.

# FILES

*~/.config/wayrdp/config*
	The config file.

*~/.config/wayrdp/sam*
	The user and the NT hash of the RDP password, mode 0600.

*~/.config/wayrdp/tls.crt*, *~/.config/wayrdp/tls.key*
	The TLS certificate, made on the first start. Its SHA-1 thumbprint is
	logged.

*/etc/pam.d/wayrdp*
	The PAM service for *-a pam*: pam_unix alone, the password and nothing
	else. Not common-auth, whose modules for presence at the machine (a
	fingerprint, a security key) must not count for a login from the
	network. PAM gets the client's address as PAM_RHOST.

# ENVIRONMENT

*WAYRDP_FORCE_PLANAR=1*
	Send Planar bitmaps even to clients that take RemoteFX. For tests.

*WAYRDP_LOGIN_TIMEOUT=<seconds>*
	How long a connection may take to log on (default 30). For tests.

# SECURITY

Three failed logins in a row lock new connections out for 30 s; at most
four connections may wait for their logon at a time. With *-a pam* a user
name that isn't the account counts as a failed login too, and every refusal
takes at least 2 s, so its time doesn't tell what was wrong. wayrdp refuses to start
when the SAM file or the TLS key can be read by others.

# COMPOSITOR SUPPORT

wayrdp needs wlr-screencopy or ext-image-copy-capture, and the virtual
keyboard and pointer protocols. mwc offers the latter only with
MWC_VIRTUAL_INPUT=1.

For the output to follow the client's window, the compositor must offer
wlr-output-management (mwc: *Mwm\*outputManagement: true*), and no other
client may use it at the same time: wlroots 0.19 and 0.20 abort then. The
pointer image needs ext-image-copy-capture's cursor session; without it,
or with *-r*, the pointer is drawn into the picture.

# AUTHORS

wayrdp is a fork of wayvnc by Andri Yngvason.
