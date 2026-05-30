/**
 * latex_window.c — LaTeX 编辑 / 预览视图
 *
 * 从 /com/github/notework/latex/latex_window.ui 装配，绑定到 LatexDoc。
 * 源码 buffer 改动后 500ms 防抖触发 latex_doc_compile_async；
 * 编译成功后将 PDF surface 数组绘制到右侧 DrawingArea（按页竖向排列）。
 */
#include "latex.h"
#include <string.h>

#define DEBOUNCE_MS 500

typedef struct {
    LatexDoc       *doc;
    GtkWidget      *root;        /* GtkBox */
    GtkTextBuffer  *buffer;
    GtkTextView    *src_view;
    GtkDrawingArea *pdf_area;
    GtkLabel       *status_label;
    GtkExpander    *err_expander;
    GtkLabel       *err_label;
    GtkScrolledWindow *pdf_scroll;
    GtkTextTag     *focus_tag;
    guint           debounce_id;
    gboolean        compiling;
    gboolean        suppress_changed;
} LatexView;

static void latex_view_free(gpointer p) {
    LatexView *v = p;
    if (!v) return;
    if (v->debounce_id) { g_source_remove(v->debounce_id); v->debounce_id = 0; }
    g_free(v);
}

static LatexView *view_of(GtkWidget *w) {
    return g_object_get_data(G_OBJECT(w), "latex-view");
}

static void schedule_redraw(LatexView *v) {
    if (!v || !v->pdf_area) return;
    /* 按总高度更新 content-height，让滚动条显示完整 */
    int n = 0;
    cairo_surface_t **arr = latex_doc_get_pdf_surfaces(v->doc, &n);
    if (n > 0 && arr) {
        int total_h = 0, max_w = 0;
        for (int i = 0; i < n; i++) {
            int w = cairo_image_surface_get_width(arr[i]);
            int h = cairo_image_surface_get_height(arr[i]);
            total_h += h + (i ? 12 : 0);
            if (w > max_w) max_w = w;
        }
        gtk_drawing_area_set_content_width (v->pdf_area, max_w);
        gtk_drawing_area_set_content_height(v->pdf_area, total_h);
    }
    gtk_widget_queue_draw(GTK_WIDGET(v->pdf_area));
}

static void on_pdf_draw(GtkDrawingArea *area, cairo_t *cr,
                         int width, int height, gpointer user_data) {
    (void)area; (void)width; (void)height;
    LatexView *v = user_data;
    cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
    cairo_paint(cr);
    int n = 0;
    cairo_surface_t **arr = latex_doc_get_pdf_surfaces(v->doc, &n);
    if (!arr || n == 0) return;
    double y = 0;
    for (int i = 0; i < n; i++) {
        cairo_surface_t *s = arr[i];
        int w = cairo_image_surface_get_width(s);
        int h = cairo_image_surface_get_height(s);
        double x = (width - w) * 0.5;
        if (x < 0) x = 0;
        /* 阴影底 */
        cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
        cairo_rectangle(cr, x + 2, y + 2, w, h);
        cairo_fill(cr);
        cairo_set_source_surface(cr, s, x, y);
        cairo_paint(cr);
        y += h + 12;
    }
}

static void set_status(LatexView *v, const char *txt) {
    if (v->status_label) gtk_label_set_text(v->status_label, txt ? txt : "");
}

static void on_compile_done(GObject *src, GAsyncResult *res, gpointer user_data) {
    (void)src;
    LatexView *v = user_data;
    GError *err = NULL;
    gboolean ok = latex_doc_compile_finish(v->doc, res, &err);
    v->compiling = FALSE;
    if (ok) {
        set_status(v, "编译完成");
        if (v->err_expander) {
            gtk_widget_set_visible(GTK_WIDGET(v->err_expander), FALSE);
            gtk_expander_set_expanded(v->err_expander, FALSE);
        }
        if (v->err_label) gtk_label_set_text(v->err_label, "");
    } else {
        const char *e = latex_doc_get_last_error(v->doc);
        set_status(v, "编译失败");
        if (v->err_label)    gtk_label_set_text(v->err_label, e ? e : (err ? err->message : "未知错误"));
        if (v->err_expander) {
            gtk_widget_set_visible(GTK_WIDGET(v->err_expander), TRUE);
            gtk_expander_set_expanded(v->err_expander, TRUE);
        }
    }
    g_clear_error(&err);
    schedule_redraw(v);
}

static void start_compile(LatexView *v) {
    if (!v || v->compiling) return;
    /* 同步 buffer → doc */
    GtkTextIter a, b;
    gtk_text_buffer_get_start_iter(v->buffer, &a);
    gtk_text_buffer_get_end_iter  (v->buffer, &b);
    char *txt = gtk_text_buffer_get_text(v->buffer, &a, &b, FALSE);
    latex_doc_set_source(v->doc, txt);
    g_free(txt);

    v->compiling = TRUE;
    set_status(v, "编译中…");
    latex_doc_compile_async(v->doc, NULL, on_compile_done, v);
}

static gboolean debounce_tick(gpointer user_data) {
    LatexView *v = user_data;
    v->debounce_id = 0;
    start_compile(v);
    return G_SOURCE_REMOVE;
}

static void on_buffer_changed(GtkTextBuffer *buf, gpointer user_data) {
    (void)buf;
    LatexView *v = user_data;
    if (v->suppress_changed) return;
    if (v->debounce_id) g_source_remove(v->debounce_id);
    v->debounce_id = g_timeout_add(DEBOUNCE_MS, debounce_tick, v);
}

static void on_compile_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    LatexView *v = user_data;
    if (v->debounce_id) { g_source_remove(v->debounce_id); v->debounce_id = 0; }
    start_compile(v);
}

/* ─── 字节 ↔ TextIter 转换 ───────────────────────────────── */

/* 取 buffer 完整字节串 → 在第 byte_off 处构造 TextIter（按 UTF-8）。 */
static gboolean byte_offset_to_iter(GtkTextBuffer *buf, int byte_off,
                                      GtkTextIter *out) {
    GtkTextIter a, b;
    gtk_text_buffer_get_start_iter(buf, &a);
    gtk_text_buffer_get_end_iter  (buf, &b);
    char *txt = gtk_text_buffer_get_text(buf, &a, &b, FALSE);
    int n = (int)strlen(txt);
    if (byte_off < 0) byte_off = 0;
    if (byte_off > n) byte_off = n;
    /* 把字节偏移转为字符偏移 */
    int char_off = (int)g_utf8_pointer_to_offset(txt, txt + byte_off);
    g_free(txt);
    gtk_text_buffer_get_iter_at_offset(buf, out, char_off);
    return TRUE;
}

static int iter_to_byte_offset(GtkTextBuffer *buf, const GtkTextIter *it) {
    GtkTextIter s;
    gtk_text_buffer_get_start_iter(buf, &s);
    char *txt = gtk_text_buffer_get_slice(buf, &s, it, TRUE);
    int n = (int)strlen(txt);
    g_free(txt);
    return n;
}

/* ─── 对外 API ───────────────────────────────────────────── */

GtkWidget *latex_view_new(LatexDoc *doc) {
    g_return_val_if_fail(doc != NULL, NULL);

    GtkBuilder *b = gtk_builder_new_from_resource(
        "/com/github/notework/latex/latex_window.ui");
    GtkWidget *root = GTK_WIDGET(gtk_builder_get_object(b, "latex_root"));
    g_object_ref_sink(root);

    LatexView *v = g_new0(LatexView, 1);
    v->doc          = doc;
    v->root         = root;
    v->src_view     = GTK_TEXT_VIEW(gtk_builder_get_object(b, "src_view"));
    v->pdf_area     = GTK_DRAWING_AREA(gtk_builder_get_object(b, "pdf_area"));
    v->status_label = GTK_LABEL(gtk_builder_get_object(b, "status_label"));
    v->err_expander = GTK_EXPANDER(gtk_builder_get_object(b, "err_expander"));
    v->err_label    = GTK_LABEL(gtk_builder_get_object(b, "err_label"));
    v->pdf_scroll   = GTK_SCROLLED_WINDOW(gtk_builder_get_object(b, "pdf_scroll"));
    v->buffer       = gtk_text_view_get_buffer(v->src_view);

    /* 初始 buffer 填入 doc->source */
    v->suppress_changed = TRUE;
    const char *src = latex_doc_get_source(doc);
    gtk_text_buffer_set_text(v->buffer, src ? src : "", -1);
    v->suppress_changed = FALSE;

    /* 焦点高亮 tag（点击 LaTeX 绑定时使用） */
    v->focus_tag = gtk_text_buffer_create_tag(v->buffer, "binding-focus",
        "background", "#b3d9ff",
        NULL);

    /* 信号 */
    g_signal_connect(v->buffer, "changed",
        G_CALLBACK(on_buffer_changed), v);
    GtkButton *btn = GTK_BUTTON(gtk_builder_get_object(b, "btn_compile"));
    g_signal_connect(btn, "clicked", G_CALLBACK(on_compile_clicked), v);
    gtk_drawing_area_set_draw_func(v->pdf_area, on_pdf_draw, v, NULL);

    g_object_set_data_full(G_OBJECT(root), "latex-view", v, latex_view_free);
    g_object_unref(b);
    return root;
}

void latex_view_focus_range(GtkWidget *view, int start, int length) {
    LatexView *v = view_of(view);
    if (!v) return;

    /* 清除旧 focus tag */
    GtkTextIter a, b;
    gtk_text_buffer_get_start_iter(v->buffer, &a);
    gtk_text_buffer_get_end_iter  (v->buffer, &b);
    gtk_text_buffer_remove_tag(v->buffer, v->focus_tag, &a, &b);

    if (start < 0 || length <= 0) return;  /* 仅清除 */

    GtkTextIter s, e;
    if (!byte_offset_to_iter(v->buffer, start, &s)) return;
    if (!byte_offset_to_iter(v->buffer, start + length, &e)) return;
    gtk_text_buffer_apply_tag(v->buffer, v->focus_tag, &s, &e);
    gtk_text_buffer_select_range(v->buffer, &s, &e);
    gtk_text_view_scroll_to_iter(v->src_view, &s, 0.1, FALSE, 0.0, 0.0);
}

gboolean latex_view_get_selection_bytes(GtkWidget *view,
                                          int *out_start, int *out_length) {
    LatexView *v = view_of(view);
    if (!v) return FALSE;
    GtkTextIter a, b;
    if (!gtk_text_buffer_get_selection_bounds(v->buffer, &a, &b))
        return FALSE;
    int sa = iter_to_byte_offset(v->buffer, &a);
    int sb = iter_to_byte_offset(v->buffer, &b);
    if (sa == sb) return FALSE;
    if (out_start)  *out_start  = sa;
    if (out_length) *out_length = sb - sa;
    return TRUE;
}

char *latex_view_extract_text(GtkWidget *view, int start, int length) {
    LatexView *v = view_of(view);
    if (!v) return NULL;
    if (start < 0 || length <= 0) return NULL;
    GtkTextIter s, e;
    if (!byte_offset_to_iter(v->buffer, start, &s)) return NULL;
    if (!byte_offset_to_iter(v->buffer, start + length, &e)) return NULL;
    return gtk_text_buffer_get_text(v->buffer, &s, &e, FALSE);
}
