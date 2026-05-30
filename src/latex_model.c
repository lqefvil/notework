/**
 * latex_model.c — LatexDoc 数据与异步编译
 *
 * 编译流程：
 *   1) 在 g_get_tmp_dir() 下创建专属临时目录（首次 lazy）；
 *   2) 把当前 source 写入 main.tex；
 *   3) g_subprocess_new("xelatex", "-interaction=nonstopmode",
 *                       "-output-directory", tmpdir, "main.tex")；
 *   4) wait_async → 完成后若 ok 用 Poppler 渲染 PDF 到 cairo surface 数组；
 *      失败则解析 main.log 取首条错误返回到 last_error。
 *
 * 线程：编译子进程异步，渲染回主线程完成（Poppler 调用在 finalize 中）。
 */
#include "latex.h"
#include <poppler.h>
#include <string.h>
#include <glib/gstdio.h>

struct LatexDoc {
    char  *source;
    char  *tmpdir;          /* lazy: 第一次编译时创建 */
    char  *last_error;      /* g_free */
    GPtrArray *surfaces;    /* cairo_surface_t*，元素 destroy = cairo_surface_destroy */
};

static void surfaces_clear(LatexDoc *d) {
    if (!d->surfaces) return;
    g_ptr_array_set_size(d->surfaces, 0);
}

LatexDoc *latex_doc_new(void) {
    LatexDoc *d = g_new0(LatexDoc, 1);
    d->source     = g_strdup("");
    d->surfaces   = g_ptr_array_new_with_free_func(
                        (GDestroyNotify)cairo_surface_destroy);
    return d;
}

static void rmrf_dir(const char *dir) {
    if (!dir) return;
    GDir *gd = g_dir_open(dir, 0, NULL);
    if (gd) {
        const char *name;
        while ((name = g_dir_read_name(gd))) {
            char *p = g_build_filename(dir, name, NULL);
            g_unlink(p);
            g_free(p);
        }
        g_dir_close(gd);
    }
    g_rmdir(dir);
}

void latex_doc_free(LatexDoc *d) {
    if (!d) return;
    g_free(d->source);
    g_free(d->last_error);
    if (d->surfaces) g_ptr_array_unref(d->surfaces);
    if (d->tmpdir) { rmrf_dir(d->tmpdir); g_free(d->tmpdir); }
    g_free(d);
}

void latex_doc_set_source(LatexDoc *d, const char *src) {
    if (!d) return;
    g_free(d->source);
    d->source = g_strdup(src ? src : "");
}

const char *latex_doc_get_source(LatexDoc *d) {
    return d ? (d->source ? d->source : "") : "";
}

const char *latex_doc_get_last_error(LatexDoc *d) {
    if (!d || !d->last_error || !*d->last_error) return NULL;
    return d->last_error;
}

cairo_surface_t **latex_doc_get_pdf_surfaces(LatexDoc *d, int *out_n) {
    if (out_n) *out_n = 0;
    if (!d || !d->surfaces || d->surfaces->len == 0) return NULL;
    if (out_n) *out_n = (int)d->surfaces->len;
    return (cairo_surface_t **)d->surfaces->pdata;
}

/* ─── 异步编译 ─────────────────────────────────────────────── */

typedef struct {
    LatexDoc *d;            /* 弱引用：与 d 同生命周期，不增引用计数 */
    char *tex_path;
    char *pdf_path;
    char *log_path;
} CompileCtx;

static void compile_ctx_free(gpointer p) {
    CompileCtx *c = p;
    g_free(c->tex_path); g_free(c->pdf_path); g_free(c->log_path);
    g_free(c);
}

/* 从 main.log 截取首条 "! ... " 错误行 */
static char *extract_first_error(const char *log_path) {
    if (!log_path) return NULL;
    gchar *content = NULL;
    if (!g_file_get_contents(log_path, &content, NULL, NULL)) return NULL;
    gchar *first = NULL;
    gchar **lines = g_strsplit(content, "\n", -1);
    for (int i = 0; lines && lines[i]; i++) {
        if (lines[i][0] == '!') {
            /* 取该行 + 下一行（通常 "l.42 ..."） */
            const char *next = lines[i+1] ? lines[i+1] : "";
            first = g_strdup_printf("%s\n%s", lines[i], next);
            break;
        }
    }
    g_strfreev(lines);
    g_free(content);
    return first;
}

/* 渲染 PDF：返回 GPtrArray<cairo_surface_t*> */
static GPtrArray *render_pdf(const char *pdf_path, char **err_out) {
    GError *gerr = NULL;
    char *uri = g_filename_to_uri(pdf_path, NULL, &gerr);
    if (!uri) {
        if (err_out) *err_out = g_strdup_printf("uri: %s",
            gerr ? gerr->message : "?");
        g_clear_error(&gerr);
        return NULL;
    }
    PopplerDocument *pd = poppler_document_new_from_file(uri, NULL, &gerr);
    g_free(uri);
    if (!pd) {
        if (err_out) *err_out = g_strdup_printf("poppler: %s",
            gerr ? gerr->message : "?");
        g_clear_error(&gerr);
        return NULL;
    }
    int n = poppler_document_get_n_pages(pd);
    GPtrArray *arr = g_ptr_array_new_with_free_func(
                        (GDestroyNotify)cairo_surface_destroy);
    const double dpi = 144.0;
    const double scale = dpi / 72.0;
    for (int i = 0; i < n; i++) {
        PopplerPage *pg = poppler_document_get_page(pd, i);
        if (!pg) continue;
        double pw, ph;
        poppler_page_get_size(pg, &pw, &ph);
        int W = (int)(pw * scale + 0.5);
        int H = (int)(ph * scale + 0.5);
        if (W <= 0 || H <= 0) { g_object_unref(pg); continue; }
        cairo_surface_t *s = cairo_image_surface_create(
                                CAIRO_FORMAT_ARGB32, W, H);
        cairo_t *cr = cairo_create(s);
        /* 白底 */
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        cairo_scale(cr, scale, scale);
        poppler_page_render(pg, cr);
        cairo_destroy(cr);
        g_ptr_array_add(arr, s);
        g_object_unref(pg);
    }
    g_object_unref(pd);
    return arr;
}

static void on_subprocess_done(GObject *src, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(user_data);
    CompileCtx *cx = g_task_get_task_data(task);
    LatexDoc *d = cx ? cx->d : NULL;

    GError *err = NULL;
    gboolean ok = g_subprocess_wait_check_finish(G_SUBPROCESS(src), res, &err);

    /* 重置上次状态 */
    g_clear_pointer(&d->last_error, g_free);
    surfaces_clear(d);

    if (!ok) {
        /* 解析 .log 取首条错误，回退到 GError 信息 */
        char *e = extract_first_error(cx->log_path);
        if (!e) e = g_strdup(err ? err->message : "xelatex 失败");
        d->last_error = e;
        g_clear_error(&err);
        g_object_unref(src);
        g_task_return_boolean(task, FALSE);
        g_object_unref(task);
        return;
    }

    /* 渲染 PDF */
    char *rerr = NULL;
    GPtrArray *arr = render_pdf(cx->pdf_path, &rerr);
    if (!arr) {
        d->last_error = rerr ? rerr : g_strdup("Poppler 渲染失败");
        g_object_unref(src);
        g_task_return_boolean(task, FALSE);
        g_object_unref(task);
        return;
    }
    /* swap 进 d->surfaces */
    for (guint i = 0; i < arr->len; i++)
        g_ptr_array_add(d->surfaces, g_ptr_array_index(arr, i));
    g_ptr_array_set_free_func(arr, NULL); /* 不释放元素 */
    g_ptr_array_unref(arr);

    g_object_unref(src);
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
}

void latex_doc_compile_async(LatexDoc *d,
                              GCancellable *cancellable,
                              GAsyncReadyCallback cb,
                              gpointer user_data) {
    /* 重要：LatexDoc 是普通 struct（非 GObject），不能当
     * GTask::source_object，否则 g_task_new 内部会按 GObject 协议
     * ref 它，造成内存破坏 / segfault。
     * 改为 source_object=NULL，把 d 携在 task data 中。 */
    GTask *task = g_task_new(NULL, cancellable, cb, user_data);

    /* 准备 tmpdir */
    if (!d->tmpdir) {
        GError *err = NULL;
        d->tmpdir = g_dir_make_tmp("notework-latex-XXXXXX", &err);
        if (!d->tmpdir) {
            g_clear_pointer(&d->last_error, g_free);
            d->last_error = g_strdup_printf("无法创建临时目录: %s",
                err ? err->message : "?");
            g_clear_error(&err);
            g_task_return_boolean(task, FALSE);
            g_object_unref(task);
            return;
        }
    }

    CompileCtx *cx = g_new0(CompileCtx, 1);
    cx->d        = d;
    cx->tex_path = g_build_filename(d->tmpdir, "main.tex", NULL);
    cx->pdf_path = g_build_filename(d->tmpdir, "main.pdf", NULL);
    cx->log_path = g_build_filename(d->tmpdir, "main.log", NULL);
    g_task_set_task_data(task, cx, compile_ctx_free);

    /* 写源码 */
    GError *err = NULL;
    if (!g_file_set_contents(cx->tex_path,
            d->source ? d->source : "", -1, &err)) {
        g_clear_pointer(&d->last_error, g_free);
        d->last_error = g_strdup_printf("写 .tex 失败: %s",
            err ? err->message : "?");
        g_clear_error(&err);
        g_task_return_boolean(task, FALSE);
        g_object_unref(task);
        return;
    }

    /* 启动 xelatex */
    GSubprocessLauncher *L = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
        G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_subprocess_launcher_set_cwd(L, d->tmpdir);
    GSubprocess *sp = g_subprocess_launcher_spawn(L, &err,
        "xelatex", "-interaction=nonstopmode",
        "-halt-on-error",
        "-output-directory", d->tmpdir,
        cx->tex_path, NULL);
    g_object_unref(L);
    if (!sp) {
        g_clear_pointer(&d->last_error, g_free);
        d->last_error = g_strdup_printf(
            "无法启动 xelatex（请安装 texlive-xetex）: %s",
            err ? err->message : "?");
        g_clear_error(&err);
        g_task_return_boolean(task, FALSE);
        g_object_unref(task);
        return;
    }
    g_subprocess_wait_check_async(sp, cancellable, on_subprocess_done, task);
}

gboolean latex_doc_compile_finish(LatexDoc *d, GAsyncResult *res, GError **err) {
    (void)d;
    g_return_val_if_fail(g_task_is_valid(res, NULL), FALSE);
    return g_task_propagate_boolean(G_TASK(res), err);
}
