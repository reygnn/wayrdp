subdir('unit')

# Drives the compositor of the RDP client in test/mwc-rdp.sh.
executable('vinput',
	[ 'vinput.c' ],
	dependencies: [ wayland_client, xkbcommon, client_protos ],
)
