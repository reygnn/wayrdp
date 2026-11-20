# One program per module: NAME-test.c with the sources it tests, always under
# ASan and UBSan, whatever the build type, so an overflow fails the test.
san = ['-fsanitize=address,undefined,float-cast-overflow',
	'-fno-sanitize-recover=all',
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
	'log': [['../../src/log.c'], inc, []],
	'time-util': [[], inc, []],
	'auth': [['../../src/rdp/auth.c', '../../src/log.c'], [inc, root_inc],
		[freerdp, winpr, winpr_tools]],
	'buffer': [['../../src/buffer.c', '../../src/shm.c', '../../src/log.c',
		'../../src/pixels.c', '../../src/util.c', '../../src/strlcpy.c'],
		[inc, root_inc], [wayland_client, client_protos, pixman, drm]],
}
if libpam.found()
	units += {'pam': [['../../src/rdp/pam.c', '../../src/log.c'],
		[inc, root_inc], [libpam]]}
endif
# Only what cannot be made real: a failing mmap.
wrap = {'buffer': ['-Wl,--wrap=mmap64']}
# The PAM service file shipped.
defines = {'pam': ['-DWAYRDP_PAM_FILE="@0@"'.format(
	meson.project_source_root() / 'wayrdp.pam')]}

foreach name, u : units
	test(name, executable(name,
		[name + '-test.c'] + u[0],
		include_directories: u[1],
		dependencies: u[2],
		c_args: san + defines.get(name, []),
		link_args: san + wrap.get(name, []),
	))
endforeach
