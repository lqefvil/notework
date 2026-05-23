/**
 * album_importer.c — 图片/PDF 导入器
 *
 * 设计要点：
 *  - 同步实现，简单可靠；批量数量不大时性能足够。
 *  - 图片：用 GdkPixbuf 加载（覆盖 jpeg/png/webp 等所有 gdk-pixbuf-loaders 支持的格式），
 *    再用 cairo + gdk_cairo_set_source_pixbuf 拷到 ARGB32 surface。
 *  - PDF：poppler_document_new_from_gfile + poppler_page_render，按 2x 倍率渲染
 *    以保证清晰度。
 *  - 加载失败的文件被跳过并 g_warning，不阻塞批量流程。
 */
#include "album.h"
#include <math.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <poppler.h>

/* PDF 渲染倍率：兼顾清晰度与内存占用。
 * 1.5x 下 A4 页约 893x1262，多页也不至于令主线程长时间卡死。 */
#define ALBUM_PDF_RENDER_SCALE 1.5

/* ─── 图片 ─────────────────────────────────────────────────────── */

cairo_surface_t *album_load_image_surface(GFile *file, GError **error) {
    char *path = g_file_get_path(file);
    if (!path) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "non-local file uri not supported");
        return NULL;
    }

    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, error);
    g_free(path);
    if (!pb) return NULL;

    int w = gdk_pixbuf_get_width (pb);
    int h = gdk_pixbuf_get_height(pb);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);
    /* 白底，避免无 alpha 的 jpeg 被当成透明 */
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    g_object_unref(pb);
    return surf;
}

/* ─── PDF ──────────────────────────────────────────────────────── */

cairo_surface_t **album_load_pdf_surfaces(GFile *file, int *out_n,
                                           GError **error,
                                           AlbumProgressFunc progress,
                                           gpointer user_data) {
    if (out_n) *out_n = 0;
    PopplerDocument *pdoc = poppler_document_new_from_gfile(
        file, NULL, NULL, error);
    if (!pdoc) return NULL;

    int n = poppler_document_get_n_pages(pdoc);
    if (n <= 0) {
        g_object_unref(pdoc);
        return NULL;
    }

    char *bn = g_file_get_basename(file);

    cairo_surface_t **arr = g_new0(cairo_surface_t *, n);
    for (int i = 0; i < n; i++) {
        /* 页面渲染前报告进度：让 UI 能在本页开始前刷新为 “X/N” */
        if (progress)
            progress("pdf", bn, 0, 1, i + 1, n, user_data);

        PopplerPage *pp = poppler_document_get_page(pdoc, i);
        if (!pp) {
            g_warning("PDF 第 %d 页加载失败，跳过", i + 1);
            continue;
        }
        double pw, ph;
        poppler_page_get_size(pp, &pw, &ph);
        int rw = (int)ceil(pw * ALBUM_PDF_RENDER_SCALE);
        int rh = (int)ceil(ph * ALBUM_PDF_RENDER_SCALE);
        if (rw <= 0 || rh <= 0) { g_object_unref(pp); continue; }

        cairo_surface_t *surf = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, rw, rh);
        cairo_t *cr = cairo_create(surf);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_paint(cr);
        cairo_scale(cr, ALBUM_PDF_RENDER_SCALE, ALBUM_PDF_RENDER_SCALE);
        poppler_page_render(pp, cr);
        cairo_destroy(cr);
        cairo_surface_flush(surf);
        g_object_unref(pp);
        arr[i] = surf;

        /* 让主循环有机会处理重绘/输入事件，避免多页 PDF 同步渲染期间
         * 被桌面判为“未响应”（该调用不会堆积堆栈，安全）。 */
        while (g_main_context_iteration(NULL, FALSE)) {}
    }
    g_free(bn);
    g_object_unref(pdoc);
    if (out_n) *out_n = n;
    return arr;
}

/* ─── 批量入口 ─────────────────────────────────────────────────── */

static gboolean is_pdf_file(GFile *f) {
    char *bn = g_file_get_basename(f);
    if (!bn) return FALSE;
    char *low = g_ascii_strdown(bn, -1);
    g_free(bn);
    gboolean is_pdf = g_str_has_suffix(low, ".pdf");
    g_free(low);
    return is_pdf;
}

int album_import_files(Album *a, GFile **files, int n_files,
                        AlbumProgressFunc progress, gpointer user_data) {
    if (!a || !files || n_files <= 0) return 0;
    int added = 0;

    for (int i = 0; i < n_files; i++) {
        GFile *f = files[i];
        if (!f) continue;
        char *display = g_file_get_basename(f);
        char *uri     = g_file_get_uri(f);

        if (is_pdf_file(f)) {
            /* PDF 开始阶段先报 0/0，ui 可先显示“打开 PDF…” */
            if (progress)
                progress("pdf", display, i + 1, n_files, 0, 0, user_data);

            int n = 0;
            GError *err = NULL;
            cairo_surface_t **surfs = album_load_pdf_surfaces(
                f, &n, &err, progress, user_data);
            if (!surfs) {
                g_warning("PDF 加载失败 [%s]: %s", display ? display : "?",
                          err ? err->message : "unknown");
                if (err) g_error_free(err);
                g_free(display); g_free(uri);
                continue;
            }
            for (int p = 0; p < n; p++) {
                if (!surfs[p]) continue;
                char *title = g_strdup_printf("%s · 第%d页",
                    display ? display : "PDF", p + 1);
                char *src   = g_strdup_printf("%s#page=%d",
                    uri ? uri : "", p + 1);
                album_append_page_from_surface(a, surfs[p], title, src);
                cairo_surface_destroy(surfs[p]);
                g_free(title); g_free(src);
                added++;
            }
            g_free(surfs);
        } else {
            if (progress)
                progress("image", display, i + 1, n_files, 0, 0, user_data);

            GError *err = NULL;
            cairo_surface_t *surf = album_load_image_surface(f, &err);
            if (!surf) {
                g_warning("图片加载失败 [%s]: %s", display ? display : "?",
                          err ? err->message : "unknown");
                if (err) g_error_free(err);
                g_free(display); g_free(uri);
                continue;
            }
            album_append_page_from_surface(a, surf,
                display ? display : "图片", uri);
            cairo_surface_destroy(surf);
            added++;
            /* 图片完成后也 yield 一下，保证多文件连续导入时能刷新状态栏 */
            while (g_main_context_iteration(NULL, FALSE)) {}
        }

        g_free(display); g_free(uri);
    }
    return added;
}
