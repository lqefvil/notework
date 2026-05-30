/**
 * notework — 主程序入口（Phase 2.x）
 *
 * 主窗口结构（参见 src/window.ui）：
 *   AdwApplicationWindow main_window
 *   └── AdwToolbarView
 *       ├── [top] AdwHeaderBar  +  AdwViewSwitcher (stack=main_stack)
 *       └── [content] AdwViewStack main_stack
 *           ├── page "album"    → GtkBox album_holder
 *           │     运行时由 album_view_new(album) 返回的内嵌控件填入
 *           └── page "timeline" → 进度轴 + 轨道列表
 *               ├── GtkScrolledWindow header_scroller
 *               │     └── GtkDrawingArea progress_axis_canvas
 *               └── GtkScrolledWindow body_scroller
 *                     └── GtkBox track_container (各 track_row 行)
 *
 * 本期（Phase 2.x）增量：
 *   - 进度轴渲染/交互移到 src/progress_axis.{c,h}；main.c 仅作为粘合层。
 *   - album_view_set_changed_cb 注册 on_album_changed：相册变动 → 重算
 *     进度轴 + 重新装填轨道行 bar 宽度。
 */

#include <adwaita.h>

#include "album.h"
#include "doodle.h"
#include "latex.h"
#include "progress_axis.h"
#include "track_row.h"

#include <string.h>

/* ─── 视图状态：随 main_window 生命周期销毁 ─────────────────────── */

typedef struct {
    Album         *album;            /* 由本结构拥有 */
    ProgressAxis  *axis;             /* 由本结构拥有 */

    GtkWidget     *album_holder;
    GtkWidget     *axis_canvas;
    GtkWidget     *track_container;     /* 右侧：众 bar 纵堆 */
    GtkWidget     *sidebar_container;   /* 左侧：众「名称+删除」行 */
    GtkScrolledWindow *header_sw;
    GtkScrolledWindow *body_sw;
    GtkScrolledWindow *sidebar_sw;

    /* album 预览视图控件（供跳转后设置聚焦高亮使用） */
    GtkWidget     *album_view;

    /* LaTeX 页 */
    GtkWidget     *latex_holder;
    GtkWidget     *latex_view;

    /* 激活区域 ↔ 高亮绑定联动 */
    int            sel_track_idx;       /* -1 表示未选中 */
    int            sel_pair_idx;
    GtkWidget     *bindings_title;
    GtkWidget     *bindings_list;
    GtkButton     *bindings_add_btn;
    GtkButton     *bindings_clear_btn;
    GtkButton     *bindings_remove_pair_btn;
    AdwViewStack  *main_stack;          /* 供跳转使用 */
} AppView;

/* ─── 轨道行装填 ─────────────────────────────────────────────────── */

/* 删除回调：sidebar 中 × 按钮挂 "track-idx" qdata，clicked 时取出。 */
static void on_sidebar_delete_clicked(GtkButton *btn, gpointer user_data);

/* 前向声明：选中联动、栏目刷新、继承树子节点递归展开。 */
static void on_pair_selected(int track_idx, int pair_idx, gpointer user_data);
static void refresh_bindings_panel(AppView *v);
static void expand_inherit_subtree(AppView *v, int track_idx, int depth,
                                    GHashTable *visited);

/* 构造 sidebar 中的一行：名称标签 + × 删除按钮。track_idx 通过
 * GINT_TO_POINTER 挂在 × 按钮上。 */
static GtkWidget *
build_sidebar_row(AppView *v, int track_idx, const char *name)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_size_request(row, -1, 32);
    /* 显式 hexpand=false，阻断内部 label hexpand=true 沿 row 向上
     * 冒泡到 sidebar_scroller。 */
    gtk_widget_set_hexpand(row, FALSE);
    /* valign=center + overflow=hidden 共同确保 row 高度严格锁定 32px，
     * 与右侧 bar 中线对齐；防止 × 按钮 nat-height 把 row 撑高错位。 */
    gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
    gtk_widget_set_overflow(row, GTK_OVERFLOW_HIDDEN);

    GtkWidget *lbl = gtk_label_new(name ? name : "(unnamed)");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(lbl, 8);
    gtk_box_append(GTK_BOX(row), lbl);

    GtkWidget *del = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(del, "flat");
    gtk_widget_add_css_class(del, "circular");
    gtk_widget_set_tooltip_text(del, "删除该轨道");
    gtk_widget_set_valign(del, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(del), "track-idx",
                       GINT_TO_POINTER(track_idx));
    g_signal_connect(del, "clicked",
                     G_CALLBACK(on_sidebar_delete_clicked), v);
    gtk_box_append(GTK_BOX(row), del);

    return row;
}

static void
populate_tracks(AppView *v)
{
    /* 同时清空两侧 */
    for (GtkWidget *c = gtk_widget_get_first_child(v->track_container); c; ) {
        GtkWidget *next = gtk_widget_get_next_sibling(c);
        gtk_box_remove(GTK_BOX(v->track_container), c);
        c = next;
    }
    for (GtkWidget *c = gtk_widget_get_first_child(v->sidebar_container); c; ) {
        GtkWidget *next = gtk_widget_get_next_sibling(c);
        gtk_box_remove(GTK_BOX(v->sidebar_container), c);
        c = next;
    }

    int bar_w = progress_axis_get_content_width(v->axis);
    /* 无论是否有轨道，track_container 最小宽度始终与进度轴一致，
     * 确保 body_scroller 有足够宽度产生水平滚动。 */
    gtk_widget_set_size_request(v->track_container, bar_w, -1);
    int n     = (v->album && v->album->tracks)
                ? (int)v->album->tracks->len : 0;
    for (int i = 0; i < n; i++) {
        Track *t = &g_array_index(v->album->tracks, Track, (guint)i);
        gtk_box_append(GTK_BOX(v->sidebar_container),
                       build_sidebar_row(v, i, t ? t->name : NULL));

        GtkWidget *bar = track_row_new(v->album, v->axis, i, bar_w);
        /* 注入单击选中回调（所有轨道共享同一个全局回调） */
        track_row_set_pair_selected_cb(bar, on_pair_selected, v);
        /* 同步当前选中态，使跳页后选中描边仍然可见 */
        if (i == v->sel_track_idx)
            track_row_set_selected_pair(bar, v->sel_pair_idx);
        else
            track_row_set_selected_pair(bar, -1);
        gtk_box_append(GTK_BOX(v->track_container), bar);
        /* track_row_new 为 transfer-full：append 加 ref，这里平衡。 */
        g_object_unref(bar);
    }
}

/* ─── Album 变更回调：进度轴重算 + 轨道重填 ─────────────────────── */

static void
on_album_changed(GtkWidget *album_view, gpointer user_data)
{
    (void)album_view;
    AppView *v = user_data;
    if (!v) return;
    /* 起始：则变更后 sel_* 可能已越界，需裁齐 */
    {
        if (!v->album || !v->album->tracks) {
            v->sel_track_idx = -1; v->sel_pair_idx = -1;
        } else {
            int nt = (int)v->album->tracks->len;
            if (v->sel_track_idx < 0 || v->sel_track_idx >= nt) {
                v->sel_track_idx = -1; v->sel_pair_idx = -1;
            } else {
                Track *t = &g_array_index(v->album->tracks, Track,
                                           (guint)v->sel_track_idx);
                int np = (t && t->pairs) ? (int)t->pairs->len : 0;
                if (v->sel_pair_idx < 0 || v->sel_pair_idx >= np) {
                    v->sel_track_idx = -1; v->sel_pair_idx = -1;
                }
            }
        }
    }
    progress_axis_refresh(v->axis);
    populate_tracks(v);
    /* 末尾：刷新栏目与进度轴 emph（内部会完成懒清理） */
    refresh_bindings_panel(v);
}

/* ─── 轨道增删交互回调 ─────────────────────────────────────────── */

static void
on_track_add_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    AppView *v = user_data;
    if (!v) return;
    album_track_append(v->album, NULL);   /* 默认名 "轨道 N" */
    on_album_changed(NULL, v);
}

static void
on_sidebar_delete_clicked(GtkButton *btn, gpointer user_data)
{
    AppView *v = user_data;
    int idx = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(btn), "track-idx"));
    if (!v) return;
    if (album_track_remove(v->album, idx))
        on_album_changed(NULL, v);
}

/* ─── 激活区域 ↔ 高亮绑定联动 ─────────────────────────────────── */

/* 跳转到 album 页并聚焦某条高亮（Task 7）。
 * page<0 / hl_id==0 视为无效；同时复核高亮存活，避免极端时序下越界。 */
static void
jump_to_highlight(AppView *v, int page, guint64 hl_id)
{
    if (!v || page < 0 || hl_id == 0) return;
    int p = -1;
    if (!album_find_highlight_by_id(v->album, hl_id, &p, NULL, NULL))
        return;
    if (v->main_stack)
        adw_view_stack_set_visible_child_name(v->main_stack, "album");
    album_set_active(v->album, p);
    progress_axis_set_focused_highlight(v->axis, p, hl_id);
    if (v->album_view)
        album_view_set_focused_highlight(v->album_view, hl_id);
    if (GTK_IS_WIDGET(v->axis_canvas))
        gtk_widget_queue_draw(v->axis_canvas);
}

/* 进度轴单击：拾取高亮 → 跳转。
 * 关键：playhead 的 GtkGestureDrag 在 drag-begin 中会 CLAIMED 序列，如果本 click
 * 手势走默认 BUBBLE+released，会被 playhead 抢占。因此采用 CAPTURE 阶段
 * 监听 pressed：命中高亮时立即 CLAIMED（阻断 playhead drag-begin）并跳转；
 * 未命中不动作，drag 按原行为接手。 */
static void
on_axis_click_pressed(GtkGestureClick *gesture,
                       int n_press, double x, double y,
                       gpointer user_data)
{
    (void)n_press;
    AppView *v = user_data;
    if (!v) return;
    int page = -1; guint64 hl = 0;
    if (!progress_axis_pick_highlight_at_px(v->axis, x, y, &page, &hl))
        return;
    /* 命中：抑制本次 sequence 被 playhead drag 接管 */
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    jump_to_highlight(v, page, hl);
}

/* 生成高亮记录的友好标签：
 *   格式 "P{page+1} · 路径#{host_shape_number} 第{order}条"
 *   order = 在同一宿主路径上、同页中、按 hl id 渐增（即创建顺序）排序后的 1-based 序号。
 * 拾取不到时退化为 "P{p+1} · ?"。调用方负责 g_free 返回的字符串。 */
static char *
format_highlight_label(Album *a, int page_idx, guint64 hl_id)
{
    AlbumPage *pg = album_get_page(a, page_idx);
    if (!pg || !pg->doc || !pg->doc->layers || hl_id == 0)
        return g_strdup_printf("P%d · ?", page_idx + 1);

    HighlightRecord *target = NULL;
    for (guint li = 0; li < pg->doc->layers->len && !target; li++) {
        Layer *L = &g_array_index(pg->doc->layers, Layer, li);
        if (L->kind != LAYER_HIGHLIGHT || !L->highlights) continue;
        for (guint k = 0; k < L->highlights->len; k++) {
            HighlightRecord *r = g_ptr_array_index(L->highlights, k);
            if (r && r->id == hl_id) { target = r; break; }
        }
    }
    if (!target)
        return g_strdup_printf("P%d · ?", page_idx + 1);

    /* 同页、同宿主路径上的其他高亮：按 id 渐增计算 target 的序位 */
    int order = 1;
    for (guint li = 0; li < pg->doc->layers->len; li++) {
        Layer *L = &g_array_index(pg->doc->layers, Layer, li);
        if (L->kind != LAYER_HIGHLIGHT || !L->highlights) continue;
        for (guint k = 0; k < L->highlights->len; k++) {
            HighlightRecord *r = g_ptr_array_index(L->highlights, k);
            if (!r || r == target) continue;
            if (r->host_layer_idx == target->host_layer_idx &&
                r->host_shape_number == target->host_shape_number &&
                r->id < target->id)
                order++;
        }
    }
    return g_strdup_printf("P%d · 路径#%d 第%d条",
                            page_idx + 1, target->host_shape_number, order);
}

/* 清除所有聚焦提示（进度轴 / album 预览 / 各轨道 bar / LaTeX 文本） */
static void
clear_all_focuses(AppView *v)
{
    if (!v) return;
    progress_axis_clear_focused_highlight(v->axis);
    if (v->album_view)
        album_view_set_focused_highlight(v->album_view, 0);
    if (v->track_container) {
        for (GtkWidget *c = gtk_widget_get_first_child(v->track_container);
             c; c = gtk_widget_get_next_sibling(c))
            track_row_set_focus_highlight(c, FALSE);
    }
    if (v->latex_view)
        latex_view_focus_range(v->latex_view, 0, 0);
    if (GTK_IS_WIDGET(v->axis_canvas))
        gtk_widget_queue_draw(v->axis_canvas);
}

/* 按钮上溯取 PairBinding（按钮 → list row → row child hbox → "bind"） */
static const PairBinding *
row_binding_of_button(GtkButton *btn)
{
    GtkWidget *row = gtk_widget_get_ancestor(GTK_WIDGET(btn),
                                              GTK_TYPE_LIST_BOX_ROW);
    if (!row) return NULL;
    GtkWidget *child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    if (!child) return NULL;
    return g_object_get_data(G_OBJECT(child), "bind");
}

/* 按绑定 kind 联动各端的聚焦效果：
 *   HL_ENV/VAR → 进度轴（env=黄 / var=绿） + album 预览描边；
 *   LATEX_RANGE → 切到 LaTeX 页 + 选中该范围；
 *   INHERIT_TRACK → 在被继承的轨道 bar 上画紫色虚线描边。 */
static void
focus_binding(AppView *v, const PairBinding *b)
{
    if (!v || !b) return;
    clear_all_focuses(v);
    switch (b->kind) {
        case BIND_HL_ENV:
        case BIND_HL_VAR: {
            int p = -1;
            if (!album_find_highlight_by_id(v->album, b->payload.hl_id,
                                              &p, NULL, NULL))
                return;
            /* 切到 album 页并跳到该高亮所在页，才能看见描边。 */
            if (v->main_stack)
                adw_view_stack_set_visible_child_name(v->main_stack, "album");
            album_set_active(v->album, p);
            progress_axis_set_focused_highlight_kind(v->axis, p,
                b->payload.hl_id, b->kind == BIND_HL_ENV ? 1 : 2);
            if (v->album_view)
                album_view_set_focused_highlight(v->album_view,
                                                  b->payload.hl_id);
            if (GTK_IS_WIDGET(v->axis_canvas))
                gtk_widget_queue_draw(v->axis_canvas);
            break;
        }
        case BIND_LATEX_RANGE:
            if (v->main_stack)
                adw_view_stack_set_visible_child_name(v->main_stack, "latex");
            if (v->latex_view)
                latex_view_focus_range(v->latex_view,
                    b->payload.range.start, b->payload.range.length);
            break;
        case BIND_INHERIT_TRACK: {
            int ti = b->payload.inherit.track_idx;
            int i = 0;
            if (!v->track_container) break;
            for (GtkWidget *c = gtk_widget_get_first_child(v->track_container);
                 c; c = gtk_widget_get_next_sibling(c), i++) {
                if (i == ti)
                    track_row_set_focus_highlight(c, TRUE);
            }
            break;
        }
        case BIND_INHERIT_MASK:
            /* mask 仅为状态记录，不产生联动聚焦。 */
            break;
    }
}

/* listbox 行选中：点击条目 → 按 kind 联动聚焦。
 * row=NULL 时清除所有聚焦。 */
static void
on_bindings_list_row_selected(GtkListBox *box, GtkListBoxRow *row,
                                gpointer user_data)
{
    (void)box;
    AppView *v = user_data;
    if (!v) return;
    if (!row) { clear_all_focuses(v); return; }
    GtkWidget *child = gtk_list_box_row_get_child(row);
    if (!child) return;
    const PairBinding *b = g_object_get_data(G_OBJECT(child), "bind");
    if (b) focus_binding(v, b);
}

/* 行内「跳转」按钮 */
static void
on_binding_row_jump_clicked(GtkButton *btn, gpointer user_data)
{
    AppView *v = user_data;
    const PairBinding *b = row_binding_of_button(btn);
    if (!v || !b) return;
    focus_binding(v, b);
}

/* 行内「env↔var 切换」按钮（仅 HL 绑定出现） */
static void
on_binding_row_toggle_kind_clicked(GtkButton *btn, gpointer user_data)
{
    AppView *v = user_data;
    if (!v || v->sel_track_idx < 0 || v->sel_pair_idx < 0) return;
    const PairBinding *b = row_binding_of_button(btn);
    if (!b || (b->kind != BIND_HL_ENV && b->kind != BIND_HL_VAR)) return;
    BindKind nk = (b->kind == BIND_HL_ENV) ? BIND_HL_VAR : BIND_HL_ENV;
    album_pair_set_hl_kind(v->album, v->sel_track_idx, v->sel_pair_idx,
                            b->payload.hl_id, nk);
    refresh_bindings_panel(v);
}

/* 行内「解除」按钮 */
static void
on_binding_row_remove_clicked(GtkButton *btn, gpointer user_data)
{
    AppView *v = user_data;
    if (!v || v->sel_track_idx < 0 || v->sel_pair_idx < 0) return;
    const PairBinding *b = row_binding_of_button(btn);
    if (!b) return;
    PairBinding dup = *b;
    album_pair_unbind(v->album, v->sel_track_idx, v->sel_pair_idx, &dup);
    refresh_bindings_panel(v);
}

/* 行内「屏蔽」复选框（仅 readonly=TRUE 继承树子节点出现）
 * data "target" = PairBinding 的副本（被屏蔽目标）。 */
static void
on_binding_row_mask_toggled(GtkCheckButton *cb, gpointer user_data)
{
    AppView *v = user_data;
    if (!v || v->sel_track_idx < 0 || v->sel_pair_idx < 0) return;
    const PairBinding *target =
        g_object_get_data(G_OBJECT(cb), "target");
    if (!target) return;
    gboolean on = gtk_check_button_get_active(cb);
    album_pair_set_inherit_mask(v->album, v->sel_track_idx,
                                 v->sel_pair_idx, target, on);
    refresh_bindings_panel(v);
}

/* 「终点夹钳」开关已废弃：级联现为无条件沿继承链向上推，
 * 不再需要边级开关。 */

/* 构建一条绑定行：图标 + 标签 + 跳转/切换/解除按钮。
 * row hbox 上 g_object_set_data_full("bind", PairBinding *) 供后续反查。
 *
 * depth：继承树缩进层级（0 = 本轨道直接绑定）。
 * readonly：TRUE 表示从祖先轨道继承而来，不允许在本面板删除/切换；
 *           提供「屏蔽」复选框。
 * origin_track_idx：readonly 条目的来源轨道下标（仅用于标题提示。readonly=FALSE 时传 -1）。 */
static GtkWidget *
build_binding_row(AppView *v, const PairBinding *b,
                  int depth, gboolean readonly, int origin_track_idx)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(row, 6 + depth * 16);
    gtk_widget_set_margin_end(row, 6);
    gtk_widget_set_margin_top(row, 2);
    gtk_widget_set_margin_bottom(row, 2);
    PairBinding *dup = g_memdup2(b, sizeof(PairBinding));
    g_object_set_data_full(G_OBJECT(row), "bind", dup, g_free);

    const char *icon = "emblem-default-symbolic";
    char *txt = NULL;
    switch (b->kind) {
        case BIND_HL_ENV:
        case BIND_HL_VAR: {
            icon = (b->kind == BIND_HL_ENV)
                   ? "mark-location-symbolic"
                   : "accessories-calculator-symbolic";
            int p = -1;
            if (album_find_highlight_by_id(v->album, b->payload.hl_id,
                                             &p, NULL, NULL)) {
                char *base = format_highlight_label(v->album, p,
                                                     b->payload.hl_id);
                txt = g_strdup_printf("%s · %s", base,
                    b->kind == BIND_HL_ENV ? "环境" : "变量");
                g_free(base);
            } else {
                txt = g_strdup("(失效高亮)");
            }
            break;
        }
        case BIND_LATEX_RANGE: {
            icon = "accessories-text-editor-symbolic";
            char *preview = NULL;
            LatexDoc *d = v->album ? v->album->latex : NULL;
            if (d) {
                const char *src = latex_doc_get_source(d);
                int slen = src ? (int)strlen(src) : 0;
                int s = b->payload.range.start;
                int l = b->payload.range.length;
                if (s >= 0 && l > 0 && s + l <= slen) {
                    int show = l < 24 ? l : 24;
                    /* 裁到 UTF-8 字符边界 */
                    while (show > 0 && (src[s + show] & 0xC0) == 0x80) show--;
                    preview = g_strndup(src + s, show);
                }
            }
            txt = g_strdup_printf("LaTeX @%d·%d · %s",
                                    b->payload.range.start,
                                    b->payload.range.length,
                                    preview ? preview : "?");
            g_free(preview);
            break;
        }
        case BIND_INHERIT_TRACK: {
            icon = "view-list-symbolic";
            int oi = b->payload.inherit.track_idx;
            int n = (v->album && v->album->tracks)
                    ? (int)v->album->tracks->len : 0;
            const char *name = "?";
            if (oi >= 0 && oi < n) {
                const Track *t = &g_array_index(v->album->tracks, Track,
                                                  (guint)oi);
                if (t->name) name = t->name;
            }
            txt = g_strdup_printf("继承轨道 %d · %s", oi + 1, name);
            break;
        }
        case BIND_INHERIT_MASK:
            /* mask 不直接渲染为行；防御性占位。 */
            txt = g_strdup("(屏蔽记录)");
            break;
    }

    GtkWidget *img = gtk_image_new_from_icon_name(icon);
    gtk_box_append(GTK_BOX(row), img);

    /* readonly 行附加「继承自 Tn」前缀提示 */
    char *full = NULL;
    if (readonly && origin_track_idx >= 0) {
        full = g_strdup_printf("⤷ T%d · %s", origin_track_idx + 1,
                                 txt ? txt : "");
    } else {
        full = g_strdup(txt ? txt : "");
    }
    GtkWidget *lbl = gtk_label_new(full);
    g_free(full);
    g_free(txt);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_box_append(GTK_BOX(row), lbl);

    if (!readonly) {
        GtkWidget *go = gtk_button_new_from_icon_name("go-jump-symbolic");
        gtk_widget_add_css_class(go, "flat");
        gtk_widget_set_tooltip_text(go, "跳转/聚焦该绑定对象");
        g_signal_connect(go, "clicked",
                         G_CALLBACK(on_binding_row_jump_clicked), v);
        gtk_box_append(GTK_BOX(row), go);

        if (b->kind == BIND_HL_ENV || b->kind == BIND_HL_VAR) {
            GtkWidget *tg = gtk_button_new_with_label(
                b->kind == BIND_HL_ENV ? "→变量" : "→环境");
            gtk_widget_add_css_class(tg, "flat");
            gtk_widget_set_tooltip_text(tg, "切换环境/变量");
            g_signal_connect(tg, "clicked",
                             G_CALLBACK(on_binding_row_toggle_kind_clicked), v);
            gtk_box_append(GTK_BOX(row), tg);
        }

        /* 本轨道 INHERIT_TRACK 行不再提供“终点夹钳”开关：
         * 级联已简化为无条件沿继承链向上推。 */

        GtkWidget *rm = gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_widget_add_css_class(rm, "flat");
        gtk_widget_set_tooltip_text(rm, "解除该绑定");
        g_signal_connect(rm, "clicked",
                         G_CALLBACK(on_binding_row_remove_clicked), v);
        gtk_box_append(GTK_BOX(row), rm);
    } else {
        /* readonly：仅提供「跳转」 + 「屏蔽」 */
        GtkWidget *go = gtk_button_new_from_icon_name("go-jump-symbolic");
        gtk_widget_add_css_class(go, "flat");
        gtk_widget_set_tooltip_text(go, "跳转/聚焦该绑定对象");
        g_signal_connect(go, "clicked",
                         G_CALLBACK(on_binding_row_jump_clicked), v);
        gtk_box_append(GTK_BOX(row), go);

        GtkWidget *mk = gtk_check_button_new_with_label("屏蔽");
        gtk_widget_set_tooltip_text(mk,
            "仅在本轨道隐藏该继承条目（不影响源轨道）");
        gboolean masked = album_pair_is_inherit_masked(v->album,
                                                        v->sel_track_idx,
                                                        v->sel_pair_idx, b);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(mk), masked);
        PairBinding *tdup = g_memdup2(b, sizeof(PairBinding));
        g_object_set_data_full(G_OBJECT(mk), "target", tdup, g_free);
        g_signal_connect(mk, "toggled",
                         G_CALLBACK(on_binding_row_mask_toggled), v);
        gtk_box_append(GTK_BOX(row), mk);
    }

    return row;
}

/* 递归展开某子轨道的绑定条目为 readonly 行。
 * visited 为环检测集（包含已展开轨道下标）；depth>=8 或环中 → 输出「…」占位。
 * 屏蔽作用域为整子树：所有 readonly 条目都查 self.pair[0] 的 mask 集。 */
static void
expand_inherit_subtree(AppView *v, int track_idx, int depth,
                       GHashTable *visited)
{
    if (!v || !v->album) return;
    if (depth > 8) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_margin_start(row, 6 + depth * 16);
        gtk_box_append(GTK_BOX(row),
            gtk_label_new("… (继承深度超限 8 层)"));
        gtk_list_box_append(GTK_LIST_BOX(v->bindings_list), row);
        g_warning("expand_inherit_subtree: depth>8 truncated at track=%d",
                  track_idx);
        return;
    }
    if (g_hash_table_contains(visited, GINT_TO_POINTER(track_idx))) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_margin_start(row, 6 + depth * 16);
        gtk_box_append(GTK_BOX(row),
            gtk_label_new("… (环路裁切)"));
        gtk_list_box_append(GTK_LIST_BOX(v->bindings_list), row);
        return;
    }
    g_hash_table_add(visited, GINT_TO_POINTER(track_idx));

    if (track_idx < 0 || !v->album->tracks ||
        (guint)track_idx >= v->album->tracks->len)
        return;
    Track *t = &g_array_index(v->album->tracks, Track, (guint)track_idx);
    if (!t->pairs || t->pairs->len == 0) return;
    HandlePair *hp = &g_array_index(t->pairs, HandlePair, 0);
    if (!hp->bindings || hp->bindings->len == 0) return;
    for (guint i = 0; i < hp->bindings->len; i++) {
        PairBinding *b = &g_array_index(hp->bindings, PairBinding, i);
        if (b->kind == BIND_INHERIT_MASK) continue;  /* mask 不传递 */
        if (album_pair_is_inherit_masked(v->album, v->sel_track_idx,
                                          v->sel_pair_idx, b))
            continue;                                /* 本轨道屏蔽 */
        GtkWidget *row = build_binding_row(v, b, depth, /*readonly*/TRUE,
                                            /*origin*/track_idx);
        gtk_list_box_append(GTK_LIST_BOX(v->bindings_list), row);
        /* INHERIT_TRACK 节点继续向上递归 */
        if (b->kind == BIND_INHERIT_TRACK) {
            expand_inherit_subtree(v, b->payload.inherit.track_idx,
                                    depth + 1, visited);
        }
    }
}

/* 重建栏目内容：懒清理 → 重建 listbox → 推送 emph 到进度轴。
 * emph 仅含 HL 类绑定；LaTeX/继承不影响进度轴热力图。 */
static void
refresh_bindings_panel(AppView *v)
{
    if (!v) return;
    /* 重建期间屏蔽 row-selected，避免清空/重建过程中 listbox 选中变化
     * 反复触发 focus_binding，导致错误跳转（例如切换 env/var 后
     * 跳到 LaTeX 页）。 */
    if (v->bindings_list)
        g_signal_handlers_block_by_func(v->bindings_list,
            G_CALLBACK(on_bindings_list_row_selected), v);
    /* listbox 清空 */
    if (v->bindings_list) {
        for (GtkWidget *c = gtk_widget_get_first_child(v->bindings_list); c; ) {
            GtkWidget *next = gtk_widget_get_next_sibling(c);
            gtk_list_box_remove(GTK_LIST_BOX(v->bindings_list), c);
            c = next;
        }
    }
    if (v->sel_track_idx < 0 || v->sel_pair_idx < 0) {
        if (v->bindings_title)
            gtk_label_set_text(GTK_LABEL(v->bindings_title),
                                "未选中激活区域");
        if (v->bindings_add_btn)
            gtk_widget_set_sensitive(GTK_WIDGET(v->bindings_add_btn), FALSE);
        if (v->bindings_clear_btn)
            gtk_widget_set_sensitive(GTK_WIDGET(v->bindings_clear_btn), FALSE);
        if (v->bindings_remove_pair_btn)
            gtk_widget_set_sensitive(GTK_WIDGET(v->bindings_remove_pair_btn), FALSE);
        progress_axis_clear_emphasized(v->axis);
        if (GTK_IS_WIDGET(v->axis_canvas))
            gtk_widget_queue_draw(v->axis_canvas);
        /* 平衡函数顶部的 block：否则多次走本分支会使计数累加，
         * 导致 row-selected 信号被永久屏蔽，表现为面板点击无响应。 */
        if (v->bindings_list)
            g_signal_handlers_unblock_by_func(v->bindings_list,
                G_CALLBACK(on_bindings_list_row_selected), v);
        return;
    }
    if (v->bindings_title) {
        char *txt = g_strdup_printf("轨道 %d · 激活区域 #%d",
                                     v->sel_track_idx + 1,
                                     v->sel_pair_idx + 1);
        gtk_label_set_text(GTK_LABEL(v->bindings_title), txt);
        g_free(txt);
    }
    /* 懒清理：失效的绑定一律剔除。使用副本避免迭代中修改。 */
    GArray *binds = album_pair_get_bindings(v->album, v->sel_track_idx,
                                             v->sel_pair_idx);
    if (binds && binds->len > 0) {
        GPtrArray *stale = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < binds->len; i++) {
            PairBinding *b = &g_array_index(binds, PairBinding, i);
            gboolean alive = TRUE;
            switch (b->kind) {
                case BIND_HL_ENV:
                case BIND_HL_VAR:
                    alive = album_find_highlight_by_id(v->album,
                                b->payload.hl_id, NULL, NULL, NULL);
                    break;
                case BIND_LATEX_RANGE: {
                    LatexDoc *d = v->album->latex;
                    int slen = d ? (int)strlen(latex_doc_get_source(d)) : 0;
                    int s = b->payload.range.start;
                    int l = b->payload.range.length;
                    alive = (s >= 0 && l > 0 && s + l <= slen);
                    break;
                }
                case BIND_INHERIT_TRACK: {
                    int n = (v->album && v->album->tracks)
                            ? (int)v->album->tracks->len : 0;
                    alive = (b->payload.inherit.track_idx >= 0 &&
                             b->payload.inherit.track_idx < n &&
                             b->payload.inherit.track_idx != v->sel_track_idx);
                    break;
                }
                case BIND_INHERIT_MASK:
                    /* mask 条目不参与存活检，不被 focus_binding 渲染。 */
                    alive = TRUE;
                    break;
            }
            if (!alive)
                g_ptr_array_add(stale, g_memdup2(b, sizeof(PairBinding)));
        }
        for (guint i = 0; i < stale->len; i++) {
            const PairBinding *db = g_ptr_array_index(stale, i);
            album_pair_unbind(v->album, v->sel_track_idx, v->sel_pair_idx, db);
        }
        g_ptr_array_unref(stale);
    }
    /* 重新取（清理后），重建行。本轨道直接绑定为第一轮（depth=0，readonly=FALSE）；
     * INHERIT_MASK 不直接渲染，仅用于后续子树展开过滤。 */
    binds = album_pair_get_bindings(v->album, v->sel_track_idx,
                                     v->sel_pair_idx);
    GArray *emph = g_array_new(FALSE, FALSE, sizeof(ProgressAxisHLKey));
    if (binds) {
        /* 单轮渲染：遇到 INHERIT_TRACK 立即就地展开其子树，保证父子条目相邻。 */
        GHashTable *visited = g_hash_table_new(g_direct_hash, g_direct_equal);
        g_hash_table_add(visited, GINT_TO_POINTER(v->sel_track_idx));
        for (guint i = 0; i < binds->len; i++) {
            PairBinding *b = &g_array_index(binds, PairBinding, i);
            if (b->kind == BIND_INHERIT_MASK) continue;
            GtkWidget *row = build_binding_row(v, b, /*depth*/0,
                                                /*readonly*/FALSE,
                                                /*origin*/-1);
            gtk_list_box_append(GTK_LIST_BOX(v->bindings_list), row);
            if (b->kind == BIND_HL_ENV || b->kind == BIND_HL_VAR) {
                int p = -1;
                if (album_find_highlight_by_id(v->album, b->payload.hl_id,
                                                 &p, NULL, NULL)) {
                    ProgressAxisHLKey k = { p, b->payload.hl_id };
                    g_array_append_val(emph, k);
                }
            }
            if (b->kind == BIND_INHERIT_TRACK) {
                expand_inherit_subtree(v, b->payload.inherit.track_idx,
                                        /*depth*/1, visited);
            }
        }
        g_hash_table_destroy(visited);
    }
    progress_axis_set_emphasized(v->axis, emph);
    g_array_unref(emph);
    if (GTK_IS_WIDGET(v->axis_canvas))
        gtk_widget_queue_draw(v->axis_canvas);
    if (v->bindings_add_btn)
        gtk_widget_set_sensitive(GTK_WIDGET(v->bindings_add_btn), TRUE);
    if (v->bindings_clear_btn)
        gtk_widget_set_sensitive(GTK_WIDGET(v->bindings_clear_btn), TRUE);
    if (v->bindings_remove_pair_btn)
        gtk_widget_set_sensitive(GTK_WIDGET(v->bindings_remove_pair_btn), TRUE);
    if (v->bindings_list)
        g_signal_handlers_unblock_by_func(v->bindings_list,
            G_CALLBACK(on_bindings_list_row_selected), v);
}

/* popover closed → 解父并销毁 */
static void
on_bindings_popover_closed(GtkPopover *pop, gpointer user_data)
{
    (void)user_data;
    gtk_widget_unparent(GTK_WIDGET(pop));
}

/* HL 行尾的 env↔var toggle：固定显示"环境/变量"二选一，按钮文字反映
 * 「按下后将切到的状态」，与 check 按钮配合用于添加。 */
static void
on_hl_kind_toggle_clicked(GtkButton *btn, gpointer user_data)
{
    (void)user_data;
    BindKind cur = (BindKind)GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(btn), "hl-kind"));
    BindKind nk = (cur == BIND_HL_ENV) ? BIND_HL_VAR : BIND_HL_ENV;
    g_object_set_data(G_OBJECT(btn), "hl-kind", GINT_TO_POINTER((int)nk));
    gtk_button_set_label(btn, nk == BIND_HL_ENV ? "环境" : "变量");
}

/* popover "确定"：按当前 stack 可见页分发到 4 类绑定的 add 接口 */
static void
on_bindings_popover_confirm(GtkButton *ok_btn, gpointer user_data)
{
    AppView *v = user_data;
    GtkPopover *pop = GTK_POPOVER(g_object_get_data(G_OBJECT(ok_btn),
                                                     "popover"));
    GtkStack   *stk = GTK_STACK  (g_object_get_data(G_OBJECT(ok_btn),
                                                     "stack"));
    if (!v || !pop || !stk) return;
    if (v->sel_track_idx < 0 || v->sel_pair_idx < 0) {
        gtk_popover_popdown(pop);
        return;
    }
    const char *page = gtk_stack_get_visible_child_name(stk);
    if (g_strcmp0(page, "hl") == 0) {
        GtkWidget *list = GTK_WIDGET(g_object_get_data(G_OBJECT(ok_btn),
                                                        "hl-list"));
        if (!list) goto done;
        for (GtkWidget *row = gtk_widget_get_first_child(list); row;
             row = gtk_widget_get_next_sibling(row)) {
            GtkWidget *cb = g_object_get_data(G_OBJECT(row), "check");
            GtkWidget *tg = g_object_get_data(G_OBJECT(row), "toggle");
            if (!cb || !GTK_IS_CHECK_BUTTON(cb)) continue;
            if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(cb))) continue;
            guint64 id = (guint64)GPOINTER_TO_SIZE(
                g_object_get_data(G_OBJECT(row), "hl-id"));
            if (id == 0) continue;
            BindKind k = BIND_HL_ENV;
            if (tg) k = (BindKind)GPOINTER_TO_INT(
                g_object_get_data(G_OBJECT(tg), "hl-kind"));
            album_pair_bind_hl(v->album, v->sel_track_idx,
                                v->sel_pair_idx, id, k);
        }
    } else if (g_strcmp0(page, "latex") == 0) {
        int s = -1, l = 0;
        if (v->latex_view &&
            latex_view_get_selection_bytes(v->latex_view, &s, &l) &&
            s >= 0 && l > 0) {
            album_pair_bind_latex_range(v->album, v->sel_track_idx,
                                         v->sel_pair_idx, s, l);
        }
    } else if (g_strcmp0(page, "inherit") == 0) {
        GtkWidget *list = GTK_WIDGET(g_object_get_data(G_OBJECT(ok_btn),
                                                        "inherit-list"));
        if (!list) goto done;
        for (GtkWidget *c = gtk_widget_get_first_child(list); c;
             c = gtk_widget_get_next_sibling(c)) {
            if (!GTK_IS_CHECK_BUTTON(c)) continue;
            if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(c))) continue;
            int ti = GPOINTER_TO_INT(
                g_object_get_data(G_OBJECT(c), "track-idx"));
            if (!album_pair_bind_inherit_track(v->album, v->sel_track_idx,
                                                 v->sel_pair_idx, ti)) {
                g_warning("inherit bind rejected: self=%d cand=%d "
                          "(环 / 超深度 / 重复)",
                          v->sel_track_idx, ti);
            }
        }
    }
done:
    gtk_popover_popdown(pop);
    refresh_bindings_panel(v);
}

/* 构造 "环境/变量高亮" 页：每行 = check + 标签 + env/var 切换 */
static GtkWidget *
build_hl_page(AppView *v, int *out_n_items)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_widget_set_size_request(sw, 280, 220);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), list);

    int n = 0;
    int np = album_page_count(v->album);
    for (int p = 0; p < np; p++) {
        AlbumPage *pg = album_get_page(v->album, p);
        if (!pg || !pg->doc || !pg->doc->layers) continue;
        for (guint li = 0; li < pg->doc->layers->len; li++) {
            Layer *L = &g_array_index(pg->doc->layers, Layer, li);
            if (L->kind != LAYER_HIGHLIGHT || !L->highlights) continue;
            for (guint ri = 0; ri < L->highlights->len; ri++) {
                HighlightRecord *h = g_ptr_array_index(L->highlights, ri);
                if (!h) continue;
                if (album_pair_has_hl_binding(v->album, v->sel_track_idx,
                                                v->sel_pair_idx, h->id))
                    continue;
                GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
                char *txt = format_highlight_label(v->album, p, h->id);
                GtkWidget *cb = gtk_check_button_new_with_label(txt);
                g_free(txt);
                gtk_widget_set_hexpand(cb, TRUE);
                GtkWidget *tg = gtk_button_new_with_label("环境");
                gtk_widget_add_css_class(tg, "flat");
                gtk_widget_set_tooltip_text(tg, "切换：环境 / 变量");
                g_object_set_data(G_OBJECT(tg), "hl-kind",
                                   GINT_TO_POINTER((int)BIND_HL_ENV));
                g_signal_connect(tg, "clicked",
                                  G_CALLBACK(on_hl_kind_toggle_clicked),
                                  NULL);
                gtk_box_append(GTK_BOX(row), cb);
                gtk_box_append(GTK_BOX(row), tg);
                g_object_set_data(G_OBJECT(row), "hl-id",
                                   GSIZE_TO_POINTER((gsize)h->id));
                g_object_set_data(G_OBJECT(row), "check", cb);
                g_object_set_data(G_OBJECT(row), "toggle", tg);
                gtk_box_append(GTK_BOX(list), row);
                n++;
            }
        }
    }
    if (n == 0) {
        GtkWidget *tip = gtk_label_new(
            "没有可添加的高亮记录（请先到画布画一些高亮）");
        gtk_label_set_wrap(GTK_LABEL(tip), TRUE);
        gtk_widget_set_margin_top(tip, 24);
        gtk_widget_set_margin_bottom(tip, 24);
        gtk_box_append(GTK_BOX(list), tip);
    }
    gtk_box_append(GTK_BOX(outer), sw);
    g_object_set_data(G_OBJECT(outer), "hl-list", list);
    if (out_n_items) *out_n_items = n;
    return outer;
}

/* 构造 "LaTeX 源码" 页：说明 + 当前选区预览 */
static GtkWidget *
build_latex_page(AppView *v, gboolean *out_has_selection)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(outer, 280, 220);
    GtkWidget *tip = gtk_label_new(
        "请先到 LaTeX 页面选中一段源码文本，回到此处点击「确定」。");
    gtk_label_set_wrap(GTK_LABEL(tip), TRUE);
    gtk_label_set_xalign(GTK_LABEL(tip), 0.0);
    gtk_box_append(GTK_BOX(outer), tip);

    int s = -1, l = 0;
    gboolean has_sel = FALSE;
    if (v->latex_view)
        has_sel = latex_view_get_selection_bytes(v->latex_view, &s, &l) &&
                  s >= 0 && l > 0;
    if (has_sel) {
        char *preview = latex_view_extract_text(v->latex_view, s, l);
        char *short_p = NULL;
        if (preview) {
            int n = (int)strlen(preview);
            int show = n < 60 ? n : 60;
            while (show > 0 && (preview[show] & 0xC0) == 0x80) show--;
            short_p = g_strndup(preview, show);
        }
        char *line = g_strdup_printf("当前选区：@%d · %d 字节\n%s%s",
                                       s, l,
                                       short_p ? short_p : "",
                                       (preview && (int)strlen(preview) > 60)
                                       ? " …" : "");
        GtkWidget *lab = gtk_label_new(line);
        gtk_label_set_wrap(GTK_LABEL(lab), TRUE);
        gtk_label_set_xalign(GTK_LABEL(lab), 0.0);
        gtk_widget_set_margin_top(lab, 8);
        gtk_box_append(GTK_BOX(outer), lab);
        g_free(short_p); g_free(preview); g_free(line);
    } else {
        GtkWidget *lab = gtk_label_new("（当前没有选中文本）");
        gtk_widget_set_margin_top(lab, 8);
        gtk_box_append(GTK_BOX(outer), lab);
    }
    if (out_has_selection) *out_has_selection = has_sel;
    return outer;
}

/* 构造 "继承（同轨道）" 页：列出其他轨道复选 */
static GtkWidget *
build_inherit_page(AppView *v, int *out_n_items)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_widget_set_size_request(sw, 280, 220);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), list);

    int n = 0;
    int nt = (v->album && v->album->tracks)
              ? (int)v->album->tracks->len : 0;
    for (int i = 0; i < nt; i++) {
        if (i == v->sel_track_idx) continue;
        /* 环检测 + 深度上限（由 album_inherit_can_bind 内部检查） */
        if (!album_inherit_can_bind(v->album, v->sel_track_idx, i))
            continue;
        /* 重复：self.pair[0] 已含该 INHERIT_TRACK */
        GArray *binds = album_pair_get_bindings(v->album, v->sel_track_idx,
                                                  v->sel_pair_idx);
        gboolean dup = FALSE;
        if (binds) {
            for (guint k = 0; k < binds->len; k++) {
                PairBinding *cur = &g_array_index(binds, PairBinding, k);
                if (cur->kind == BIND_INHERIT_TRACK &&
                    cur->payload.inherit.track_idx == i) { dup = TRUE; break; }
            }
        }
        if (dup) continue;
        Track *t = &g_array_index(v->album->tracks, Track, (guint)i);
        char *label = g_strdup_printf("轨道 %d · %s", i + 1,
                                        t->name ? t->name : "(unnamed)");
        GtkWidget *cb = gtk_check_button_new_with_label(label);
        g_free(label);
        g_object_set_data(G_OBJECT(cb), "track-idx", GINT_TO_POINTER(i));
        gtk_box_append(GTK_BOX(list), cb);
        n++;
    }
    if (n == 0) {
        GtkWidget *tip = gtk_label_new(
            "没有可继承的轨道（成环 / 超过 8 层深度 / 已绑定均被过滤）");
        gtk_label_set_wrap(GTK_LABEL(tip), TRUE);
        gtk_widget_set_margin_top(tip, 24);
        gtk_widget_set_margin_bottom(tip, 24);
        gtk_box_append(GTK_BOX(list), tip);
    }
    gtk_box_append(GTK_BOX(outer), sw);
    g_object_set_data(G_OBJECT(outer), "inherit-list", list);
    if (out_n_items) *out_n_items = n;
    return outer;
}

/* "+ 添加"按钮：弹 popover，内含 StackSwitcher + Stack 三页 */
static void
on_bindings_add_clicked(GtkButton *btn, gpointer user_data)
{
    AppView *v = user_data;
    if (!v) return;
    if (v->sel_track_idx < 0 || v->sel_pair_idx < 0) return;

    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), TRUE);
    gtk_widget_set_parent(pop, GTK_WIDGET(btn));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 8);
    gtk_widget_set_margin_end(vbox, 8);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);

    GtkWidget *stack    = gtk_stack_new();
    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher),
                                  GTK_STACK(stack));
    gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(vbox), switcher);

    int n_hl = 0, n_inh = 0;
    gboolean has_sel = FALSE;
    GtkWidget *hl_page  = build_hl_page(v, &n_hl);
    GtkWidget *tex_page = build_latex_page(v, &has_sel);
    GtkWidget *inh_page = build_inherit_page(v, &n_inh);
    gtk_stack_add_titled(GTK_STACK(stack), hl_page,  "hl",      "环境/变量");
    gtk_stack_add_titled(GTK_STACK(stack), tex_page, "latex",   "LaTeX");
    gtk_stack_add_titled(GTK_STACK(stack), inh_page, "inherit", "继承轨道");
    gtk_box_append(GTK_BOX(vbox), stack);

    GtkWidget *bbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(bbox, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("取消");
    GtkWidget *ok = gtk_button_new_with_label("确定");
    gtk_widget_add_css_class(ok, "suggested-action");
    gtk_widget_set_sensitive(ok,
        (n_hl > 0) || has_sel || (n_inh > 0));
    gtk_box_append(GTK_BOX(bbox), cancel);
    gtk_box_append(GTK_BOX(bbox), ok);
    gtk_box_append(GTK_BOX(vbox), bbox);

    gtk_popover_set_child(GTK_POPOVER(pop), vbox);

    /* 把上下文挂到 ok 按钮：popover / stack / 每页内部 list */
    g_object_set_data(G_OBJECT(ok), "popover",      pop);
    g_object_set_data(G_OBJECT(ok), "stack",        stack);
    g_object_set_data(G_OBJECT(ok), "hl-list",
                       g_object_get_data(G_OBJECT(hl_page),  "hl-list"));
    g_object_set_data(G_OBJECT(ok), "inherit-list",
                       g_object_get_data(G_OBJECT(inh_page), "inherit-list"));

    g_signal_connect(ok, "clicked",
                     G_CALLBACK(on_bindings_popover_confirm), v);
    g_signal_connect_swapped(cancel, "clicked",
                             G_CALLBACK(gtk_popover_popdown), pop);
    g_signal_connect(pop, "closed",
                     G_CALLBACK(on_bindings_popover_closed), NULL);

    gtk_popover_popup(GTK_POPOVER(pop));
}

/* "清空"按钮 */
static void
on_bindings_clear_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    AppView *v = user_data;
    if (!v) return;
    if (v->sel_track_idx < 0 || v->sel_pair_idx < 0) return;
    album_pair_clear_bindings(v->album, v->sel_track_idx, v->sel_pair_idx);
    refresh_bindings_panel(v);
}

/* "删除激活区域"按钮：从轨道中移除当前选中的 HandlePair。
 * 删除后选中态置 -1，触发 on_album_changed 重建轨道行 + 进度轴 emph 清理。 */
static void
on_remove_active_pair_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    AppView *v = user_data;
    if (!v) return;
    if (v->sel_track_idx < 0 || v->sel_pair_idx < 0) return;
    int track_idx = v->sel_track_idx;
    int pair_idx  = v->sel_pair_idx;
    /* 显式重置选中态，避免在 on_album_changed 越界裁齐之前被重绘读取到旧值 */
    v->sel_track_idx = -1;
    v->sel_pair_idx  = -1;
    if (!album_track_remove_pair(v->album, track_idx, pair_idx)) return;
    /* 触发统一的变更通道：populate_tracks + refresh_bindings_panel */
    on_album_changed(NULL, v);
}

/* 单击激活区域：更新选中状态并联动栏目+进度轴 emph */
static void
on_pair_selected(int track_idx, int pair_idx, gpointer user_data)
{
    AppView *v = user_data;
    if (!v) return;
    v->sel_track_idx = track_idx;
    v->sel_pair_idx  = pair_idx;
    /* 同步所有 bar 的视觉选中态 */
    if (v->track_container) {
        int i = 0;
        for (GtkWidget *c = gtk_widget_get_first_child(v->track_container);
             c; c = gtk_widget_get_next_sibling(c), i++) {
            if (i == track_idx)
                track_row_set_selected_pair(c, pair_idx);
            else
                track_row_set_selected_pair(c, -1);
        }
    }
    refresh_bindings_panel(v);
}

/* ─── 滚动同步 ─────────────────────────────────────────────── */
/*  · 水平：header 直接使用 body 的 hadjustment，两者始终同步。
 *  · 垂直：sidebar 与 body 共享 vadjustment，同步轨道行与名称行。 */
static void
wire_scroll_sync(AppView *v)
{
    GtkAdjustment *body_h = gtk_scrolled_window_get_hadjustment(v->body_sw);
    gtk_scrolled_window_set_hadjustment(v->header_sw, body_h);

    GtkAdjustment *body_v = gtk_scrolled_window_get_vadjustment(v->body_sw);
    if (v->sidebar_sw && body_v)
        gtk_scrolled_window_set_vadjustment(v->sidebar_sw, body_v);
}

/* ─── 一次性安装应用全局 CSS ─────────────────────────────────────── */

static void
install_app_css(void)
{
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css,
        "/com/github/notework/style.css");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* ─── AppView 释放（挂在 main_window 上，窗口销毁时调用） ─────── */

static void
appview_free(gpointer data)
{
    AppView *v = data;
    if (!v) return;
    /* axis 持有 canvas 的 draw_func 与 gesture，必须在 album_free 之前
     * 解除（draw 期间访问 album），因此先 free axis 再 free album。 */
    if (v->axis) {
        progress_axis_free(v->axis);
        v->axis = NULL;
    }
    if (v->album) {
        album_free(v->album);
        v->album = NULL;
    }
    g_free(v);
}

/* ─── 应用生命周期 ───────────────────────────────────────────────── */

static void
on_startup(GApplication *app, gpointer user_data)
{
    (void)app; (void)user_data;
    install_app_css();
}

/* ─── 截图便签按钮：启动 notework-snip 子进程 ───────────────────── */
static void
on_snip_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    (void)user_data;
    GError *err = NULL;
    /* 优先 PATH；fallback 到开发期 ./build/notework-snip */
    const char *argv1[] = { "notework-snip", NULL };
    GSubprocess *sp = g_subprocess_newv((const gchar * const *)argv1,
        G_SUBPROCESS_FLAGS_NONE, &err);
    if (!sp) {
        g_clear_error(&err);
        const char *argv2[] = { "./build/notework-snip", NULL };
        sp = g_subprocess_newv((const gchar * const *)argv2,
            G_SUBPROCESS_FLAGS_NONE, &err);
    }
    if (!sp) {
        g_warning("启动 notework-snip 失败: %s",
                  err ? err->message : "(unknown)");
        g_clear_error(&err);
        return;
    }
    g_object_unref(sp);
}

/* ─── G_APPLICATION_HANDLES_OPEN：远程实例传入文件 ───────────────
 * notework-snip「保存到相册」会用 g_subprocess 启动 `notework <path>`，
 * 由 GApplication 单实例机制路由到当前正在运行的主程序，触发本回调。
 * 我们把 GFile 列表导入到当前 main_window 的 album，并刷新 UI。 */
static void
on_open(GApplication *app, gpointer files_p, gint n_files,
        const char *hint, gpointer user_data)
{
    (void)hint; (void)user_data;
    if (n_files <= 0 || !files_p) return;
    /* 取主窗口（首次启动时尚未创建，先 activate 拉起视图） */
    GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));
    if (!windows) {
        g_application_activate(app);
        windows = gtk_application_get_windows(GTK_APPLICATION(app));
    }
    if (!windows) return;
    GtkWindow *win = GTK_WINDOW(windows->data);
    AppView *v = g_object_get_data(G_OBJECT(win), "app-view");
    if (!v || !v->album) return;
    GFile **files = (GFile **)files_p;
    album_import_files(v->album, files, n_files, NULL, NULL);
    /* 切换到「相册」页，方便用户立即看到新导入的图片 */
    if (v->main_stack)
        adw_view_stack_set_visible_child_name(v->main_stack, "album");
    /* 刷新 album_view 与进度轴/轨道 */
    if (v->album_view)
        album_window_refresh(v->album_view);
    on_album_changed(v->album_view, v);
    gtk_window_present(win);
}

static void
on_activate(GApplication *app, gpointer user_data)
{
    (void)user_data;
    GtkBuilder *builder = gtk_builder_new_from_resource(
        "/com/github/notework/window.ui");

    AppView *v = g_new0(AppView, 1);
    v->album            = album_new();
    v->sel_track_idx    = -1;
    v->sel_pair_idx     = -1;
    v->album_holder     = GTK_WIDGET(gtk_builder_get_object(builder, "album_holder"));
    v->axis_canvas      = GTK_WIDGET(gtk_builder_get_object(builder, "progress_axis_canvas"));
    v->track_container  = GTK_WIDGET(gtk_builder_get_object(builder, "track_container"));
    v->sidebar_container= GTK_WIDGET(gtk_builder_get_object(builder, "sidebar_container"));
    v->header_sw        = GTK_SCROLLED_WINDOW(gtk_builder_get_object(builder, "header_scroller"));
    v->body_sw          = GTK_SCROLLED_WINDOW(gtk_builder_get_object(builder, "body_scroller"));
    v->sidebar_sw       = GTK_SCROLLED_WINDOW(gtk_builder_get_object(builder, "sidebar_scroller"));
    v->main_stack       = ADW_VIEW_STACK(gtk_builder_get_object(builder, "main_stack"));
    v->bindings_title   = GTK_WIDGET(gtk_builder_get_object(builder, "bindings_title"));
    v->bindings_list    = GTK_WIDGET(gtk_builder_get_object(builder, "bindings_list"));
    v->bindings_add_btn = GTK_BUTTON(gtk_builder_get_object(builder, "bindings_add_btn"));
    v->bindings_clear_btn= GTK_BUTTON(gtk_builder_get_object(builder, "bindings_clear_btn"));
    v->bindings_remove_pair_btn = GTK_BUTTON(gtk_builder_get_object(builder, "bindings_remove_pair_btn"));
    v->latex_holder     = GTK_WIDGET(gtk_builder_get_object(builder, "latex_holder"));

    /* 进度轴：在装填 track 行之前构造，以便后者拿到 content_width */
    v->axis = progress_axis_new(GTK_DRAWING_AREA(v->axis_canvas), v->album);
    progress_axis_refresh(v->axis);
    /* 播放头拖动手势（点击/拖动进度轴移动播放头） */
    progress_axis_setup_playhead(v->axis, v->track_container);

    /* 装填相册编辑视图（共享 album） */
    GtkWidget *alb_view = album_view_new(v->album);
    v->album_view = alb_view;
    gtk_box_append(GTK_BOX(v->album_holder), alb_view);

    /* 装填 LaTeX 页：按需创建的 LatexDoc，插入 latex_holder 容器 */
    if (v->latex_holder) {
        LatexDoc *ld = album_get_latex_doc(v->album);
        v->latex_view = latex_view_new(ld);
        if (v->latex_view)
            gtk_box_append(GTK_BOX(v->latex_holder), v->latex_view);
    }

    /* 注册变更通道：相册任何修改 → 进度轴重算 + 轨道行重填 */
    album_view_set_changed_cb(alb_view, on_album_changed, v);

    /* 滚动同步 + 首次轨道装填 */
    wire_scroll_sync(v);
    populate_tracks(v);
    /* 进度轴单击拾取高亮→跳转（Task 7）。
     * 采用 CAPTURE 阶段监听 pressed，命中高亮时 CLAIMED 序列，避免 playhead 的
     * GtkGestureDrag 在 drag-begin 中抢占。未命中时不 claim，drag 按原行为接管。 */
    GtkGesture *axis_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(axis_click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(
        GTK_EVENT_CONTROLLER(axis_click), GTK_PHASE_CAPTURE);
    g_signal_connect(axis_click, "pressed",
                     G_CALLBACK(on_axis_click_pressed), v);
    gtk_widget_add_controller(v->axis_canvas, GTK_EVENT_CONTROLLER(axis_click));

    /* 绑定栏目按钮 */
    if (v->bindings_add_btn)
        g_signal_connect(v->bindings_add_btn, "clicked",
                         G_CALLBACK(on_bindings_add_clicked), v);
    if (v->bindings_clear_btn)
        g_signal_connect(v->bindings_clear_btn, "clicked",
                         G_CALLBACK(on_bindings_clear_clicked), v);
    if (v->bindings_remove_pair_btn)
        g_signal_connect(v->bindings_remove_pair_btn, "clicked",
                         G_CALLBACK(on_remove_active_pair_clicked), v);
    if (v->bindings_list)
        g_signal_connect(v->bindings_list, "row-selected",
                         G_CALLBACK(on_bindings_list_row_selected), v);
    /* 首次初始化栏目状态（设为未选中样式、按钮置灰） */
    refresh_bindings_panel(v);

    /* 连接「+ 新建轨道」按钮 */
    GtkButton *add_btn = GTK_BUTTON(
        gtk_builder_get_object(builder, "track_add_button"));
    if (add_btn)
        g_signal_connect(add_btn, "clicked",
                         G_CALLBACK(on_track_add_clicked), v);

    /* HeaderBar：截图便签按钮 → 启动 notework-snip 子进程 */
    GtkButton *snip_btn = GTK_BUTTON(
        gtk_builder_get_object(builder, "snip_btn"));
    if (snip_btn)
        g_signal_connect(snip_btn, "clicked",
                         G_CALLBACK(on_snip_clicked), v);

    GtkWindow *win = GTK_WINDOW(
        gtk_builder_get_object(builder, "main_window"));
    gtk_window_set_application(win, GTK_APPLICATION(app));

    /* AppView 生命周期跟随窗口 */
    g_object_set_data_full(G_OBJECT(win), "app-view", v, appview_free);

    gtk_window_present(win);
    g_object_unref(builder);
}

/* ─── 主入口 ─────────────────────────────────────────────────────── */

int
main(int argc, char *argv[])
{
    AdwApplication *app = adw_application_new(
        "com.github.notework", G_APPLICATION_HANDLES_OPEN);

    g_signal_connect(app, "startup",  G_CALLBACK(on_startup),  NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open",     G_CALLBACK(on_open),     NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
