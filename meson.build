project('c-hello-world', 'cpp', 'c',
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


if get_option('example')
  message('Example option is enabled')
  add_project_arguments('-DEXAMPLE_ENABLED', language: ['cpp', 'c'])
else
  message('Example option is disabled')
endif



dependencies = [
]

sources = [
  'src/main.c',
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
  install: false,
  install_dir: 'bin',
)



