project('json-lens', 'c', 'cpp',
  version: '0.1.0',
  meson_version: '>=0.50.0',
  default_options: ['warning_level=3', 'werror=true', 'c_std=c11', 'cpp_std=c++14'])

sdl2_dep = dependency('sdl2')

# Fallback handling for cross-platform OpenGL linking
gl_dep = dependency('gl', required: false)
if not gl_dep.found()
  if host_machine.system() == 'windows'
    cc = meson.get_compiler('cpp')
    gl_dep = cc.find_library('opengl32')
  elif host_machine.system() == 'darwin'
    gl_dep = dependency('appleframeworks', modules: ['OpenGL'])
  endif
endif

# Provide ImGui directly from the local source tree
imgui_inc = include_directories('third_party/imgui', 'third_party/imgui/backends')
imgui_src = [
  'third_party/imgui/imgui.cpp',
  'third_party/imgui/imgui_draw.cpp',
  'third_party/imgui/imgui_tables.cpp',
  'third_party/imgui/imgui_widgets.cpp',
  'third_party/imgui/backends/imgui_impl_sdl2.cpp',
  'third_party/imgui/backends/imgui_impl_opengl3.cpp'
]

executable('json-lens',
  'src/main.cpp',
  'src/arena_json.c',
  'src/settings.cpp',
  'src/utils.cpp',
  'src/views.cpp',
  imgui_src,
  include_directories: [include_directories('src'), imgui_inc],
  dependencies: [sdl2_dep, gl_dep],
  install: true,
  win_subsystem: 'windows'
)

install_data('com.example.JsonLens.desktop',
  install_dir: get_option('datadir') / 'applications'
)