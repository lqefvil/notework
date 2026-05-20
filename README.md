# Notework

基于 **GTK 4 + libadwaita** 的 Linux 桌面应用，目标功能包括现代化窗口控件、PDF 文档渲染（Poppler + Cairo）、LaTeX 文档预览、全文搜索（SQLite FTS5）以及多语言支持。

## 技术栈

- **语言**：C11（`-Wall -Wextra -Wpedantic`）
- **GUI**：GTK 4.10+ / libadwaita 1.0+
- **绘制**：Cairo 1.18+
- **构建**：Meson + Ninja
- **资源**：GResource（XML 声明式布局：`window.ui` / `row.ui`）

## 依赖

Ubuntu / Debian 安装命令：

```bash
sudo apt install build-essential meson ninja-build pkg-config \
                 libgtk-4-dev libadwaita-1-dev libxml2-utils
```

> `libxml2-utils` 提供 `xmllint`，GResource 编译时启用 `xml-stripblanks` 需要它。

## 构建与运行

```bash
# 配置构建目录
meson setup builddir

# 编译
ninja -C builddir

# 运行
./builddir/notework
```

### 虚拟机环境运行提示

在 VMware 等虚拟机中运行时，若出现窗口缩放或滚动出现白色方块残影，需切换到 Cairo 纯 CPU 渲染后端：

```bash
GSK_RENDERER=cairo ./builddir/notework
```

## 目录结构

```
notework/
├── meson.build              # Meson 构建脚本
└── src/
    ├── main.c               # 程序入口
    ├── window.ui            # 主窗口 GtkBuilder XML
    ├── row.ui               # 行控件 GtkBuilder XML
    ├── style.css            # 样式表
    └── notework.gresource.xml  # GResource 描述
```

## 许可证

待定。
