wayrdp(1)

# NAME

wayrdp - An RDP server for wlroots-based Wayland compositors.

# SYNOPSIS

*wayrdp* [options] [address [port]]

# DESCRIPTION

wayrdp captures an output of the compositor in $WAYLAND_DISPLAY and serves it
over RDP. The default address is 127.0.0.1, the default port 3389.

Logins use NLA against a password set with *--set-password*. The client's
keys arrive as physical scancodes; *-k* sets the layout that gives them
meaning.

# OPTIONS

*-a, --auth=nla|pam*
	The login. *nla* (default): CredSSP against the password from
	*--set-password*. *pam* comes with milestone M5.

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
	Print the version and exit.

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

# ENVIRONMENT

*WAYRDP_FORCE_PLANAR=1*
	Send Planar bitmaps even to clients that take RemoteFX. For tests.

# COMPOSITOR SUPPORT

wayrdp needs wlr-screencopy or ext-image-copy-capture, and the virtual
keyboard and pointer protocols. mwc offers the latter only with
MWC_VIRTUAL_INPUT=1.

# AUTHORS

wayrdp is a fork of wayvnc by Andri Yngvason.
