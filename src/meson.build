
project('COMM_COMPRESSION', 'C', 'CPP',
        version: '0.0.1',
        license: '',
        default_options: ['buildtype=debug'],
        meson_version: '>= 0.61.2'
)

languages = ['c', 'cpp']
add_project_arguments('-D DOCA_LOGGING_ALLOW_DLOG', language: languages)
add_project_arguments('-D DOCA_ALLOW_EXPERIMENTAL_API', language: languages)

add_project_link_arguments('-Wl,-rpath,/usr/local/lib/', language: languages)

app_dependencies = []
app_dependencies += dependency('json-c')
app_dependencies += dependency('threads')
app_dependencies += dependency('doca')
app_dependencies += dependency('zlib')
app_dependencies += dependency('libzstd')

common_dir_path = '/opt/mellanox/doca/applications/common/src'
samples_dir_path = '/opt/mellanox/doca/samples/'

app_inc_dirs = [
	include_directories(common_dir_path),
    include_directories('/opt/mellanox/doca'),
    include_directories('/usr/local/include/'),
]

app_srcs = [
	'file_compression_core.c',
	common_dir_path + '/pack.c',
	common_dir_path + '/utils.c',
	common_dir_path + '/doca_utils.c',
	samples_dir_path + '/common.c',
]

vanilla_app_srcs = [
	'file_compression.c',
]

base_c_args = [
        '-Wno-missing-braces',
        '-Wno-missing-field-initializers',
]

executable('doca_comm_compression',
	app_srcs + vanilla_app_srcs,
	c_args : base_c_args,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: true)


app_compression_total_srcs = [
	'file_compression_total_main.c',
	'file_compression_total_core.c',
	common_dir_path + '/pack.c',
	common_dir_path + '/utils.c',
	common_dir_path + '/doca_utils.c',
	samples_dir_path + '/common.c',
]

executable('doca_comm_compression_total',
	app_compression_total_srcs,
	c_args : base_c_args,
	dependencies : app_dependencies,
	include_directories : app_inc_dirs,
	install: true)
