project('notework', 'c',
  version : '0.1.0',
  default_options : [
    'c_std=c11',
    'warning_level=3',  # -Wall -Wextra -Wpedantic
  ],
)

# 依赖
gtk4_dep    = dependency('gtk4', version : '>=4.10')
adw_dep     = dependency('libadwaita-1', version : '>=1.0')
m_dep       = meson.get_compiler('c').find_library('m', required : false)
# 相册模块用 poppler-glib 把 PDF 拆成 cairo_surface
poppler_dep = dependency('poppler-glib', version : '>=21', required : true)

# 引入 gnome 模块（用于编译 GResource）
gnome = import('gnome')

# 将 src/notework.gresource.xml 及其引用的 UI 文件编译为两个源文件（.c + .h）
resources = gnome.compile_resources(
  'notework-resources',
  'src/notework.gresource.xml',
  source_dir : 'src',
  c_name : 'notework',
)

# 可执行文件
executable('notework',
  sources : ['src/main.c', resources],
  dependencies : [gtk4_dep, adw_dep],
  install : true,
)

# ─── 涂鸦/绘图模块（独立可执行，便于独立调试；后续可与主程序集成）───
doodle_resources = gnome.compile_resources(
  'doodle-resources',
  'src/doodle.gresource.xml',
  source_dir : 'src',
  c_name : 'doodle',
)

executable('notework-doodle',
  sources : [
    'src/doodle_main.c',
    'src/doodle_window.c',
    'src/doodle_canvas.c',
    'src/doodle_model.c',
    doodle_resources,
  ],
  dependencies : [gtk4_dep, adw_dep, m_dep],
  install : true,
)

# ─── 相册/多页编辑模块（独立可执行，便于独立调试；后续可与主程序集成）───
album_resources = gnome.compile_resources(
  'album-resources',
  'src/album.gresource.xml',
  source_dir : 'src',
  c_name : 'album',
)

executable('notework-album',
  sources : [
    'src/album_main.c',
    'src/album_window.c',
    'src/album_model.c',
    'src/album_importer.c',
    'src/doodle_window.c',
    'src/doodle_canvas.c',
    'src/doodle_model.c',
    album_resources,
  ],
  dependencies : [gtk4_dep, adw_dep, m_dep, poppler_dep],
  install : true,
)
