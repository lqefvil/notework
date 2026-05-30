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
# notework-snip 在 X11 后端通过 _NET_WM_STATE_ABOVE ClientMessage 实现置顶，
# 直接使用 XSendEvent/XFlush，故需要显式链接 libX11（Wayland-only 环境可
# 缺失）。
x11_dep     = dependency('x11', required : false)
# 相册模块用 poppler-glib 把 PDF 拆成 cairo_surface
poppler_dep = dependency('poppler-glib', version : '>=21', required : true)
# tokensam 模块用 graphviz dot 引擎计算 DAG 布局
cgraph_dep  = dependency('libcgraph', version : '>=2.40', required : true)
gvc_dep     = dependency('libgvc',    version : '>=2.40', required : true)

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
  sources : [
    'src/main.c',
    'src/progress_axis.c',
    'src/track_row.c',
    'src/album_window.c',
    'src/album_model.c',
    'src/album_importer.c',
    'src/doodle_window.c',
    'src/doodle_canvas.c',
    'src/doodle_model.c',
    'src/latex_model.c',
    'src/latex_window.c',
    resources,
  ],
  dependencies : [gtk4_dep, adw_dep, m_dep, poppler_dep],
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
    'src/latex_model.c',
    album_resources,
  ],
  dependencies : [gtk4_dep, adw_dep, m_dep, poppler_dep],
  install : true,
)

# ─── 截图悬浮便签模块（独立可执行；portal 截图 + 悬浮窗口 + 投递到相册）───
snip_resources = gnome.compile_resources(
  'snip-resources',
  'src/snip.gresource.xml',
  source_dir : 'src',
  c_name : 'snip',
)

executable('notework-snip',
  sources : [
    'src/snip_main.c',
    snip_resources,
  ],
  dependencies : [gtk4_dep, adw_dep, m_dep, x11_dep],
  install : true,
)

# ─── Token 级 SAM 模块（独立可执行；同义词等价类 + DAG 可视化）───
tokensam_resources = gnome.compile_resources(
  'tokensam-resources',
  'src/tokensam.gresource.xml',
  source_dir : 'src',
  c_name : 'tokensam',
)

executable('notework-tokensam',
  sources : [
    'src/tokensam_main.c',
    'src/tokensam_window.c',
    'src/tokensam_canvas.c',
    'src/tokensam_lexicon.c',
    'src/tokensam_tokenizer.c',
    'src/tokensam_sam.c',
    'src/tokensam_layout.c',
    tokensam_resources,
  ],
  dependencies : [gtk4_dep, adw_dep, m_dep, cgraph_dep, gvc_dep],
  install : true,
)
