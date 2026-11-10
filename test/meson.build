test('table-printer', executable('table-printer',
	[
		'table-printer-test.c',
		'../src/table-printer.c',
	],
	include_directories: inc,
	dependencies: [ ],
))
test('option-parser', executable('option-parser',
	[
		'option-parser-test.c',
		'../src/option-parser.c',
		'../src/table-printer.c',
		'../src/strlcpy.c',
	],
	include_directories: inc,
	dependencies: [ ],
))
test('keymap', executable('keymap',
	[
		'keymap-test.c',
		'../src/rdp/keymap.c',
	],
	include_directories: [inc, include_directories('../src')],
	dependencies: [ ],
))
# Drives the compositor of the RDP client in test/mwc-rdp.sh.
executable('vinput',
	[ 'vinput.c' ],
	dependencies: [ wayland_client, xkbcommon, client_protos ],
)
test('text', executable('text',
	[
		'text-test.c',
		'../src/rdp/text.c',
	],
	include_directories: [inc, include_directories('../src')],
	dependencies: [ ],
))
test('convert', executable('convert',
	[
		'convert-test.c',
		'../src/rdp/convert.c',
	],
	include_directories: [inc, include_directories('../src')],
	dependencies: [ ],
))
