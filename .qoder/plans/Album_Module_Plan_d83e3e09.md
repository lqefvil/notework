# Album 模块开发计划

## 目标
独立可执行 `notework-album`：批量导入 jpeg/png + PDF 拆页，页排序，对每页涂鸦（应用后涂鸦层淡显），图层（涂鸦/图片）跨页批量复制并设层级，图层可再编辑/删除。

## 总体架构
- 复用：[doodle_canvas](file:///home/admin01/notework/src/doodle.h#L131)、[doodle_model](file:///home/admin01/notework/src/doodle_model.c)（涂鸦层 Shape/编号体系不动）
- 扩展：现有 [LAYER_IMAGE_STUB](file:///home/admin01/notework/src/doodle.h#L52) 升级为持有真实 `GdkTexture` 的图片层；[Layer](file:///home/admin01/notework/src/doodle.h#L55-L60) 增加复制/插入/移动 API
- 新增：相册壳层（页集合 / 导入器 / 缩略图列表 / 图层面板 / 跨页复制对话框）

## 数据模型
- 新增 `src/album.h`：
  ```
  typedef struct { DoodleDoc *doc; char *src_uri; } AlbumPage;
  typedef struct { GArray *pages; int active; } Album;
  ```
- 扩展 [Layer](file:///home/admin01/notework/src/doodle.h#L55-L60)（向后兼容，新增字段）：
  ```
  GdkTexture *texture;          /* 仅 LAYER_IMAGE_STUB 用 */
  double tex_w, tex_h;          /* 像素尺寸 */
  double x, y, scale;           /* 画布定位 */
  ```
- 扩展 doodle API（[doodle.h](file:///home/admin01/notework/src/doodle.h)）：
  - `Layer *doc_clone_layer(const DoodleDoc*, int idx)` / `void doc_insert_layer_at(DoodleDoc*, int pos, Layer*)`
  - `void doc_remove_layer_at(DoodleDoc*, int idx)` / `void doc_move_layer(DoodleDoc*, int from, int to)`
  - `void doodle_canvas_set_doodle_alpha(GtkWidget*, double)`：编辑期 1.0、应用后 0.25

## 任务

### Task 1：依赖与构建骨架
- [meson.build](file:///home/admin01/notework/meson.build) 增加 `poppler_dep = dependency('poppler-glib', version : '>=21')`
- 新增 `src/album.gresource.xml` + `src/album_window.ui`
- 新增 executable `notework-album`，源文件：`album_main.c / album_window.c / album_model.c / album_importer.c / doodle_window.c / doodle_canvas.c / doodle_model.c`，依赖 `[gtk4_dep, adw_dep, m_dep, poppler_dep]`

### Task 2：扩展 doodle 渲染与图层 API
- [doodle.h](file:///home/admin01/notework/src/doodle.h)：追加上文所列字段与函数声明
- [doodle_model.c](file:///home/admin01/notework/src/doodle_model.c)：新增 `doc_clone_layer / doc_insert_layer_at / doc_remove_layer_at / doc_move_layer`；克隆涂鸦层用 [shape_clone](file:///home/admin01/notework/src/doodle_model.c#L43)；图片层 `g_object_ref(texture)`
- [doodle_canvas.c](file:///home/admin01/notework/src/doodle_canvas.c) 渲染分派：遇 `LAYER_IMAGE_STUB && texture != NULL` 时用 `gdk_texture_download` 或 `gdk_cairo_set_source_pixbuf` 替代方案，简单做法：保存 `cairo_surface_t` 缓存；涂鸦层绘制时整体乘 `doodle_alpha`

### Task 3：导入器（src/album_importer.c）
- 公共入口 `void album_import_files(Album*, char **uris, int n, GAsyncReadyCallback)`
- jpeg/png：`gdk_texture_new_from_filename`，每个文件 → 1 个新 page（doc 内含 1 个图片层 + 1 个空涂鸦层）
- PDF：`poppler_document_new_from_gfile` → 逐页 `poppler_page_render` 到 `cairo_image_surface` → 转 `GdkTexture`，每页 1 个 page
- 失败文件跳过并 g_warning

### Task 4：相册主窗口（src/album_window.c + album_window.ui）
- 布局：HeaderBar + 主分栏
  - HeaderBar：「导入图片」「导入 PDF」「涂鸦本页」「应用」「复制图层…」
  - 左：`GtkListView` 页缩略图（128px），用 `GtkDragSource/DropTarget` 实现拖排序，回写 `Album.pages` 顺序
  - 中：`GtkStack`：`preview`（GtkPicture 显示当前页 cairo 渲染快照） / `editor`（[doodle_view_new](file:///home/admin01/notework/src/doodle.h#L161) 嵌入，doc 用当前 page->doc）
  - 右：图层面板（GtkListBox）：列出当前页 layers（顶层在前），每行：[类型] [名] [可见性 toggle] [↑] [↓] [✕]
- 状态切换：「涂鸦本页」→ stack 切 editor、`set_doodle_alpha(1.0)`；「应用」→ 切回 preview、`set_doodle_alpha(0.25)`、刷新缩略图
- 涂鸦层放在图片层之上：复用现有 [doc_insert_image_stub_below_active](file:///home/admin01/notework/src/doodle_model.c#L429-L438) 语义

### Task 5：跨页图层复制对话框
- 入口：右侧图层选中后点 HeaderBar「复制图层…」
- AdwDialog：
  - 目标页多选（GtkCheckButton 列表 + 全选）
  - 层级选项：放置「最顶 / 最底 / 当前涂鸦层之上 / 当前涂鸦层之下」
- 确认后：`l = doc_clone_layer(src_doc, sel_idx)` → 对每个目标 page，按层级位置 `doc_insert_layer_at`

### Task 6：图层编辑/删除
- 图层面板的 ↑↓ → `doc_move_layer`
- ✕ → `doc_remove_layer_at`（active_layer 校正）
- 双击涂鸦层 → 等价点「涂鸦本页」并把 [active_layer](file:///home/admin01/notework/src/doodle.h#L64) 设为该层
- 双击图片层 → 弹出小对话框改 `x/y/scale`（极简，留待后续完善）

### Task 7：入口程序与样式
- `src/album_main.c`：仿 [doodle_main.c](file:///home/admin01/notework/src/doodle_main.c) 的 AdwApplication 启动器
- 复用 [src/style.css](file:///home/admin01/notework/src/style.css)，album.gresource.xml 引入

## 关键风险与对策
- **GdkTexture 与 Cairo 互转**：用 `cairo_image_surface_create` + `poppler_page_render` 出 surface，再 `gdk_memory_texture_new` 转 texture；缩略图直接用 surface 缩放
- **缩略图性能**：异步生成 + LRU 缓存（按页 id），不阻塞 UI
- **页删除时 doc 释放**：`doodle_doc_free` 已存在；图片层 texture 在 `layer_clear` 中需 `g_object_unref`
- **doodle 编号 vs 跨页复制**：克隆涂鸦层时编号空间随层走（每页独立），无冲突

## 不在本期范围
- 撤销/重做（doodle 当前也未做）
- 文件保存/加载（仅运行期内存模型）
- 图片层旋转/裁剪（仅支持位移与等比缩放）
