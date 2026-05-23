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
}

static void on_canvas_changed(GtkWidget *canvas, gpointer data) {
    (void)canvas;
    refresh_panel((WinState *)data);
}

/* ─── 信号回调 ──────────────────────────────────────────────────── */

static void on_tool_toggled(GtkToggleButton *btn, gpointer data) {
    if (!gtk_toggle_button_get_active(btn)) return;
    WinState *win = data;
    Tool t = TOOL_SELECT;
    if      ((GtkWidget *)btn == win->tool_line  ) t = TOOL_LINE;
    else if ((GtkWidget *)btn == win->tool_path  ) t = TOOL_PATH;
    else if ((GtkWidget *)btn == win->tool_erase ) t = TOOL_ERASE;
    else if ((GtkWidget *)btn == win->tool_select) t = TOOL_SELECT;
    doodle_canvas_set_tool(win->canvas, t);
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

    g_signal_connect(win->tool_line,   "toggled", G_CALLBACK(on_tool_toggled),  win);
    g_signal_connect(win->tool_path,   "toggled", G_CALLBACK(on_tool_toggled),  win);
    g_signal_connect(win->tool_erase,  "toggled", G_CALLBACK(on_tool_toggled),  win);
    g_signal_connect(win->tool_select, "toggled", G_CALLBACK(on_tool_toggled),  win);

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
