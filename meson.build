project(
	'wayrdp',
	'c',
	license: 'ISC',
	meson_version: '>=0.58.0',
	default_options: [
		'c_std=gnu11',
		'warning_level=2',
	],
)

buildtype = get_option('buildtype')
host_system = host_machine.system()
prefix = get_option('prefix')

c_args = [
	'-D_GNU_SOURCE',
	'-DAML_UNSTABLE_API=1',

	'-Wno-unused-parameter',
	'-Wno-missing-field-initializers',
]

if buildtype != 'debug' and buildtype != 'debugoptimized'
	c_args += '-DNDEBUG'
endif

add_project_arguments(c_args, language: 'c')

cc = meson.get_compiler('c')

libm = cc.find_library('m', required: false)
librt = cc.find_library('rt', required: false)
pixman = dependency('pixman-1')
gbm = dependency('gbm', required: get_option('screencopy-dmabuf'))
drm = dependency('libdrm')
xkbcommon = dependency('xkbcommon', version: '>=1.0.0')
wayland_client = dependency('wayland-client')

# FreeRDP 3.24 is the oldest that Ubuntu 26.04 still has on some
# architectures (see docs/wayrdp-Entwurf.md).
freerdp_version = '>=3.24.0'
# As system headers: they warn about their own deprecated declarations.
freerdp = dependency('freerdp3', version: freerdp_version,
	include_type: 'system')
freerdp_server = dependency('freerdp-server3', version: freerdp_version,
	include_type: 'system')
winpr = dependency('winpr3', version: freerdp_version,
	include_type: 'system')
# makecert, for the self-signed certificate on first start.
winpr_tools = dependency('winpr-tools3', version: freerdp_version,
	include_type: 'system')

libpam = cc.find_library('pam', required: get_option('pam'),
	has_headers: ['security/pam_appl.h'])

aml_version = ['>=1.0.0', '<2.0.0']
aml_project = subproject('aml', required: false, version: aml_version,
	default_options: ['default_library=static'])
if aml_project.found()
	aml = aml_project.get_variable('aml_dep')
else
	aml = dependency('aml1', version: aml_version)
endif

inc = include_directories('include')

subdir('protocols')

sources = [
	'src/main.c',
	'src/log.c',
	'src/strlcpy.c',
	'src/shm.c',
	'src/screencopy.c',
	'src/ext-image-copy-capture.c',
	'src/screencopy-interface.c',
	'src/data-control.c',
	'src/output.c',
	'src/output-management.c',
	'src/pointer.c',
	'src/keyboard.c',
	'src/seat.c',
	'src/cfg.c',
	'src/intset.c',
	'src/buffer.c',
	'src/pixels.c',
	'src/transform-util.c',
	'src/util.c',
	'src/option-parser.c',
	'src/table-printer.c',
	'src/image-source.c',
	'src/toplevel.c',
	'src/desktop.c',
	'src/wayland.c',
	'src/vec.c',
	'src/rdp/server.c',
	'src/rdp/listener.c',
	'src/rdp/peer.c',
	'src/rdp/encoder.c',
	'src/rdp/auth.c',
	'src/rdp/keymap.c',
	'src/rdp/input.c',
	'src/rdp/clipboard.c',
	'src/rdp/text.c',
	'src/rdp/convert.c',
	'src/rdp/display.c',
	'src/rdp/cursor.c',
	'src/rdp/gfx.c',
	'src/rdp/flow.c',
	'src/rdp/deadline.c',
	'src/rdp/pam.c',
]

dependencies = [
	dependency('threads'),
	libm,
	librt,
	pixman,
	aml,
	gbm,
	drm,
	wayland_client,
	xkbcommon,
	client_protos,
	freerdp,
	freerdp_server,
	winpr,
	winpr_tools,
]

if libpam.found()
	dependencies += libpam
endif

config = configuration_data()

config.set('PREFIX', '"' + prefix + '"')

if host_system == 'linux' and get_option('systemtap') and cc.has_header('sys/sdt.h')
	config.set('HAVE_USDT', true)
endif

if cc.has_header('linux/dma-heap.h') and cc.has_header('linux/dma-buf.h')
	config.set('HAVE_LINUX_DMA_HEAP', true)
endif

if cc.has_function('memfd_create')
	config.set('HAVE_MEMFD', true)
	config.set('HAVE_MEMFD_CREATE', true)
elif cc.has_function('SYS_memfd_create', prefix : '#include <sys/syscall.h>')
	config.set('HAVE_MEMFD', true)
endif

if gbm.found() and not get_option('screencopy-dmabuf').disabled()
	config.set('ENABLE_SCREENCOPY_DMABUF', true)
endif

if libpam.found()
	config.set('ENABLE_PAM', true)
	install_data('wayrdp.pam', rename: 'wayrdp',
		install_dir: get_option('sysconfdir') / 'pam.d')
endif

configure_file(
	output: 'config.h',
	configuration: config,
)

# The commit built, after the version (include/version.h); written on
# every build, but only when it changes.
gitversion = vcs_tag(
	command: ['sh', meson.current_source_dir() / 'util/gitversion.sh'],
	input: 'include/gitversion.h.in',
	output: 'gitversion.h',
	replace_string: '@WAYRDP_GIT@',
	fallback: '',
)

wayrdp_exe = executable(
	'wayrdp',
	sources + [gitversion],
	dependencies: dependencies,
	include_directories: [inc, include_directories('src')],
	install: true,
)

scdoc = dependency('scdoc', native: true, required: get_option('man-pages'))
if scdoc.found()
	scdoc_prog = find_program(scdoc.get_variable(pkgconfig: 'scdoc'), native: true)
	sh = find_program('sh', native: true)
	mandir = get_option('mandir')
	manpages = {
		'wayrdp.scd': 'wayrdp.1',
	}

	foreach input, output : manpages
		custom_target(
			output,
			input: input,
			output: output,
			command: [
				sh, '-c', '@0@ <@INPUT@ >@1@'.format(scdoc_prog.full_path(), output)
			],
			install: true,
			install_dir: '@0@/man1'.format(mandir)
		)
	endforeach
endif

if get_option('tests')
	subdir('test')
endif
