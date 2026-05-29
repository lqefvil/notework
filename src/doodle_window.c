/**
 * doodle_window.c — 涂鸦窗口装配
 *
 * 从 GResource 加载 doodle_window.ui，构造 DoodleDoc / 画布 / 右侧面板，
 * 并把所有信号连接到位：
 *   - 头部工具切换 → 画布工具
 *   - 阵列参数 SpinButton → 画布预览参数
 *   - 阵列开始/应用/取消按钮 → 画布生命周期
 *   - 添加图片图层 → 文档插入占位层
 *   - 画布"模型已变更"回调 → 重建编号列表 / 图层列表 / 缺号提示
 */
#include "doodle.h"
#include <string.h>

typedef struct {
    GtkBuilder *builder;
    DoodleDoc  *doc;
    gboolean    own_doc;   /* FALSE 时释放窗口不销毁 doc */
    GtkWidget  *canvas;

    /* 引用面板控件 */
    GtkWidget *missing_label;
    GtkWidget *numbers_list;
    GtkWidget *layers_list;
    GtkWidget *ar_rows_spin;
    GtkWidget *ar_cols_spin;
    GtkWidget *ar_gx_spin;
    GtkWidget *ar_gy_spin;
    GtkWidget *ar_start_btn;
    GtkWidget *ar_apply_btn;
    GtkWidget *ar_cancel_btn;
    GtkWidget *compact_btn;

    GtkWidget *tool_line;
    GtkWidget *tool_path;
    GtkWidget *tool_erase;
    GtkWidget *tool_select;
    GtkWidget *tool_hl;
    GtkWidget *tool_hl_erase;
    GtkWidget *tool_rect;
    GtkWidget *tool_ellipse;

    /* 目标层切换 */
    GtkWidget *target_path_btn;
    GtkWidget *target_paint_btn;
    gboolean   syncing_target; /* 防 toggled 信号回环 */

    GtkWidget *hl_global_spin;
    GtkWidget *hl_local_spin;
    GtkWidget *hl_delete_btn;
    GtkWidget *pen_color_btn;
    gboolean   syncing_hl_width; /* 防 hl_local_spin 同步信号回环 */

    /* 视图缩放 */
    GtkWidget *zoom_out_btn;
    GtkWidget *zoom_reset_btn;
    GtkWidget *zoom_in_btn;
} WinState;

static void win_state_free(gpointer p) {
    WinState *s = p;
    if (s->builder) g_object_unref(s->builder);
    if (s->own_doc && s->doc) doodle_doc_free(s->doc);
    g_free(s);
}

/* ─── 通用辅助 ──────────────────────────────────────────────────── */

static gssize store_index_of(ShapeStore *st, Shape *s) {
    for (gsize i = 0; i < st->n; i++)
        if (st->items[i] == s) return (gssize)i;
    return -1;
}

static void list_box_clear(GtkListBox *box) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))))
        gtk_list_box_remove(box, child);
}

/* 把缺号数组格式化成 "5, 9-11, 14" 风格 */
static char *format_missing(GArray *m) {
    if (m->len == 0) return g_strdup("缺号: —");
    GString *gs = g_string_new("缺号: ");
    guint i = 0;
    while (i < m->len) {
        int a = g_array_index(m, int, i);
        int b = a;
        while (i + 1 < m->len && g_array_index(m, int, i + 1) == b + 1) {
            i++; b++;
        }
        if (a == b) g_string_append_printf(gs, "%d", a);
        else        g_string_append_printf(gs, "%d-%d", a, b);
        i++;
        if (i < m->len) g_string_append(gs, ", ");
    }
    return g_string_free(gs, FALSE);
}

/* ─── 编号列表行 ────────────────────────────────────────────────── */

typedef struct {
    WinState *win;
    Shape    *shape;       /* 行所属图形 */
    int       child_idx;   /* SHAPE_ARRAY 时为子项索引；否则 -1 */
    GtkWidget *spin;       /* 编号 SpinButton */
} RowCtx;

static void row_ctx_free(gpointer p) { g_free(p); }

static void refresh_panel(WinState *win); /* fwd */

static void on_row_spin_changed(GtkSpinButton *spin, gpointer data) {
    RowCtx *rc = data;
    int new_n = (int)gtk_spin_button_get_value(spin);
    if (new_n < 1) return;

    if (rc->shape->kind == SHAPE_ARRAY) {
        if (rc->child_idx >= 0)
            rc->shape->u.arr.child_numbers[rc->child_idx] = new_n;
    } else {
        shape_set_number(rc->shape, new_n);
    }
    /* 仅刷新缺号 / 重复提示与画布编号标签；不必重建整个列表 */
    GArray *miss = doc_missing_numbers(rc->win->doc);
    char *text = format_missing(miss);
    gtk_label_set_text(GTK_LABEL(rc->win->missing_label), text);
    g_free(text); g_array_free(miss, TRUE);

    if (doc_has_duplicate_numbers(rc->win->doc))
        gtk_widget_add_css_class(rc->win->missing_label, "doodle-warn-dup");
    else
        gtk_widget_remove_css_class(rc->win->missing_label, "doodle-warn-dup");

    doodle_canvas_request_redraw(rc->win->canvas);
}

static void on_row_delete(GtkButton *btn, gpointer data) {
    (void)btn;
    RowCtx *rc = data;
    ShapeStore *st = doc_active_store(rc->win->doc);
    gssize idx = store_index_of(st, rc->shape);
    if (idx < 0) return;
    /* 阵列子项行的删除按钮删除整组（按"整体组"语义） */
    int sel = doodle_canvas_get_selected_index(rc->win->canvas);
    if (sel == (int)idx) doodle_canvas_set_selected_index(rc->win->canvas, -1);
    else if (sel > (int)idx) doodle_canvas_set_selected_index(rc->win->canvas, sel - 1);
    doc_remove_shape_at(rc->win->doc, (gsize)idx);
    refresh_panel(rc->win);
    doodle_canvas_request_redraw(rc->win->canvas);
}

/* 构造单个编号行：[标签] [SpinButton] [✕] */
static GtkWidget *build_number_row(WinState *win, Shape *shape, int child_idx,
                                   const char *kind_text, int number) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start (row, 8);
    gtk_widget_set_margin_end   (row, 8);
    gtk_widget_set_margin_top   (row, 4);
    gtk_widget_set_margin_bottom(row, 4);

    GtkWidget *kind_lbl = gtk_label_new(kind_text);
    gtk_widget_set_size_request(kind_lbl, 80, -1);
    gtk_label_set_xalign(GTK_LABEL(kind_lbl), 0.0);
    gtk_box_append(GTK_BOX(row), kind_lbl);

    GtkAdjustment *adj = gtk_adjustment_new(number, 1, 100000, 1, 10, 0);
    GtkWidget *spin = gtk_spin_button_new(adj, 1, 0);
    gtk_widget_set_hexpand(spin, TRUE);
    gtk_box_append(GTK_BOX(row), spin);

    GtkWidget *del = gtk_button_new_from_icon_name("edit-delete-symbolic");
    gtk_widget_set_tooltip_text(del, "删除");
    gtk_box_append(GTK_BOX(row), del);

    RowCtx *rc = g_new0(RowCtx, 1);
    rc->win = win;
    rc->shape = shape;
    rc->child_idx = child_idx;
    rc->spin = spin;
    g_object_set_data_full(G_OBJECT(row), "row-ctx", rc, row_ctx_free);

    g_signal_connect(spin, "value-changed", G_CALLBACK(on_row_spin_changed), rc);
    g_signal_connect(del,  "clicked",       G_CALLBACK(on_row_delete),       rc);

    return row;
}

/* 把整个 doc 的图形展开为编号行（阵列展开为子项行），按"画布顺序"列出 */
static void rebuild_numbers_list(WinState *win) {
    GtkListBox *box = GTK_LIST_BOX(win->numbers_list);
    list_box_clear(box);

    ShapeStore *st = doc_active_store(win->doc);
    for (gsize i = 0; i < st->n; i++) {
        Shape *s = st->items[i];
        if (s->kind == SHAPE_LINE) {
            char *kt = g_strdup_printf("直线 #%d", s->number);
            gtk_list_box_append(box, build_number_row(win, s, -1, kt, s->number));
            g_free(kt);
        } else if (s->kind == SHAPE_PATH) {
            char *kt = g_strdup_printf("手绘 #%d", s->number);
            gtk_list_box_append(box, build_number_row(win, s, -1, kt, s->number));
            g_free(kt);
        } else { /* SHAPE_ARRAY */
            int nb = s->u.arr.n_bases;
            for (int r = 0; r < s->u.arr.rows; r++) {
                for (int c = 0; c < s->u.arr.cols; c++) {
                    for (int bi = 0; bi < nb; bi++) {
                        int ci = (r * s->u.arr.cols + c) * nb + bi;
                        int n  = s->u.arr.child_numbers[ci];
                        char *kt;
                        if (nb > 1)
                            kt = g_strdup_printf("阵 r%d,c%d/b%d", r + 1, c + 1, bi + 1);
                        else
                            kt = g_strdup_printf("阵 r%d,c%d", r + 1, c + 1);
                        gtk_list_box_append(box, build_number_row(win, s, ci, kt, n));
                        g_free(kt);
                    }
                }
            }
        }
    }
}

/* ─── 图层列表 ──────────────────────────────────────────────────── */

static GtkWidget *build_layer_row(int idx_in_array, gboolean is_active,
                                  Layer *L) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start (row, 8);
    gtk_widget_set_margin_end   (row, 8);
    gtk_widget_set_margin_top   (row, 4);
    gtk_widget_set_margin_bottom(row, 4);

    const char *tag = (L->kind == LAYER_DOODLE) ? "[涂鸦]" : "[图片]";
    GtkWidget *tlbl = gtk_label_new(tag);
    gtk_widget_set_size_request(tlbl, 60, -1);
    gtk_label_set_xalign(GTK_LABEL(tlbl), 0.0);
    gtk_box_append(GTK_BOX(row), tlbl);

    char *t = g_strdup_printf("%s%s", L->name ? L->name : "(unnamed)",
                              is_active ? "  ★" : "");
    GtkWidget *nl = gtk_label_new(t);
    g_free(t);
    gtk_widget_set_hexpand(nl, TRUE);
    gtk_label_set_xalign(GTK_LABEL(nl), 0.0);
    /* 防长名字擑宽右栏：末尾省略号。 */
    gtk_label_set_ellipsize(GTK_LABEL(nl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(nl), 16);
    gtk_box_append(GTK_BOX(row), nl);

    /* 用 idx 标注便于调试，无交互 */
    char *ix = g_strdup_printf("#%d", idx_in_array);
    GtkWidget *ixl = gtk_label_new(ix);
    g_free(ix);
    gtk_widget_add_css_class(ixl, "dim-label");
    gtk_box_append(GTK_BOX(row), ixl);
    return row;
}

static void rebuild_layers_list(WinState *win) {
    GtkListBox *box = GTK_LIST_BOX(win->layers_list);
    list_box_clear(box);
    int n = doc_layer_count(win->doc);
    /* 顶层在前：从大下标到 0 */
    for (int i = n - 1; i >= 0; i--) {
        Layer *L = &g_array_index(win->doc->layers, Layer, i);
        gtk_list_box_append(box,
            build_layer_row(i, i == win->doc->active_layer, L));
    }
}

/* ─── 综合刷新 ──────────────────────────────────────────────────── */

static void update_array_button_states(WinState *win) {
    gboolean active = doodle_canvas_is_array_active(win->canvas);
    gboolean can_start = (!active) &&
        doodle_canvas_selection_array_eligible(win->canvas);
    gtk_widget_set_sensitive(win->ar_start_btn,  can_start);
    gtk_widget_set_sensitive(win->ar_apply_btn,  active);
    gtk_widget_set_sensitive(win->ar_cancel_btn, active);
}

/* 根据画布高亮选中状态同步 hl_local_spin / hl_delete_btn 的可用性与值。 */
static void update_highlight_panel(WinState *win) {
    int li = -1, ri = -1;
    doodle_canvas_get_selected_highlight(win->canvas, &li, &ri);
    gboolean has = (li >= 0 && ri >= 0);
    gtk_widget_set_sensitive(win->hl_local_spin, has);
    if (win->hl_delete_btn)
        gtk_widget_set_sensitive(win->hl_delete_btn, has);

    if (has) {
        Layer *L = &g_array_index(win->doc->layers, Layer, li);
        if (L->kind == LAYER_HIGHLIGHT && L->highlights &&
            (guint)ri < L->highlights->len) {
            HighlightRecord *r = g_ptr_array_index(L->highlights, ri);
            if (r) {
                win->syncing_hl_width = TRUE;
                gtk_spin_button_set_value(
                    GTK_SPIN_BUTTON(win->hl_local_spin), r->width);
                win->syncing_hl_width = FALSE;
            }
        }
    }
}

static void refresh_panel(WinState *win) {
    rebuild_numbers_list(win);
    rebuild_layers_list (win);

    GArray *miss = doc_missing_numbers(win->doc);
    char *t = format_missing(miss);
    gtk_label_set_text(GTK_LABEL(win->missing_label), t);
    g_free(t); g_array_free(miss, TRUE);
    if (doc_has_duplicate_numbers(win->doc))
        gtk_widget_add_css_class   (win->missing_label, "doodle-warn-dup");
    else
        gtk_widget_remove_css_class(win->missing_label, "doodle-warn-dup");

    update_array_button_states(win);
    update_highlight_panel(win);
}

static void on_canvas_changed(GtkWidget *canvas, gpointer data) {
    (void)canvas;
    refresh_panel((WinState *)data);
}

/* ─── 信号回调 ──────────────────────────────────────────────────── */

/* 视图缩放：同步当前倍率到重置按钮文本。 */
static void zoom_sync_label(WinState *win) {
    if (!win->canvas || !win->zoom_reset_btn) return;
    double s = doodle_canvas_get_view_scale(win->canvas);
    int p = (int)(s * 100.0 + 0.5);
    char buf[16];
    g_snprintf(buf, sizeof(buf), "%d%%", p);
    gtk_button_set_label(GTK_BUTTON(win->zoom_reset_btn), buf);
}

/* 按位阶面调整倍率：25/50/75/100/125/150/200/300/400 */
static double zoom_step_next(double cur, int dir) {
    static const double steps[] = {
        0.25, 0.50, 0.75, 1.00, 1.25, 1.50, 2.00, 3.00, 4.00
    };
    int n = (int)(sizeof(steps) / sizeof(steps[0]));
    if (dir > 0) {
        for (int i = 0; i < n; i++) if (steps[i] > cur + 1e-6) return steps[i];
        return steps[n - 1];
    } else {
        for (int i = n - 1; i >= 0; i--) if (steps[i] < cur - 1e-6) return steps[i];
        return steps[0];
    }
}

static void on_zoom_out_clicked(GtkButton *b, gpointer data) {
    (void)b;
    WinState *win = data;
    double cur = doodle_canvas_get_view_scale(win->canvas);
    doodle_canvas_set_view_scale(win->canvas, zoom_step_next(cur, -1));
    zoom_sync_label(win);
}

static void on_zoom_in_clicked(GtkButton *b, gpointer data) {
    (void)b;
    WinState *win = data;
    double cur = doodle_canvas_get_view_scale(win->canvas);
    doodle_canvas_set_view_scale(win->canvas, zoom_step_next(cur, +1));
    zoom_sync_label(win);
}

static void on_zoom_reset_clicked(GtkButton *b, gpointer data) {
    (void)b;
    WinState *win = data;
    doodle_canvas_set_view_scale(win->canvas, DOODLE_VIEW_SCALE_DEF);
    zoom_sync_label(win);
}

/* 「删除高亮」按钮回调：删除当前选中的高亮记录。 */
static void on_hl_delete_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    WinState *win = data;
    if (!win || !win->canvas) return;
    doodle_canvas_delete_selected_highlight(win->canvas);
}

/* 笔色选择按钮：rgba 改变时同步到画布全局笔色 */
static void on_pen_color_changed(GObject *btn, GParamSpec *ps, gpointer data) {
    (void)ps;
    WinState *win = data;
    if (!win || !win->canvas) return;
    const GdkRGBA *rgba =
        gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(btn));
    if (!rgba) return;
    DRGBA c = { rgba->red, rgba->green, rgba->blue, rgba->alpha };
    doodle_canvas_set_global_pen_color(win->canvas, c);
}

static void on_tool_toggled(GtkToggleButton *btn, gpointer data) {
    if (!gtk_toggle_button_get_active(btn)) return;
    WinState *win = data;
    Tool t = TOOL_SELECT;
    if      ((GtkWidget *)btn == win->tool_line  ) t = TOOL_LINE;
    else if ((GtkWidget *)btn == win->tool_path  ) t = TOOL_PATH;
    else if ((GtkWidget *)btn == win->tool_erase ) t = TOOL_ERASE;
    else if ((GtkWidget *)btn == win->tool_select) t = TOOL_SELECT;
    else if ((GtkWidget *)btn == win->tool_hl    ) t = TOOL_HIGHLIGHT;
    else if ((GtkWidget *)btn == win->tool_hl_erase) t = TOOL_HL_ERASE;
    else if ((GtkWidget *)btn == win->tool_rect  ) t = TOOL_RECT;
    else if ((GtkWidget *)btn == win->tool_ellipse) t = TOOL_ELLIPSE;
    doodle_canvas_set_tool(win->canvas, t);
    /* 工具切换可能强制改 paint_target（RECT/ELLIPSE → TRUE），同步到 segment */
    if (win->target_path_btn && win->target_paint_btn) {
        gboolean pt = doodle_canvas_get_paint_target(win->canvas);
        win->syncing_target = TRUE;
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(pt ? win->target_paint_btn : win->target_path_btn), TRUE);
        win->syncing_target = FALSE;
    }
    update_highlight_panel(win);
}

static void on_target_toggled(GtkToggleButton *btn, gpointer data) {
    WinState *win = data;
    if (win->syncing_target) return;
    if (!gtk_toggle_button_get_active(btn)) return;
    gboolean paint = ((GtkWidget *)btn == win->target_paint_btn);
    doodle_canvas_set_paint_target(win->canvas, paint);
}

static void on_hl_global_changed(GtkSpinButton *sp, gpointer data) {
    WinState *win = data;
    doodle_canvas_set_global_highlight_width(win->canvas,
        gtk_spin_button_get_value(sp));
}

static void on_hl_local_changed(GtkSpinButton *sp, gpointer data) {
    WinState *win = data;
    if (win->syncing_hl_width) return;
    double v = gtk_spin_button_get_value(sp);
    doodle_canvas_set_selected_highlight_width(win->canvas, v);
}

static void on_ar_param_changed(GtkSpinButton *sp, gpointer data) {
    (void)sp;
    WinState *win = data;
    doodle_canvas_set_array_params(win->canvas,
        (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(win->ar_rows_spin)),
        (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(win->ar_cols_spin)),
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(win->ar_gx_spin)),
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(win->ar_gy_spin)));
}

static void on_ar_start(GtkButton *b, gpointer data) {
    (void)b;
    WinState *win = data;
    int rows = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(win->ar_rows_spin));
    int cols = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(win->ar_cols_spin));
    double gx = gtk_spin_button_get_value(GTK_SPIN_BUTTON(win->ar_gx_spin));
    double gy = gtk_spin_button_get_value(GTK_SPIN_BUTTON(win->ar_gy_spin));
    if (!doodle_canvas_begin_array_preview(win->canvas, rows, cols, gx, gy))
        gtk_label_set_text(GTK_LABEL(win->missing_label),
            "请先用「选择」工具选中一个或多个非阵列图形（Ctrl+单击可多选）再开始预览");
    else
        update_array_button_states(win);
}

static void on_ar_apply (GtkButton *b, gpointer d) {
    (void)b; doodle_canvas_apply_array (((WinState *)d)->canvas);
    refresh_panel((WinState *)d);
}
static void on_ar_cancel(GtkButton *b, gpointer d) {
    (void)b; doodle_canvas_cancel_array(((WinState *)d)->canvas);
    refresh_panel((WinState *)d);
}

static void on_add_image_layer(GtkButton *b, gpointer data) {
    (void)b;
    WinState *win = data;
    doc_insert_image_stub_below_active(win->doc);
    refresh_panel(win);
    doodle_canvas_request_redraw(win->canvas);
}

static void on_compact_numbers(GtkButton *b, gpointer data) {
    (void)b;
    WinState *win = data;
    doc_compact_numbers(win->doc);
    refresh_panel(win);
    doodle_canvas_request_redraw(win->canvas);
}

/* ─── 装配 ──────────────────────────────────────────────────────── */

static GtkWidget *build_view_internal(WinState *win) {
    GtkBuilder *b = gtk_builder_new_from_resource(
        "/com/github/notework/doodle/doodle_window.ui");
    win->builder = b;

    GtkWidget *holder = GTK_WIDGET(gtk_builder_get_object(b, "canvas_holder"));
    win->canvas = doodle_canvas_new(win->doc);
    gtk_box_append(GTK_BOX(holder), win->canvas);

    win->missing_label = GTK_WIDGET(gtk_builder_get_object(b, "missing_label"));
    win->numbers_list  = GTK_WIDGET(gtk_builder_get_object(b, "numbers_list"));
    win->layers_list   = GTK_WIDGET(gtk_builder_get_object(b, "layers_list"));
    win->ar_rows_spin  = GTK_WIDGET(gtk_builder_get_object(b, "ar_rows_spin"));
    win->ar_cols_spin  = GTK_WIDGET(gtk_builder_get_object(b, "ar_cols_spin"));
    win->ar_gx_spin    = GTK_WIDGET(gtk_builder_get_object(b, "ar_gx_spin"));
    win->ar_gy_spin    = GTK_WIDGET(gtk_builder_get_object(b, "ar_gy_spin"));
    win->ar_start_btn  = GTK_WIDGET(gtk_builder_get_object(b, "ar_start_btn"));
    win->ar_apply_btn  = GTK_WIDGET(gtk_builder_get_object(b, "ar_apply_btn"));
    win->ar_cancel_btn = GTK_WIDGET(gtk_builder_get_object(b, "ar_cancel_btn"));
    win->compact_btn   = GTK_WIDGET(gtk_builder_get_object(b, "compact_btn"));
    win->tool_line     = GTK_WIDGET(gtk_builder_get_object(b, "tool_line_btn"));
    win->tool_path     = GTK_WIDGET(gtk_builder_get_object(b, "tool_path_btn"));
    win->tool_erase    = GTK_WIDGET(gtk_builder_get_object(b, "tool_erase_btn"));
    win->tool_select   = GTK_WIDGET(gtk_builder_get_object(b, "tool_select_btn"));
    win->tool_hl       = GTK_WIDGET(gtk_builder_get_object(b, "tool_hl_btn"));
    win->tool_hl_erase = GTK_WIDGET(gtk_builder_get_object(b, "tool_hl_erase_btn"));
    win->tool_rect     = GTK_WIDGET(gtk_builder_get_object(b, "tool_rect_btn"));
    win->tool_ellipse  = GTK_WIDGET(gtk_builder_get_object(b, "tool_ellipse_btn"));
    win->target_path_btn  = GTK_WIDGET(gtk_builder_get_object(b, "target_path_btn"));
    win->target_paint_btn = GTK_WIDGET(gtk_builder_get_object(b, "target_paint_btn"));
    win->hl_global_spin= GTK_WIDGET(gtk_builder_get_object(b, "hl_global_spin"));
    win->hl_local_spin = GTK_WIDGET(gtk_builder_get_object(b, "hl_local_spin"));
    win->hl_delete_btn = GTK_WIDGET(gtk_builder_get_object(b, "hl_delete_btn"));
    win->pen_color_btn = GTK_WIDGET(gtk_builder_get_object(b, "pen_color_btn"));

    /* 视图缩放按钮 */
    win->zoom_out_btn   = GTK_WIDGET(gtk_builder_get_object(b, "zoom_out_btn"));
    win->zoom_reset_btn = GTK_WIDGET(gtk_builder_get_object(b, "zoom_reset_btn"));
    win->zoom_in_btn    = GTK_WIDGET(gtk_builder_get_object(b, "zoom_in_btn"));

    /* 同步画布全局 width 初始值到 spin */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(win->hl_global_spin),
        doodle_canvas_get_global_highlight_width(win->canvas));

    g_signal_connect(win->tool_line,   "toggled", G_CALLBACK(on_tool_toggled),  win);
    g_signal_connect(win->tool_path,   "toggled", G_CALLBACK(on_tool_toggled),  win);
    g_signal_connect(win->tool_erase,  "toggled", G_CALLBACK(on_tool_toggled),  win);
    g_signal_connect(win->tool_select, "toggled", G_CALLBACK(on_tool_toggled),  win);
    g_signal_connect(win->tool_hl,     "toggled", G_CALLBACK(on_tool_toggled),  win);
    g_signal_connect(win->tool_hl_erase,"toggled", G_CALLBACK(on_tool_toggled),  win);
    g_signal_connect(win->tool_rect,    "toggled", G_CALLBACK(on_tool_toggled),  win);
    g_signal_connect(win->tool_ellipse, "toggled", G_CALLBACK(on_tool_toggled),  win);

    if (win->target_path_btn)
        g_signal_connect(win->target_path_btn, "toggled",
                         G_CALLBACK(on_target_toggled), win);
    if (win->target_paint_btn)
        g_signal_connect(win->target_paint_btn, "toggled",
                         G_CALLBACK(on_target_toggled), win);

    g_signal_connect(win->hl_global_spin, "value-changed",
                     G_CALLBACK(on_hl_global_changed), win);
    g_signal_connect(win->hl_local_spin,  "value-changed",
                     G_CALLBACK(on_hl_local_changed),  win);
    if (win->hl_delete_btn)
        g_signal_connect(win->hl_delete_btn, "clicked",
                         G_CALLBACK(on_hl_delete_clicked), win);

    if (win->pen_color_btn) {
        /* 把画布当前 pen_color 同步到按钮初始 rgba */
        DRGBA pc = doodle_canvas_get_global_pen_color(win->canvas);
        GdkRGBA rgba = { pc.r, pc.g, pc.b, pc.a > 0 ? pc.a : 1.0 };
        gtk_color_dialog_button_set_rgba(
            GTK_COLOR_DIALOG_BUTTON(win->pen_color_btn), &rgba);
        g_signal_connect(win->pen_color_btn, "notify::rgba",
                         G_CALLBACK(on_pen_color_changed), win);
    }

    if (win->zoom_out_btn)
        g_signal_connect(win->zoom_out_btn,   "clicked",
                         G_CALLBACK(on_zoom_out_clicked),   win);
    if (win->zoom_reset_btn)
        g_signal_connect(win->zoom_reset_btn, "clicked",
                         G_CALLBACK(on_zoom_reset_clicked), win);
    if (win->zoom_in_btn)
        g_signal_connect(win->zoom_in_btn,    "clicked",
                         G_CALLBACK(on_zoom_in_clicked),    win);
    zoom_sync_label(win);

    g_signal_connect(win->ar_rows_spin, "value-changed", G_CALLBACK(on_ar_param_changed), win);
    g_signal_connect(win->ar_cols_spin, "value-changed", G_CALLBACK(on_ar_param_changed), win);
    g_signal_connect(win->ar_gx_spin,   "value-changed", G_CALLBACK(on_ar_param_changed), win);
    g_signal_connect(win->ar_gy_spin,   "value-changed", G_CALLBACK(on_ar_param_changed), win);

    g_signal_connect(win->ar_start_btn,  "clicked", G_CALLBACK(on_ar_start),  win);
    g_signal_connect(win->ar_apply_btn,  "clicked", G_CALLBACK(on_ar_apply),  win);
    g_signal_connect(win->ar_cancel_btn, "clicked", G_CALLBACK(on_ar_cancel), win);
    if (win->compact_btn)
        g_signal_connect(win->compact_btn, "clicked",
                         G_CALLBACK(on_compact_numbers), win);

    g_signal_connect(gtk_builder_get_object(b, "add_image_layer_btn"),
                     "clicked", G_CALLBACK(on_add_image_layer), win);

    doodle_canvas_set_changed_cb(win->canvas, on_canvas_changed, win);

    /* 初始 tool = 默认选中 toggle 对应工具 */
    on_tool_toggled(GTK_TOGGLE_BUTTON(win->tool_select), win);
    refresh_panel(win);

    return GTK_WIDGET(gtk_builder_get_object(b, "dwin"));
}

/* ─── 公共 API ─────────────────────────────────────────────────── */

GtkWidget *doodle_window_new(void) {
    WinState *win = g_new0(WinState, 1);
    win->doc     = doodle_doc_new();
    win->own_doc = TRUE;
    GtkWidget *w = build_view_internal(win);
    g_object_set_data_full(G_OBJECT(w), "doodle-win-state", win, win_state_free);
    return w;
}

GtkWidget *doodle_window_new_for_doc(DoodleDoc *doc, gboolean own_doc) {
    WinState *win = g_new0(WinState, 1);
    win->doc     = doc ? doc : doodle_doc_new();
    win->own_doc = doc ? own_doc : TRUE;
    GtkWidget *w = build_view_internal(win);
    g_object_set_data_full(G_OBJECT(w), "doodle-win-state", win, win_state_free);
    return w;
}

GtkWidget *doodle_view_new(void) {
    /* 简化：返回完整窗口的内容部分。
     * 当前实现等价于 doodle_window_new()，调用者按需 reparent。
     * 后续如需真正的"内嵌视图"，可在此手工构造去除 HeaderBar 的版本。 */
    return doodle_window_new();
}

DoodleDoc *doodle_view_get_doc(GtkWidget *view) {
    WinState *s = g_object_get_data(G_OBJECT(view), "doodle-win-state");
    return s ? s->doc : NULL;
}

/* 设置初始工具（供 album 入口在弹出 doodle 后调用）。 */
void doodle_window_set_initial_tool(GtkWidget *win, Tool t) {
    WinState *s = g_object_get_data(G_OBJECT(win), "doodle-win-state");
    if (!s) return;
    GtkToggleButton *btn = NULL;
    switch (t) {
    case TOOL_LINE:      btn = GTK_TOGGLE_BUTTON(s->tool_line);   break;
    case TOOL_PATH:      btn = GTK_TOGGLE_BUTTON(s->tool_path);   break;
    case TOOL_ERASE:     btn = GTK_TOGGLE_BUTTON(s->tool_erase);  break;
    case TOOL_SELECT:    btn = GTK_TOGGLE_BUTTON(s->tool_select); break;
    case TOOL_HIGHLIGHT: btn = GTK_TOGGLE_BUTTON(s->tool_hl);     break;
    case TOOL_HL_ERASE:  btn = GTK_TOGGLE_BUTTON(s->tool_hl_erase); break;
    case TOOL_RECT:      btn = GTK_TOGGLE_BUTTON(s->tool_rect);    break;
    case TOOL_ELLIPSE:   btn = GTK_TOGGLE_BUTTON(s->tool_ellipse); break;
    }
    if (btn) gtk_toggle_button_set_active(btn, TRUE);
    doodle_canvas_set_tool(s->canvas, t);
    update_highlight_panel(s);
}
