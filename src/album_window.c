/**
 * album_window.c — 相册主窗口装配
 *
 * 布局（自左向右）：
 *   1) 页面缩略图列表（GtkListBox，单选）+ 上下移/删除
 *   2) 当前页预览画布（GtkDrawingArea，cairo 直接调用 doodle_render_doc）
 *   3) 图层面板（GtkListBox 单选）+ 上下移/隐显/删除 + 跨页复制
 *   顶部：导入图片 / 导入 PDF / 涂鸦本页 / 复制图层 / 应用淡显（toggle）
 *
 * 关键交互：
 *   - 「涂鸦本页」弹出独立 doodle_window，doc 共享（own_doc=FALSE），关窗后刷新缩略图/预览。
 *   - 「应用淡显」控制预览中 doodle 层的不透明度（1.0 / 0.25）。
 *   - 「复制图层…」弹出对话框，把当前选中图层批量克隆到其他页，可指定层级位置。
 */
#include "album.h"
#include <string.h>

/* 缩略图尺寸：高 72 像素，宽自适应 */
#define ALBUM_THUMB_H 72
#define ALBUM_THUMB_W 96

typedef struct AlbumState AlbumState;

typedef enum {
    COPY_POS_TOP,            /* 目标页最顶 */
    COPY_POS_BOTTOM,         /* 目标页最底 */
    COPY_POS_ABOVE_DOODLE,   /* 当前 doodle 层之上 */
    COPY_POS_BELOW_DOODLE    /* 当前 doodle 层之下 */
} CopyPos;

struct AlbumState {
    GtkBuilder *builder;
    Album      *album;

    GtkWidget *awin;
    GtkWidget *pages_list;
    GtkWidget *layers_list;
    GtkWidget *preview_holder;
    GtkWidget *preview_canvas;
    GtkWidget *status_label;
    GtkWidget *selection_info_label;
    GtkWidget *apply_toggle;

    GtkWidget *doodle_btn;
    GtkWidget *copy_layer_btn;
    GtkWidget *page_up_btn, *page_down_btn, *page_delete_btn;
    GtkWidget *layer_up_btn, *layer_down_btn,
              *layer_visible_btn, *layer_delete_btn;
    GtkWidget *import_image_btn, *import_pdf_btn;

    /* 图层选择：自上而下的可视下标（0=最顶层），与 doc->layers 反向 */
    int selected_layer_visual;

    /* 当前打开的 doodle 弹窗（最多一个） */
    GtkWindow *doodle_win;

    /* 防止刷新引发的 row-selected 反复触发 */
    gboolean refreshing;
};

static void al_state_free(gpointer p) {
    AlbumState *s = p;
    if (!s) return;
    if (s->builder) g_object_unref(s->builder);
    if (s->album)   album_free(s->album);
    g_free(s);
}

/* 把"自上而下"的可视下标转成 doc->layers 内部下标 */
static int visual_to_doc_idx(const DoodleDoc *doc, int visual) {
    int n = doc_layer_count(doc);
    if (visual < 0 || visual >= n) return -1;
    return n - 1 - visual;
}

static int G_GNUC_UNUSED doc_to_visual_idx(const DoodleDoc *doc, int doc_idx) {
    int n = doc_layer_count(doc);
    if (doc_idx < 0 || doc_idx >= n) return -1;
    return n - 1 - doc_idx;
}

/* 把焦点 doodle 层（最顶层 doodle）设为 active */
G_GNUC_UNUSED static void focus_top_doodle_layer(DoodleDoc *doc) {
    int n = doc_layer_count(doc);
    for (int i = n - 1; i >= 0; i--) {
        Layer *L = &g_array_index(doc->layers, Layer, i);
        if (L->kind == LAYER_DOODLE) {
            doc_set_active_layer(doc, i);
            return;
        }
    }
}

/* ─── 工具：清空 GtkListBox ──────────────────────────────────── */

static void list_box_clear(GtkListBox *box) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))))
        gtk_list_box_remove(box, child);
}

/* ─── 预览画布 ───────────────────────────────────────────────── */

typedef struct {
    AlbumState *st;
} PreviewDrawCtx;

static void preview_draw_cb(GtkDrawingArea *area, cairo_t *cr,
                            int width, int height, gpointer data) {
    (void)area;
    PreviewDrawCtx *pdc = data;
    AlbumState *st = pdc->st;

    /* 清底 */
    cairo_set_source_rgb(cr, 0.96, 0.96, 0.96);
    cairo_paint(cr);

    AlbumPage *p = album_active_page(st->album);
    if (!p || !p->doc) {
        /* 提示：用 pango 渲染以正确处理 CJK 字符，避免 cairo toy text 乱码 */
        const char *txt = "请通过顶部按钮导入图片或 PDF";
        PangoLayout *layout = pango_cairo_create_layout(cr);
        PangoFontDescription *fd = pango_font_description_from_string("Sans 11");
        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);
        pango_layout_set_text(layout, txt, -1);
        int tw_p = 0, th_p = 0;
        pango_layout_get_pixel_size(layout, &tw_p, &th_p);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
        cairo_move_to(cr, (width - tw_p) * 0.5, (height - th_p) * 0.5);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
        return;
    }

    /* 估算原始内容尺寸：取 doc 中所有图片层的最大覆盖范围；若无图片则退化为画布大小 */
    double cw = 0, ch = 0;
    for (int i = 0; i < doc_layer_count(p->doc); i++) {
        Layer *L = &g_array_index(p->doc->layers, Layer, i);
        if (L->kind == LAYER_IMAGE_STUB && L->surface) {
            double s = (L->scale != 0.0) ? L->scale : 1.0;
            double rw = L->x + L->img_w * s;
            double rh = L->y + L->img_h * s;
            if (rw > cw) cw = rw;
            if (rh > ch) ch = rh;
        }
    }
    if (cw <= 0 || ch <= 0) {
        cw = (double)width;
        ch = (double)height;
    }

    /* 等比适配 */
    double sx = (double)width  / cw;
    double sy = (double)height / ch;
    double s  = MIN(sx, sy);
    if (s <= 0) s = 1.0;

    int draw_w = (int)(cw * s);
    int draw_h = (int)(ch * s);
    int ox = (width  - draw_w) / 2;
    int oy = (height - draw_h) / 2;

    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, s, s);

    /* 已应用 → 涂鸦淡显；未应用 → 涂鸦不变 */
    double alpha = p->applied ? 0.25 : 1.0;
    /* 用户的 toggle（apply_toggle） active 时保持淡显语义 */
    if (st->apply_toggle &&
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->apply_toggle))) {
        alpha = 0.25;
    } else if (st->apply_toggle &&
        !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->apply_toggle))) {
        alpha = 1.0;
    }

    doodle_render_doc(cr, p->doc, alpha, (int)cw, (int)ch);
    cairo_restore(cr);
}

/* ─── 缩略图行 ───────────────────────────────────────────────── */

typedef struct {
    AlbumState *st;
    int         page_idx;
} ThumbCtx;

static void thumb_draw_cb(GtkDrawingArea *area, cairo_t *cr,
                          int width, int height, gpointer data) {
    (void)area;
    ThumbCtx *tc = data;
    AlbumPage *p = album_get_page(tc->st->album, tc->page_idx);
    cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
    cairo_paint(cr);
    if (!p || !p->doc) return;

    double cw = 0, ch = 0;
    for (int i = 0; i < doc_layer_count(p->doc); i++) {
        Layer *L = &g_array_index(p->doc->layers, Layer, i);
        if (L->kind == LAYER_IMAGE_STUB && L->surface) {
            double s = (L->scale != 0.0) ? L->scale : 1.0;
            double rw = L->x + L->img_w * s;
            double rh = L->y + L->img_h * s;
            if (rw > cw) cw = rw;
            if (rh > ch) ch = rh;
        }
    }
    if (cw <= 0 || ch <= 0) { cw = width; ch = height; }
    double s = MIN((double)width / cw, (double)height / ch);
    if (s <= 0) s = 1.0;
    int draw_w = (int)(cw * s);
    int draw_h = (int)(ch * s);
    int ox = (width - draw_w) / 2;
    int oy = (height - draw_h) / 2;

    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, s, s);
    doodle_render_doc(cr, p->doc, p->applied ? 0.25 : 1.0,
                       (int)cw, (int)ch);
    cairo_restore(cr);
}

static GtkWidget *build_page_row(AlbumState *st, int idx) {
    AlbumPage *p = album_get_page(st->album, idx);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start (row, 6);
    gtk_widget_set_margin_end   (row, 6);
    gtk_widget_set_margin_top   (row, 4);
    gtk_widget_set_margin_bottom(row, 4);

    GtkWidget *thumb = gtk_drawing_area_new();
    gtk_widget_set_size_request(thumb, ALBUM_THUMB_W, ALBUM_THUMB_H);
    ThumbCtx *tc = g_new0(ThumbCtx, 1);
    tc->st = st; tc->page_idx = idx;
    g_object_set_data_full(G_OBJECT(thumb), "thumb-ctx", tc, g_free);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(thumb),
                                    thumb_draw_cb, tc, NULL);
    gtk_box_append(GTK_BOX(row), thumb);

    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vb, TRUE);
    char *t = g_strdup_printf("第%d页", idx + 1);
    GtkWidget *l1 = gtk_label_new(t);
    g_free(t);
    gtk_label_set_xalign(GTK_LABEL(l1), 0.0);
    gtk_widget_add_css_class(l1, "heading");
    gtk_box_append(GTK_BOX(vb), l1);

    GtkWidget *l2 = gtk_label_new(p ? p->title : "");
    gtk_label_set_xalign(GTK_LABEL(l2), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(l2), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(l2, "dim-label");
    gtk_box_append(GTK_BOX(vb), l2);
    gtk_box_append(GTK_BOX(row), vb);

    return row;
}

/* ─── 图层行 ─────────────────────────────────────────────────── */

static GtkWidget *build_layer_row(int visual_idx, const Layer *L) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start (row, 8);
    gtk_widget_set_margin_end   (row, 8);
    gtk_widget_set_margin_top   (row, 4);
    gtk_widget_set_margin_bottom(row, 4);

    const char *tag = (L->kind == LAYER_DOODLE) ? "[涂鸦]" : "[图片]";
    GtkWidget *tlbl = gtk_label_new(tag);
    gtk_widget_set_size_request(tlbl, 60, -1);
    gtk_label_set_xalign(GTK_LABEL(tlbl), 0.0);
    if (!L->visible) gtk_widget_add_css_class(tlbl, "dim-label");
    gtk_box_append(GTK_BOX(row), tlbl);

    GtkWidget *nl = gtk_label_new(L->name ? L->name : "(unnamed)");
    gtk_widget_set_hexpand(nl, TRUE);
    gtk_label_set_xalign(GTK_LABEL(nl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(nl), PANGO_ELLIPSIZE_END);
    if (!L->visible) gtk_widget_add_css_class(nl, "dim-label");
    gtk_box_append(GTK_BOX(row), nl);

    char *ix = g_strdup_printf("#%d", visual_idx);
    GtkWidget *ixl = gtk_label_new(ix);
    g_free(ix);
    gtk_widget_add_css_class(ixl, "dim-label");
    gtk_box_append(GTK_BOX(row), ixl);
    return row;
}

/* ─── 刷新 ──────────────────────────────────────────────────── */

static void refresh_status(AlbumState *st) {
    int n = album_page_count(st->album);
    char *t;
    if (n <= 0) {
        t = g_strdup("尚未导入任何页");
    } else {
        AlbumPage *p = album_active_page(st->album);
        t = g_strdup_printf("共 %d 页 · 当前第 %d 页 · %s",
            n, st->album->active + 1,
            p && p->title ? p->title : "未命名");
    }
    gtk_label_set_text(GTK_LABEL(st->status_label), t);
    g_free(t);
}

/* 状态栏下方：展示当前选中图层的详细信息。
 * 右栏表项可能被压缩裁断，本行以中央列完整宽度呈现。 */
static void refresh_selection_info(AlbumState *st) {
    if (!st || !st->selection_info_label) return;
    AlbumPage *p = album_active_page(st->album);
    if (!p || !p->doc) {
        gtk_label_set_text(GTK_LABEL(st->selection_info_label), "");
        return;
    }
    int n = doc_layer_count(p->doc);
    int v = st->selected_layer_visual;
    if (v < 0 || v >= n) {
        gtk_label_set_text(GTK_LABEL(st->selection_info_label),
                            "当前未选中图层");
        return;
    }
    int di = visual_to_doc_idx(p->doc, v);
    if (di < 0) {
        gtk_label_set_text(GTK_LABEL(st->selection_info_label), "");
        return;
    }
    Layer *L = &g_array_index(p->doc->layers, Layer, di);
    const char *kind = (L->kind == LAYER_DOODLE) ? "涂鸦" : "图片";
    const char *vis  = L->visible ? "可见" : "已隐藏";
    char *txt = NULL;
    if (L->kind == LAYER_DOODLE) {
        txt = g_strdup_printf(
            "选中图层 #%d  [%s]  %s  ·  %s  ·  形状 %u 个",
            v, kind, L->name ? L->name : "(unnamed)", vis,
            (unsigned)L->store.n);
    } else {
        txt = g_strdup_printf(
            "选中图层 #%d  [%s]  %s  ·  %s  ·  %d×%d",
            v, kind, L->name ? L->name : "(unnamed)", vis,
            (int)L->img_w, (int)L->img_h);
    }
    gtk_label_set_text(GTK_LABEL(st->selection_info_label), txt);
    g_free(txt);
}

static void refresh_pages_list(AlbumState *st) {
    st->refreshing = TRUE;
    list_box_clear(GTK_LIST_BOX(st->pages_list));
    int n = album_page_count(st->album);
    for (int i = 0; i < n; i++)
        gtk_list_box_append(GTK_LIST_BOX(st->pages_list),
                             build_page_row(st, i));
    if (n > 0 && st->album->active >= 0) {
        GtkListBoxRow *r = gtk_list_box_get_row_at_index(
            GTK_LIST_BOX(st->pages_list), st->album->active);
        if (r) gtk_list_box_select_row(GTK_LIST_BOX(st->pages_list), r);
    }
    st->refreshing = FALSE;
}

static void refresh_layers_list(AlbumState *st) {
    st->refreshing = TRUE;
    list_box_clear(GTK_LIST_BOX(st->layers_list));
    AlbumPage *p = album_active_page(st->album);
    if (!p || !p->doc) {
        st->refreshing = FALSE;
        return;
    }
    int n = doc_layer_count(p->doc);
    for (int v = 0; v < n; v++) {
        int di = visual_to_doc_idx(p->doc, v);
        Layer *L = &g_array_index(p->doc->layers, Layer, di);
        gtk_list_box_append(GTK_LIST_BOX(st->layers_list),
                             build_layer_row(v, L));
    }
    /* 选中：如果尚未选但有图层，默认选顶层（可视下标 0），
     * 否则“复制图层”等依赖选中的按钮会一直是灰色。 */
    if (st->selected_layer_visual < 0 && n > 0) {
        st->selected_layer_visual = 0;
    }
    if (st->selected_layer_visual >= n) {
        st->selected_layer_visual = n - 1;
    }
    if (st->selected_layer_visual >= 0 &&
        st->selected_layer_visual < n) {
        GtkListBoxRow *r = gtk_list_box_get_row_at_index(
            GTK_LIST_BOX(st->layers_list), st->selected_layer_visual);
        if (r) gtk_list_box_select_row(GTK_LIST_BOX(st->layers_list), r);
    }
    st->refreshing = FALSE;
    refresh_selection_info(st);
}

static void refresh_buttons_sensitive(AlbumState *st) {
    int n = album_page_count(st->album);
    gboolean has = n > 0;
    gtk_widget_set_sensitive(st->doodle_btn,       has);
    gtk_widget_set_sensitive(st->page_up_btn,      has && st->album->active > 0);
    gtk_widget_set_sensitive(st->page_down_btn,    has && st->album->active < n - 1);
    gtk_widget_set_sensitive(st->page_delete_btn,  has);
    gtk_widget_set_sensitive(st->apply_toggle,     has);

    AlbumPage *p = album_active_page(st->album);
    int ln = p && p->doc ? doc_layer_count(p->doc) : 0;
    gboolean has_layer = (st->selected_layer_visual >= 0 &&
                          st->selected_layer_visual < ln);
    gtk_widget_set_sensitive(st->layer_up_btn,      has_layer && st->selected_layer_visual > 0);
    gtk_widget_set_sensitive(st->layer_down_btn,    has_layer && st->selected_layer_visual < ln - 1);
    gtk_widget_set_sensitive(st->layer_visible_btn, has_layer);
    gtk_widget_set_sensitive(st->layer_delete_btn,  has_layer && ln > 1);
    gtk_widget_set_sensitive(st->copy_layer_btn,    has_layer && album_page_count(st->album) > 1);
}

static void refresh_all(AlbumState *st) {
    refresh_pages_list(st);
    refresh_layers_list(st);
    refresh_status(st);
    refresh_buttons_sensitive(st);
    if (st->preview_canvas) gtk_widget_queue_draw(st->preview_canvas);
}

/* ─── 信号回调 ──────────────────────────────────────────────── */

static void on_pages_row_selected(GtkListBox *box, GtkListBoxRow *row,
                                  gpointer data) {
    AlbumState *st = data;
    if (st->refreshing) return;
    if (!row) return;
    int i = gtk_list_box_row_get_index(row);
    if (i == st->album->active) return;
    album_set_active(st->album, i);
    st->selected_layer_visual = -1;
    refresh_layers_list(st);
    refresh_status(st);
    refresh_buttons_sensitive(st);
    gtk_widget_queue_draw(st->preview_canvas);
    /* 刷新左侧已选行 row 高亮（不需要重建）*/
    (void)box;
}

static void on_layers_row_selected(GtkListBox *box, GtkListBoxRow *row,
                                   gpointer data) {
    AlbumState *st = data;
    if (st->refreshing) return;
    st->selected_layer_visual = row ? gtk_list_box_row_get_index(row) : -1;
    refresh_buttons_sensitive(st);
    refresh_selection_info(st);
    (void)box;
}

static void on_apply_toggle(GtkToggleButton *btn, gpointer data) {
    (void)btn;
    AlbumState *st = data;
    /* 同步当前页 applied 标记 */
    AlbumPage *p = album_active_page(st->album);
    if (p) p->applied = gtk_toggle_button_get_active(btn);
    /* 缩略图也跟着刷新 */
    refresh_pages_list(st);
    if (st->preview_canvas) gtk_widget_queue_draw(st->preview_canvas);
}

/* 进入 doodle 编辑期的会话上下文：用于关窗时实现
 * “未画即未应用 → 删层；画了 → 命名为当前页” 的语义。 */
typedef struct {
    AlbumState *st;
    AlbumPage  *page_ref;       /* 进入时的页 ptr（编辑期不应被删除/移动） */
    DoodleDoc  *doc_ref;        /* 进入时的 doc ptr，关窗时直接操作 */
    gboolean    inserted_layer; /* TRUE: 进入时无涂鸦层，临时插入了一个 */
    gsize       snapshot_n;     /* 进入时 active 涂鸦层的 shape 数量 */
} DoodleEditCtx;

static void doodle_edit_ctx_free(gpointer p) {
    g_free(p);
}

/* 由 doodle 弹窗关闭时回调：刷新一切 */
static gboolean on_doodle_close_request(GtkWindow *win, gpointer data) {
    DoodleEditCtx *ctx = data;
    AlbumState *st = ctx->st;
    DoodleDoc  *doc = ctx->doc_ref;
    AlbumPage  *p   = ctx->page_ref;

    /* 在关窗时定位最顶层的涂鸦层（编辑期可能被新增/删除） */
    int top_doodle_idx = -1;
    int n = doc_layer_count(doc);
    for (int i = n - 1; i >= 0; i--) {
        Layer *L = &g_array_index(doc->layers, Layer, i);
        if (L->kind == LAYER_DOODLE) { top_doodle_idx = i; break; }
    }

    if (top_doodle_idx >= 0) {
        Layer  *L     = &g_array_index(doc->layers, Layer, top_doodle_idx);
        gsize   cur_n = L->store.n;
        gboolean changed = (cur_n != ctx->snapshot_n);
        gboolean empty   = (cur_n == 0);

        if (ctx->inserted_layer && empty) {
            /* 用户没画 → 删除本次临时插入的空涂鸦层（=未应用） */
            doc_remove_layer(doc, top_doodle_idx);
        } else if (changed && p) {
            /* 用户做了变更 → 按当前页名重命名（=已应用） */
            char *nm = g_strdup_printf("%s · 涂鸦",
                p->title ? p->title : "涂鸦");
            g_free(L->name);
            L->name = nm;
        }
    }

    /* 编辑结束 → 标记当前页为已应用（即使未画，也保持原值不强制） */
    if (p) p->applied = TRUE;
    if (st->apply_toggle)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->apply_toggle), TRUE);
    st->doodle_win = NULL;
    refresh_all(st);
    (void)win;
    return FALSE; /* 允许关闭 */
}

static void on_doodle_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    AlbumState *st = data;
    AlbumPage *p = album_active_page(st->album);
    if (!p || !p->doc) return;
    if (st->doodle_win) {
        gtk_window_present(st->doodle_win);
        return;
    }
    /* 准备进入编辑期的上下文：
     *  - 若页面没有涂鸦层，临时插入一个空涂鸦层供作画。
     *  - 记录进入时 active 涂鸦层的 shape 数量，用于判断是否“画过”。 */
    int top_doodle_idx = -1;
    {
        int nlayers = doc_layer_count(p->doc);
        for (int i = nlayers - 1; i >= 0; i--) {
            Layer *L = &g_array_index(p->doc->layers, Layer, i);
            if (L->kind == LAYER_DOODLE) { top_doodle_idx = i; break; }
        }
    }
    DoodleEditCtx *ctx = g_new0(DoodleEditCtx, 1);
    ctx->st       = st;
    ctx->page_ref = p;
    ctx->doc_ref  = p->doc;
    if (top_doodle_idx < 0) {
        /* 未应用状态下进入 doodle 窗 → 临时插入空涂鸦层，
         * 名称取当前页（若未画东西，关窗时会被删除）。 */
        char *nm = g_strdup_printf("%s · 涂鸦",
            p->title ? p->title : "涂鸦");
        Layer dl = layer_new_doodle_value(nm);
        g_free(nm);
        int append_at = doc_layer_count(p->doc); /* 最顶层 */
        doc_insert_layer_at(p->doc, append_at, dl);
        doc_set_active_layer(p->doc, append_at);
        ctx->inserted_layer = TRUE;
        ctx->snapshot_n     = 0;
    } else {
        Layer *L = &g_array_index(p->doc->layers, Layer, top_doodle_idx);
        doc_set_active_layer(p->doc, top_doodle_idx);
        ctx->inserted_layer = FALSE;
        ctx->snapshot_n     = L->store.n;
    }
    /* 编辑前清除 applied，让用户看到清晰的涂鸦 */
    p->applied = FALSE;

    GtkWidget *dwin = doodle_window_new_for_doc(p->doc, FALSE);
    /* 按页面图片尺寸设置 doodle 窗口默认大小，让内容不需缩放即可完整可见。
     * 添加一个 chrome 估算（左工具列 + 右图层面板 + 头栏），
     * 再以当前显示器可用区 92% 为上限。 */
    {
        double cw = 0, ch = 0;
        for (int i = 0; i < doc_layer_count(p->doc); i++) {
            Layer *L = &g_array_index(p->doc->layers, Layer, i);
            if (L->kind == LAYER_IMAGE_STUB && L->surface) {
                double s = (L->scale != 0.0) ? L->scale : 1.0;
                double rw = L->x + L->img_w * s;
                double rh = L->y + L->img_h * s;
                if (rw > cw) cw = rw;
                if (rh > ch) ch = rh;
            }
        }
        const int CHROME_W = 360;  /* 左工具栏 + 右图层面板 + 边距 */
        const int CHROME_H = 90;   /* HeaderBar + 状态栏 + 边距 */
        int tw = (cw > 0) ? (int)cw + CHROME_W : 1100;
        int th = (ch > 0) ? (int)ch + CHROME_H : 760;

        GdkRectangle geom = {0, 0, 1920, 1080};
        GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(st->awin));
        if (surf) {
            GdkDisplay *disp = gdk_surface_get_display(surf);
            GdkMonitor *mon = disp
                ? gdk_display_get_monitor_at_surface(disp, surf) : NULL;
            if (mon) gdk_monitor_get_geometry(mon, &geom);
        }
        int max_w = (int)(geom.width  * 0.92);
        int max_h = (int)(geom.height * 0.92);
        if (tw > max_w) tw = max_w;
        if (th > max_h) th = max_h;
        if (tw < 800) tw = 800;
        if (th < 600) th = 600;
        gtk_window_set_default_size(GTK_WINDOW(dwin), tw, th);
    }
    /* 关联到当前应用 */
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(st->awin));
    if (app) gtk_window_set_application(GTK_WINDOW(dwin), app);
    gtk_window_set_transient_for(GTK_WINDOW(dwin), GTK_WINDOW(st->awin));
    gtk_window_set_modal(GTK_WINDOW(dwin), FALSE);
    g_signal_connect_data(dwin, "close-request",
                          G_CALLBACK(on_doodle_close_request),
                          ctx, (GClosureNotify)doodle_edit_ctx_free, 0);
    st->doodle_win = GTK_WINDOW(dwin);
    gtk_window_present(GTK_WINDOW(dwin));
}

static void on_page_up_clicked(GtkButton *b, gpointer data) {
    (void)b;
    AlbumState *st = data;
    int i = st->album->active;
    if (i <= 0) return;
    album_move_page(st->album, i, i - 1);
    refresh_all(st);
}

static void on_page_down_clicked(GtkButton *b, gpointer data) {
    (void)b;
    AlbumState *st = data;
    int n = album_page_count(st->album);
    int i = st->album->active;
    if (i < 0 || i >= n - 1) return;
    album_move_page(st->album, i, i + 1);
    refresh_all(st);
}

static void on_page_delete_clicked(GtkButton *b, gpointer data) {
    (void)b;
    AlbumState *st = data;
    int i = st->album->active;
    if (i < 0) return;
    album_remove_page(st->album, i);
    st->selected_layer_visual = -1;
    refresh_all(st);
}

static void on_layer_up_clicked(GtkButton *b, gpointer data) {
    (void)b;
    AlbumState *st = data;
    AlbumPage *p = album_active_page(st->album);
    if (!p || !p->doc) return;
    int v = st->selected_layer_visual;
    if (v <= 0) return;
    int from = visual_to_doc_idx(p->doc, v);
    int to   = visual_to_doc_idx(p->doc, v - 1);
    doc_move_layer(p->doc, from, to);
    st->selected_layer_visual = v - 1;
    refresh_layers_list(st);
    refresh_buttons_sensitive(st);
    gtk_widget_queue_draw(st->preview_canvas);
    refresh_pages_list(st);
}

static void on_layer_down_clicked(GtkButton *b, gpointer data) {
    (void)b;
    AlbumState *st = data;
    AlbumPage *p = album_active_page(st->album);
    if (!p || !p->doc) return;
    int v = st->selected_layer_visual;
    int n = doc_layer_count(p->doc);
    if (v < 0 || v >= n - 1) return;
    int from = visual_to_doc_idx(p->doc, v);
    int to   = visual_to_doc_idx(p->doc, v + 1);
    doc_move_layer(p->doc, from, to);
    st->selected_layer_visual = v + 1;
    refresh_layers_list(st);
    refresh_buttons_sensitive(st);
    gtk_widget_queue_draw(st->preview_canvas);
    refresh_pages_list(st);
}

static void on_layer_delete_clicked(GtkButton *b, gpointer data) {
    (void)b;
    AlbumState *st = data;
    AlbumPage *p = album_active_page(st->album);
    if (!p || !p->doc) return;
    int v = st->selected_layer_visual;
    if (v < 0) return;
    int di = visual_to_doc_idx(p->doc, v);
    doc_remove_layer(p->doc, di);
    /* 视觉位置尽量保持：选中同位置或下一行 */
    int n = doc_layer_count(p->doc);
    if (st->selected_layer_visual >= n) st->selected_layer_visual = n - 1;
    refresh_layers_list(st);
    refresh_buttons_sensitive(st);
    gtk_widget_queue_draw(st->preview_canvas);
    refresh_pages_list(st);
}

static void on_layer_visible_clicked(GtkButton *b, gpointer data) {
    (void)b;
    AlbumState *st = data;
    AlbumPage *p = album_active_page(st->album);
    if (!p || !p->doc) return;
    int v = st->selected_layer_visual;
    if (v < 0) return;
    int di = visual_to_doc_idx(p->doc, v);
    Layer *L = &g_array_index(p->doc->layers, Layer, di);
    L->visible = !L->visible;
    refresh_layers_list(st);
    gtk_widget_queue_draw(st->preview_canvas);
    refresh_pages_list(st);
}

/* ─── 文件选择：导入图片 / PDF ───────────────────────────────── */

typedef struct {
    AlbumState *st;
    gboolean    pdf_only;
} ImportPickCtx;

/* 进度回调：在导入期间实时更新右侧 status_label，
 * 并 yield 主循环以使文本实际被重绘。 */
static void on_import_progress(const char *stage,
                                const char *file_basename,
                                int file_idx, int file_total,
                                int page_idx, int page_total,
                                gpointer user_data) {
    AlbumState *st = user_data;
    if (!st || !st->status_label) return;

    char *txt = NULL;
    const char *bn = file_basename ? file_basename : "?";
    if (g_strcmp0(stage, "pdf") == 0) {
        if (page_total > 0) {
            txt = g_strdup_printf("正在转换 PDF：%s · 第 %d/%d 页",
                                   bn, page_idx, page_total);
        } else {
            txt = g_strdup_printf("正在打开 PDF（%d/%d）：%s…",
                                   file_idx, file_total, bn);
        }
    } else { /* image */
        txt = g_strdup_printf("正在导入图片（%d/%d）：%s",
                               file_idx, file_total, bn);
    }
    gtk_label_set_text(GTK_LABEL(st->status_label), txt);
    g_free(txt);

    /* 立即驱动一轮重绘，否则状态文本可能要等本页渲染完才可见 */
    while (g_main_context_iteration(NULL, FALSE)) {}
}

static void import_files_finish(GObject *src, GAsyncResult *res, gpointer data) {
    ImportPickCtx *ipc = data;
    AlbumState *st = ipc->st;
    GError *err = NULL;
    GListModel *files = gtk_file_dialog_open_multiple_finish(
        GTK_FILE_DIALOG(src), res, &err);
    if (!files) {
        if (err && !g_error_matches(err, GTK_DIALOG_ERROR,
                                     GTK_DIALOG_ERROR_DISMISSED))
            g_warning("文件选择失败: %s", err->message);
        if (err) g_error_free(err);
        g_free(ipc);
        return;
    }

    guint n = g_list_model_get_n_items(files);
    GFile **arr = g_new0(GFile *, n);
    for (guint i = 0; i < n; i++)
        arr[i] = G_FILE(g_list_model_get_item(files, i));

    /* 导入可能是重负载同步操作（特别是多页 PDF），
     * 临时禁用导入按钮并切换为 wait 光标，避免用户以为窗口崩溃 */
    gtk_widget_set_sensitive(st->import_image_btn, FALSE);
    gtk_widget_set_sensitive(st->import_pdf_btn,   FALSE);
    GdkCursor *busy = gdk_cursor_new_from_name("wait", NULL);
    gtk_widget_set_cursor(GTK_WIDGET(st->awin), busy);
    if (busy) g_object_unref(busy);
    /* 立即刷一帧，使 wait 光标生效 */
    while (g_main_context_iteration(NULL, FALSE)) {}

    int added = album_import_files(st->album, arr, (int)n,
                                    on_import_progress, st);

    for (guint i = 0; i < n; i++) g_object_unref(arr[i]);
    g_free(arr);
    g_object_unref(files);

    gtk_widget_set_cursor(GTK_WIDGET(st->awin), NULL);
    gtk_widget_set_sensitive(st->import_image_btn, TRUE);
    gtk_widget_set_sensitive(st->import_pdf_btn,   TRUE);

    if (added > 0 && st->album->active < 0)
        album_set_active(st->album, 0);
    refresh_all(st);
    g_free(ipc);
}

static void start_import_dialog(AlbumState *st, gboolean pdf_only) {
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg,
        pdf_only ? "选择 PDF 文件（可多选）" : "选择图片文件（可多选）");

    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *flt = gtk_file_filter_new();
    if (pdf_only) {
        gtk_file_filter_set_name(flt, "PDF (*.pdf)");
        gtk_file_filter_add_pattern(flt, "*.pdf");
        gtk_file_filter_add_pattern(flt, "*.PDF");
    } else {
        gtk_file_filter_set_name(flt, "图片 (jpg/png/webp/...)");
        gtk_file_filter_add_mime_type(flt, "image/jpeg");
        gtk_file_filter_add_mime_type(flt, "image/png");
        gtk_file_filter_add_mime_type(flt, "image/webp");
        gtk_file_filter_add_mime_type(flt, "image/bmp");
        gtk_file_filter_add_mime_type(flt, "image/tiff");
    }
    g_list_store_append(filters, flt);
    g_object_unref(flt);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    g_object_unref(filters);

    ImportPickCtx *ipc = g_new0(ImportPickCtx, 1);
    ipc->st = st; ipc->pdf_only = pdf_only;
    gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(st->awin), NULL,
                                   import_files_finish, ipc);
    g_object_unref(dlg);
}

static void on_import_image_clicked(GtkButton *b, gpointer data) {
    (void)b; start_import_dialog((AlbumState *)data, FALSE);
}

static void on_import_pdf_clicked(GtkButton *b, gpointer data) {
    (void)b; start_import_dialog((AlbumState *)data, TRUE);
}

/* ─── 复制图层对话框（前向声明） ─────────────────────────────── */

static void open_copy_layer_dialog(AlbumState *st);

static void on_copy_layer_clicked(GtkButton *b, gpointer data) {
    (void)b;
    AlbumState *st = data;
    open_copy_layer_dialog(st);
}

/* ─── 装配 ──────────────────────────────────────────────────── */

static GtkWidget *grab(GtkBuilder *b, const char *id) {
    return GTK_WIDGET(gtk_builder_get_object(b, id));
}

GtkWidget *album_window_new(void) {
    AlbumState *st = g_new0(AlbumState, 1);
    st->album = album_new();
    st->selected_layer_visual = -1;

    GtkBuilder *b = gtk_builder_new_from_resource(
        "/com/github/notework/album/album_window.ui");
    st->builder = b;
    st->awin            = grab(b, "awin");
    st->pages_list      = grab(b, "pages_list");
    st->layers_list     = grab(b, "layers_list");
    st->preview_holder  = grab(b, "preview_holder");
    st->status_label    = grab(b, "status_label");
    st->selection_info_label = grab(b, "selection_info_label");
    st->apply_toggle    = grab(b, "apply_toggle");
    st->doodle_btn      = grab(b, "doodle_btn");
    st->copy_layer_btn  = grab(b, "copy_layer_btn");
    st->page_up_btn     = grab(b, "page_up_btn");
    st->page_down_btn   = grab(b, "page_down_btn");
    st->page_delete_btn = grab(b, "page_delete_btn");
    st->layer_up_btn    = grab(b, "layer_up_btn");
    st->layer_down_btn  = grab(b, "layer_down_btn");
    st->layer_visible_btn = grab(b, "layer_visible_btn");
    st->layer_delete_btn  = grab(b, "layer_delete_btn");
    st->import_image_btn  = grab(b, "import_image_btn");
    st->import_pdf_btn    = grab(b, "import_pdf_btn");

    /* 预览画布：每页中央占位 */
    st->preview_canvas = gtk_drawing_area_new();
    gtk_widget_set_hexpand(st->preview_canvas, TRUE);
    gtk_widget_set_vexpand(st->preview_canvas, TRUE);
    PreviewDrawCtx *pdc = g_new0(PreviewDrawCtx, 1);
    pdc->st = st;
    g_object_set_data_full(G_OBJECT(st->preview_canvas), "preview-ctx",
                            pdc, g_free);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(st->preview_canvas),
                                    preview_draw_cb, pdc, NULL);
    gtk_box_append(GTK_BOX(st->preview_holder), st->preview_canvas);

    /* 信号 */
    g_signal_connect(st->pages_list,  "row-selected",
                     G_CALLBACK(on_pages_row_selected),  st);
    g_signal_connect(st->layers_list, "row-selected",
                     G_CALLBACK(on_layers_row_selected), st);
    g_signal_connect(st->import_image_btn, "clicked",
                     G_CALLBACK(on_import_image_clicked), st);
    g_signal_connect(st->import_pdf_btn,   "clicked",
                     G_CALLBACK(on_import_pdf_clicked),   st);
    g_signal_connect(st->doodle_btn,       "clicked",
                     G_CALLBACK(on_doodle_clicked),       st);
    g_signal_connect(st->apply_toggle,     "toggled",
                     G_CALLBACK(on_apply_toggle),         st);
    g_signal_connect(st->copy_layer_btn,   "clicked",
                     G_CALLBACK(on_copy_layer_clicked),   st);
    g_signal_connect(st->page_up_btn,      "clicked",
                     G_CALLBACK(on_page_up_clicked),      st);
    g_signal_connect(st->page_down_btn,    "clicked",
                     G_CALLBACK(on_page_down_clicked),    st);
    g_signal_connect(st->page_delete_btn,  "clicked",
                     G_CALLBACK(on_page_delete_clicked),  st);
    g_signal_connect(st->layer_up_btn,     "clicked",
                     G_CALLBACK(on_layer_up_clicked),     st);
    g_signal_connect(st->layer_down_btn,   "clicked",
                     G_CALLBACK(on_layer_down_clicked),   st);
    g_signal_connect(st->layer_visible_btn,"clicked",
                     G_CALLBACK(on_layer_visible_clicked),st);
    g_signal_connect(st->layer_delete_btn, "clicked",
                     G_CALLBACK(on_layer_delete_clicked), st);

    g_object_set_data_full(G_OBJECT(st->awin), "album-state",
                            st, al_state_free);

    refresh_all(st);
    return st->awin;
}

/* ─── 复制图层对话框（实现） ─────────────────────────────────── */

typedef struct {
    AlbumState *st;
    GtkWindow  *dlg;
    GtkWidget  *check_all;
    GArray     *checks; /* GtkCheckButton*，长度 = 页数；index 与页 idx 对齐 */
    GtkWidget  *pos_combo;
} CopyDlg;

static void copy_dlg_free(gpointer p) {
    CopyDlg *d = p;
    if (!d) return;
    if (d->checks) g_array_free(d->checks, TRUE);
    g_free(d);
}

static void on_copy_dlg_check_all(GtkCheckButton *cb, gpointer data) {
    CopyDlg *d = data;
    gboolean on = gtk_check_button_get_active(cb);
    for (guint i = 0; i < d->checks->len; i++) {
        GtkCheckButton *c = g_array_index(d->checks, GtkCheckButton *, i);
        gtk_check_button_set_active(c, on);
    }
}

static void on_copy_dlg_cancel(GtkButton *b, gpointer data) {
    (void)b;
    CopyDlg *d = data;
    gtk_window_destroy(d->dlg);
}

static void on_copy_dlg_apply(GtkButton *b, gpointer data) {
    (void)b;
    CopyDlg *d = data;
    AlbumState *st = d->st;
    AlbumPage *src = album_active_page(st->album);
    if (!src || !src->doc) { gtk_window_destroy(d->dlg); return; }
    int v = st->selected_layer_visual;
    int di = visual_to_doc_idx(src->doc, v);
    if (di < 0) { gtk_window_destroy(d->dlg); return; }

    guint pos_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->pos_combo));
    int pos_choice = (pos_sel == GTK_INVALID_LIST_POSITION) ? COPY_POS_TOP : (int)pos_sel;

    int n_pages = album_page_count(st->album);
    int copied = 0;
    for (int i = 0; i < n_pages; i++) {
        if (i == st->album->active) continue;
        if (i >= (int)d->checks->len) break;
        GtkCheckButton *c = g_array_index(d->checks, GtkCheckButton *, i);
        if (!gtk_check_button_get_active(c)) continue;

        AlbumPage *dst = album_get_page(st->album, i);
        if (!dst || !dst->doc) continue;

        Layer cl = doc_clone_layer_value(src->doc, di);
        int target_pos = 0;
        int dn = doc_layer_count(dst->doc);
        switch ((CopyPos)pos_choice) {
        case COPY_POS_BOTTOM:
            target_pos = 0;
            break;
        case COPY_POS_TOP:
            target_pos = dn;
            break;
        case COPY_POS_BELOW_DOODLE:
            /* 找最顶 doodle 层，插在其下方（doc_idx = top_doodle 处） */
            target_pos = 0;
            for (int k = dn - 1; k >= 0; k--) {
                Layer *L = &g_array_index(dst->doc->layers, Layer, k);
                if (L->kind == LAYER_DOODLE) { target_pos = k; break; }
            }
            break;
        case COPY_POS_ABOVE_DOODLE:
            /* 插在最顶 doodle 层之上（doc_idx = top_doodle + 1） */
            target_pos = dn;
            for (int k = dn - 1; k >= 0; k--) {
                Layer *L = &g_array_index(dst->doc->layers, Layer, k);
                if (L->kind == LAYER_DOODLE) { target_pos = k + 1; break; }
            }
            break;
        }
        doc_insert_layer_at(dst->doc, target_pos, cl);
        copied++;
    }

    gtk_window_destroy(d->dlg);
    refresh_all(st);
    char *msg = g_strdup_printf("已复制到 %d 页", copied);
    gtk_label_set_text(GTK_LABEL(st->status_label), msg);
    g_free(msg);
}

static void open_copy_layer_dialog(AlbumState *st) {
    AlbumPage *src = album_active_page(st->album);
    if (!src) return;
    int v = st->selected_layer_visual;
    if (v < 0) return;
    int di = visual_to_doc_idx(src->doc, v);
    if (di < 0) return;
    Layer *L = &g_array_index(src->doc->layers, Layer, di);

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "复制图层到其他页");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(st->awin));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 380, 460);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start (root, 14);
    gtk_widget_set_margin_end   (root, 14);
    gtk_widget_set_margin_top   (root, 14);
    gtk_widget_set_margin_bottom(root, 14);
    gtk_window_set_child(GTK_WINDOW(dlg), root);

    char *desc = g_strdup_printf("源：第 %d 页 · %s [%s]",
        st->album->active + 1,
        L->kind == LAYER_DOODLE ? "涂鸦层" : "图片层",
        L->name ? L->name : "");
    GtkWidget *hd = gtk_label_new(desc);
    g_free(desc);
    gtk_label_set_xalign(GTK_LABEL(hd), 0.0);
    gtk_widget_add_css_class(hd, "heading");
    gtk_box_append(GTK_BOX(root), hd);

    /* 位置下拉 */
    GtkWidget *pos_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *pos_lbl = gtk_label_new("插入位置：");
    gtk_box_append(GTK_BOX(pos_box), pos_lbl);
    const char *pos_strings[] = {
        "最顶（覆盖在所有图层之上）",
        "最底",
        "当前 doodle 层之上",
        "当前 doodle 层之下",
        NULL
    };
    GtkWidget *pos_combo = gtk_drop_down_new_from_strings(pos_strings);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(pos_combo), COPY_POS_TOP);
    gtk_widget_set_hexpand(pos_combo, TRUE);
    gtk_box_append(GTK_BOX(pos_box), pos_combo);
    gtk_box_append(GTK_BOX(root), pos_box);

    /* 全选 */
    GtkWidget *check_all = gtk_check_button_new_with_label("全选");
    gtk_box_append(GTK_BOX(root), check_all);

    /* 目标页列表 */
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sw, TRUE);
    GtkWidget *list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), list_box);
    gtk_box_append(GTK_BOX(root), sw);

    CopyDlg *d = g_new0(CopyDlg, 1);
    d->st = st;
    d->dlg = GTK_WINDOW(dlg);
    d->check_all = check_all;
    d->pos_combo = pos_combo;
    d->checks = g_array_new(FALSE, FALSE, sizeof(GtkCheckButton *));

    int n = album_page_count(st->album);
    for (int i = 0; i < n; i++) {
        AlbumPage *p = album_get_page(st->album, i);
        char *t = g_strdup_printf("第%d页 · %s",
            i + 1, p && p->title ? p->title : "");
        GtkWidget *cb = gtk_check_button_new_with_label(t);
        g_free(t);
        if (i == st->album->active)
            gtk_widget_set_sensitive(cb, FALSE); /* 不允许复制到自己 */
        gtk_box_append(GTK_BOX(list_box), cb);
        GtkCheckButton *cbb = GTK_CHECK_BUTTON(cb);
        g_array_append_val(d->checks, cbb);
    }

    /* 按钮 */
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("取消");
    GtkWidget *apply  = gtk_button_new_with_label("复制");
    gtk_widget_add_css_class(apply, "suggested-action");
    gtk_box_append(GTK_BOX(btns), cancel);
    gtk_box_append(GTK_BOX(btns), apply);
    gtk_box_append(GTK_BOX(root), btns);

    g_signal_connect(check_all, "toggled",
                     G_CALLBACK(on_copy_dlg_check_all), d);
    g_signal_connect(cancel, "clicked",
                     G_CALLBACK(on_copy_dlg_cancel), d);
    g_signal_connect(apply,  "clicked",
                     G_CALLBACK(on_copy_dlg_apply),  d);

    g_object_set_data_full(G_OBJECT(dlg), "copy-dlg", d, copy_dlg_free);
    gtk_window_present(GTK_WINDOW(dlg));
}
