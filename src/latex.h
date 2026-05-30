/**
 * latex.h — 集成 LaTeX 输入 / 预览模块
 *
 * 职责：
 *  - LatexDoc：保存 LaTeX 源码字符串及最近一次成功编译的 PDF 渲染结果；
 *  - 异步编译：调用外部 xelatex (texlive-xetex) 编译至临时目录，
 *    再用 Poppler 渲染为 cairo_surface_t 数组；
 *  - 视图层：水平 Paned（左：源码 TextView，右：PDF DrawingArea）。
 *
 * 生命周期：LatexDoc 由 Album 拥有（按需创建，album_free 时释放）。
 */
#pragma once

#include <gtk/gtk.h>
#include <cairo.h>

G_BEGIN_DECLS

typedef struct LatexDoc LatexDoc;

LatexDoc *latex_doc_new (void);
void      latex_doc_free(LatexDoc *d);

/* 源码：内部 g_strdup 一份；NULL 等价于空串 */
void        latex_doc_set_source(LatexDoc *d, const char *src);
const char *latex_doc_get_source(LatexDoc *d);

/* 异步编译：内部 g_subprocess_new xelatex；
 * 完成回调通过 GAsyncReadyCallback (source=LatexDoc *)；
 * 用 latex_doc_compile_finish(d, res, &err) 取结果。 */
void     latex_doc_compile_async  (LatexDoc *d,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback cb,
                                    gpointer user_data);
gboolean latex_doc_compile_finish (LatexDoc *d, GAsyncResult *res, GError **err);

/* 取最近一次编译产物：返回 cairo_surface_t* 数组（按页）；
 * 不可修改、不要 destroy；若无产物返回 NULL 且 *out_n=0。 */
cairo_surface_t **latex_doc_get_pdf_surfaces(LatexDoc *d, int *out_n);

/* 最近一次编译报错（首行 / 概要）；无错或编译成功返回 NULL。 */
const char *latex_doc_get_last_error(LatexDoc *d);

/* ─── 视图层 ───────────────────────────────────────────────── */

/* 构造一个 GtkBox：内部含工具条 + 水平 Paned（源码 / 预览）。
 * doc 由调用方拥有，生命周期不短于该 widget。 */
GtkWidget *latex_view_new(LatexDoc *doc);

/* 选中并滚动至源码字节范围。供绑定面板"点击 LaTeX 绑定"调用。 */
void latex_view_focus_range(GtkWidget *view, int start, int length);

/* 取当前 buffer 的字节级 selection bounds：未选中返回 FALSE。
 * out_start/out_length 可传 NULL（但通常同时传）。 */
gboolean latex_view_get_selection_bytes(GtkWidget *view,
                                          int *out_start, int *out_length);

/* 取一段字节范围的字符串副本（调用方 g_free）；越界返回 NULL。 */
char *latex_view_extract_text(GtkWidget *view, int start, int length);

G_END_DECLS
