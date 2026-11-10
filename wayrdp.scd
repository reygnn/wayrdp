wayrdp(1)

# NAME

wayrdp - An RDP server for wlroots-based Wayland compositors.

# SYNOPSIS

*wayrdp* [options] [address [port]]

# DESCRIPTION

wayrdp captures an output of the compositor in $WAYLAND_DISPLAY and serves it
over RDP. The default address is 127.0.0.1, the default port 3389.

The RDP listener arrives with milestone M1; this version connects to the
compositor and captures frames only.

# OPTIONS

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

*-v*
	Print the version and exit.

# CONFIGURATION

Lines of *key = value* in the config file. Keys: address, port, auth,
sam_file, certificate_file, private_key_file, keyboard, output, max_rate.
Options on the command line win.

# COMPOSITOR SUPPORT

wayrdp needs wlr-screencopy or ext-image-copy-capture, and the virtual
keyboard and pointer protocols. mwc offers the latter only with
MWC_VIRTUAL_INPUT=1.

# AUTHORS

wayrdp is a fork of wayvnc by Andri Yngvason.
