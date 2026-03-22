project('json-lens', 'vala', 'c',
  version: '0.1.0',
  meson_version: '>=0.50.0',
  default_options: ['warning_level=2', 'c_std=c11'])

gnome = import('gnome')
gtk_dep = dependency('gtk4', version: '>=4.0')
glib_dep = dependency('glib-2.0', version: '>=2.60')

cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required: false)
gtksource_dep = dependency('gtksourceview-5')

executable('json-lens',
  'src/main.vala',
  'src/arena_json.vapi',
  'src/arena_json.c',
  include_directories: include_directories('src'),
  dependencies: [gtk_dep, glib_dep, math_dep, gtksource_dep],
  c_args: ['-w'],
  install: true,
  win_subsystem: 'windows'
)

install_data('com.example.JsonLens.desktop',
  install_dir: get_option('datadir') / 'applications'
)