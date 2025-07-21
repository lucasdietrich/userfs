project('userfs', 'cpp', 'c',
  version: '1.0',
  default_options: [
    'cpp_std=c++17',
    'c_std=c11',
    'buildtype=debug',
    'warning_level=3',
    'werror=false',
    'default_library=shared'
  ],
)

add_global_arguments([

], language: ['cpp', 'c'])


if get_option('partition_table') == 'dos'
  add_global_arguments('-DUSERFS_PARTITION_TABLE_DOS', language: ['cpp', 'c'])
endif

dependencies = [
  dependency('fdisk'),
  dependency('blkid'),
]

sources = [
  'src/main.c',
]

include_directories = [
  # 'include',
  'src',
]

link_with = [
]

executable(
  meson.project_name(),
  sources,
  dependencies: dependencies,
  include_directories: include_directories,
  link_with: link_with,
  install: true,
  install_dir: 'bin',
)



