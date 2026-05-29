/**
 * album.h — 多页相册/PDF 涂鸦模块对外接口
 *
 * 职责：
 *  - 维护多页文档：每页一个 DoodleDoc（含图片层 + 涂鸦层）。
 *  - 提供导入器（jpeg/png/PDF）、跨页图层复制、页排序。
 *  - 提供 album 主窗口（独立可执行 notework-album）。
 *
 * 复用 doodle 模块的 DoodleDoc/Layer/Shape 体系；本模块只引入额外的
 * "页集合"概念，单页编辑直接弹出 doodle_window_new_for_doc()。
 */
#pragma once

#include "doodle.h"

G_BEGIN_DECLS

/* ─── 数据模型 ─────────────────────────────────────────────────── */

typedef struct {
    DoodleDoc *doc;       /* 由 album 拥有：含 1 个图片层 + 1 个涂鸦层 */
    char      *src_uri;   /* 来源（GFile uri 或 "pdf://...#page=K"），可选 */
    char      *title;     /* 页面标题（用于 UI 显示） */
    gboolean   applied;   /* TRUE 表示用户已经"应用"过涂鸦：预览淡显 */
} AlbumPage;

typedef struct {
    GArray *pages;        /* AlbumPage（按值存储） */
    int     active;       /* 当前选中页下标；空相册时为 -1 */
    GArray *tracks;       /* Track（按值存储）。本期仅骨架，后期演进为
                           * 全局轨道集合：跨页面聅合高亮区域 + 起止指针对。 */
} Album;

/* 一对把手 = 一段「激活区域」。a_arc/b_arc 为全局弧长（贯穿
 * 整本相册的弧长域，0..total_arc）。约定 a_arc <= b_arc。
 * bindings: 可选的高亮绑定列表，元素类型 guint64（HighlightRecord.id）；
 * 仅存 hl_id，不存 page_idx/layer_idx/rec_idx，消除下标漂移。可能为 NULL。 */
typedef struct {
    double  a_arc;
    double  b_arc;
    GArray *bindings;   /* GArray<guint64>；可为 NULL，懒初始化 */
} HandlePair;

/* 轨道：名称 + 0..N 对把手。每对把手在轨道行上以两个三角把手 +
 * 半透明色块表达，色块横贯进度轴宽度（与轴坐标系对齐）。 */
typedef struct {
    char   *name;
    GArray *pairs;   /* HandlePair（按值存储）；可能为空 */
} Track;

/* 轨道把手对增删 / 修改。本期仅在内存中管理，无持久化。
 *   · album_track_add_pair: 在 track_idx 末尾追加一对，返回 pair 下标；
 *     越界返回 -1。a/b 顺序自动归一。
 *   · album_track_remove_pair: 越界返回 FALSE。
 *   · album_track_update_pair: 用于拖动结束后写回；越界返回 FALSE，
 *     a/b 顺序自动归一。 */
int       album_track_add_pair   (Album *a, int track_idx,
                                  double a_arc, double b_arc);
gboolean  album_track_remove_pair(Album *a, int track_idx, int pair_idx);
gboolean  album_track_update_pair(Album *a, int track_idx, int pair_idx,
                                  double a_arc, double b_arc);

/* ─── 激活区域 ↔ 高亮绑定 ────────────────────────────────
 * bindings 仅存 HighlightRecord.id。定位查找统一走
 * album_find_highlight_by_id（全局反查），避免下标漂移。 */
gboolean  album_pair_bind_highlight  (Album *a, int track_idx, int pair_idx,
                                       guint64 hl_id);
gboolean  album_pair_unbind_highlight(Album *a, int track_idx, int pair_idx,
                                       guint64 hl_id);
gboolean  album_pair_clear_bindings  (Album *a, int track_idx, int pair_idx);
GArray   *album_pair_get_bindings    (Album *a, int track_idx, int pair_idx);
gboolean  album_pair_has_binding     (Album *a, int track_idx, int pair_idx,
                                       guint64 hl_id);

/* 全局反查：在所有页的所有 LAYER_HIGHLIGHT 中查找 id == hl_id 的记录。
 * 命中返回 TRUE 并写出 (page,layer,rec) 下标；未命中返回 FALSE。
 * out_* 可传 NULL。 */
gboolean  album_find_highlight_by_id(Album *a, guint64 hl_id,
                                      int *out_page, int *out_layer, int *out_rec);

Album    *album_new(void);
void      album_free(Album *a);

int       album_page_count(const Album *a);
AlbumPage*album_get_page  (Album *a, int idx);     /* 越界返回 NULL */
AlbumPage*album_active_page(Album *a);             /* 无页时 NULL */

/* 追加一页（接管 doc 所有权）。title/src_uri 内部 g_strdup */
int       album_append_page(Album *a, DoodleDoc *doc,
                             const char *title, const char *src_uri);

void      album_remove_page(Album *a, int idx);
void      album_move_page  (Album *a, int from, int to);
void      album_set_active (Album *a, int idx);

/* 由 surface 直接构造一页：内部 doodle_doc_new + insert_image_layer */
int       album_append_page_from_surface(Album *a,
                                          cairo_surface_t *surface,
                                          const char *title,
                                          const char *src_uri);

/* ─── 导入器 ───────────────────────────────────────────────────── */

/* 进度回调：stage 如 "pdf"/"image"；page_idx/page_total 仅在 PDF 多页时 >0，
 * 其余场景为 0/0。任何参数可能为 NULL（调用者需判空）。 */
typedef void (*AlbumProgressFunc)(const char *stage,
                                   const char *file_basename,
                                   int file_idx, int file_total,
                                   int page_idx, int page_total,
                                   gpointer user_data);

/* 同步导入：每个文件追加若干页（jpeg/png 1 页；PDF N 页）。
 * 返回新追加的页数；任何失败的文件被跳过并 g_warning。
 * progress 可选，会在阶段性节点调用以供 UI 刷新状态。 */
int       album_import_files(Album *a, GFile **files, int n_files,
                              AlbumProgressFunc progress, gpointer user_data);

/* 单个 jpeg/png 文件 → cairo_surface_t（ARGB32），失败返回 NULL。
 * 调用者负责 cairo_surface_destroy。 */
cairo_surface_t *album_load_image_surface(GFile *file, GError **error);

/* PDF 文件 → cairo_surface_t 数组，每页一个；
 * *out_n 写入页数；返回 NULL 表示失败。
 * progress 可选：每渲染完一页会调用以供 UI 刷新。
 * 调用者负责 destroy 每个 surface 并 g_free 数组。 */
cairo_surface_t **album_load_pdf_surfaces(GFile *file, int *out_n,
                                           GError **error,
                                           AlbumProgressFunc progress,
                                           gpointer user_data);

/* ─── 主窗口 ───────────────────────────────────────────────────── */

GtkWidget *album_window_new(void);

/* 从 album_window_new() 返回的 window 取出内部 Album*（用于命令行/远程
 * 实例 import 文件场景）。无内置 album 时返回 NULL。 */
Album    *album_window_get_album(GtkWidget *win);

/* 通知 album 窗口刷新缩略图/图层面板（导入新页或外部修改后调用）。 */
void      album_window_refresh  (GtkWidget *win);

/* 内嵌视图：返回一个 GtkBox，包含完整的相册编辑 UI
 * （工具条 + 页缩略图 + 预览 + 图层面板）。
 * album 由调用方拥有且生命周期不短于该视图。 */
GtkWidget *album_view_new(Album *album);

/* 轨道列表的 GListModel：本期返回基于 album->tracks 的 GtkStringList
 * （仅名称字段）。后期会替换为自定义 GListModel。 */
GListModel *album_create_track_model(Album *a);

/* 轨道增删。调用后调用方需自行触发样式刷新（例如重装填轨道行）。
 *   · album_track_append: name 为 NULL 时使用默认名 "轨道 N"，
 *     N = 当前 tracks->len + 1；返回新轨道下标（追加于末尾）。
 *   · album_track_remove: 下标越界返回 FALSE。 */
int       album_track_append(Album *a, const char *name);
gboolean  album_track_remove(Album *a, int idx);

/* 变更通知：相册内容发生变动（页增删、涂鸦修改、高亮增删、
 * 图层变动等）后被调用；view 为 album_view_new()/album_window_new() 返回
 * 的控件。多次 set 会覆盖上一次；cb=NULL 取消。 */
typedef void (*AlbumChangedFn)(GtkWidget *album_view, gpointer user_data);
void album_view_set_changed_cb(GtkWidget *album_view,
                                AlbumChangedFn cb, gpointer user_data);

/* 聚焦某条高亮记录于预览画布上（跳转后的视觉提示）。
 * hl_id=0 表示清除。仅当当前页含该 hl_id 时生效；切页后仍保持该
 * 状态，只是不同页上不绘制。该 API 不会切活动页。 */
void album_view_set_focused_highlight(GtkWidget *album_view, guint64 hl_id);

G_END_DECLS
