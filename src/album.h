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
} Album;

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

G_END_DECLS
