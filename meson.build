project('oszilloskop_gui', 'c',
  version : '0.1.0',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')

raylib_dep = dependency('raylib', required : false)
if not raylib_dep.found()
  raylib_lib = cc.find_library('raylib',
    dirs : ['/opt/raylib-6.0/lib'],
    required : true,
  )
  raylib_dep = declare_dependency(
    include_directories : include_directories('/opt/raylib-6.0/include'),
    dependencies : [raylib_lib],
  )
endif

libusb_dep  = dependency('libusb-1.0', required : true)
threads_dep = dependency('threads',    required : true)

sources = files(
  'src/app.c',
  'src/app_logic.c',
  'src/app_view.c',
  'src/control_panel.c',
  'src/controls.c',
  'src/display_buffer.c',
  'src/display_column.c',
  'src/main.c',
  'src/sample_ring.c',
  'src/usb_reader.c',
)

executable('oszilloskop_gui',
  sources,
  include_directories : include_directories('src'),
  dependencies : [raylib_dep, libusb_dep, threads_dep],
  build_rpath : '/opt/raylib-6.0/lib',
)
