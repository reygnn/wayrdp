# One program per module: NAME-test.c with the sources it tests, always under
# ASan and UBSan, whatever the build type, so an overflow fails the test.
san = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
	'-fno-omit-frame-pointer']
src_inc = include_directories('../../src')
# '../..' also brings the build root, where config.h is.
root_inc = include_directories('../..', '../../src')

units = {
	'table-printer': [['../../src/table-printer.c'], inc, []],
	'option-parser': [['../../src/option-parser.c', '../../src/table-printer.c',
		'../../src/strlcpy.c'], inc, []],
	'keymap': [['../../src/rdp/keymap.c'], [inc, src_inc], []],
	'text': [['../../src/rdp/text.c'], [inc, src_inc], []],
	'convert': [['../../src/rdp/convert.c'], [inc, src_inc], []],
	'planar': [['../../src/rdp/encoder.c'], [inc, root_inc],
		[freerdp, winpr, pixman]],
	'flow': [['../../src/rdp/flow.c'], [inc, src_inc], []],
	'intset': [['../../src/intset.c'], inc, []],
}

foreach name, u : units
	test(name, executable(name,
		[name + '-test.c'] + u[0],
		include_directories: u[1],
		dependencies: u[2],
		c_args: san,
		link_args: san,
	))
endforeach
