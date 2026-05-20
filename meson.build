project('notework', 'c',
  version : '0.1.0',
  default_options : [
    'c_std=c11',
    'warning_level=3',  # -Wall -Wextra -Wpedantic
  ],
)

# 依赖
gtk4_dep = dependency('gtk4', version : '>=4.10')
adw_dep  = dependency('libadwaita-1', version : '>=1.0')

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
