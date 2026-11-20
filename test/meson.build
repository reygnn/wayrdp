subdir('unit')

# Drives the compositor of the RDP client in test/mwc-rdp.sh.
executable('vinput',
	[ 'vinput.c' ],
	dependencies: [ wayland_client, xkbcommon, client_protos ],
)
# A selection owner that writes blocking, for the large paste in test/mwc-rdp.sh.
executable('selowner',
	[ 'selowner.c' ],
	dependencies: [ wayland_client, client_protos ],
)
