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
elif get_option('partition_table') == 'gpt'
  add_global_arguments('-DUSERFS_PARTITION_TABLE_GPT', language: ['cpp', 'c'])
  error('GPT partition table is not supported yet')
else
  error('Unsupported partition table type: ' + get_option('partition_table'))
endif

add_global_arguments('-DUSERFS_PART_NO=' + get_option('userfs_partno'), language: ['cpp', 'c'])
if get_option('overlay_opt')
  add_global_arguments('-DUSERFS_OVERLAY_OPT', language: ['cpp', 'c'])
endif

if get_option('block_device_type') == 'mmc'
  add_global_arguments('-DUSERFS_BLOCK_DEVICE_TYPE_MMC', language: ['cpp', 'c'])
else
  add_global_arguments('-DUSERFS_BLOCK_DEVICE_TYPE_DISK', language: ['cpp', 'c'])
endif

add_global_arguments('-DDISK="' + get_option('block_device_name') + '"', language: ['cpp', 'c'])

dependencies = [
  dependency('fdisk'),
  dependency('blkid'),
]

sources = [
  'src/main.c',
  'src/overlays.c',
  'src/btrfs.c',
  'src/utils.c',
  'src/disk.c',
]

include_directories = [
  'include',
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



