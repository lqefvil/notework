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
#include "progress_axis.h"
#include "track_row.h"

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
} AppView;

/* ─── 轨道行装填 ─────────────────────────────────────────────────── */

/* 删除回调：sidebar 中 × 按钮挂 "track-idx" qdata，clicked 时取出。 */
static void on_sidebar_delete_clicked(GtkButton *btn, gpointer user_data);

/* 构造 sidebar 中的一行：名称标签 + × 删除按钮。track_idx 通过
 * GINT_TO_POINTER 挂在 × 按钮上。 */
static GtkWidget *
build_sidebar_row(AppView *v, int track_idx, const char *name)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_size_request(row, -1, 32);
    /* 显式 hexpand=false，阻断内部 label hexpand=true 沿 row 向上
     * 冒泡到 sidebar_scroller。 */
    gtk_widget_set_hexpand(row, FALSE);

    GtkWidget *lbl = gtk_label_new(name ? name : "(unnamed)");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lbl, TRUE);
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
    progress_axis_refresh(v->axis);
    populate_tracks(v);
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

static void
on_activate(GApplication *app, gpointer user_data)
{
    (void)user_data;

    GtkBuilder *builder = gtk_builder_new_from_resource(
        "/com/github/notework/window.ui");

    AppView *v = g_new0(AppView, 1);
    v->album            = album_new();
    v->album_holder     = GTK_WIDGET(gtk_builder_get_object(builder, "album_holder"));
    v->axis_canvas      = GTK_WIDGET(gtk_builder_get_object(builder, "progress_axis_canvas"));
    v->track_container  = GTK_WIDGET(gtk_builder_get_object(builder, "track_container"));
    v->sidebar_container= GTK_WIDGET(gtk_builder_get_object(builder, "sidebar_container"));
    v->header_sw        = GTK_SCROLLED_WINDOW(gtk_builder_get_object(builder, "header_scroller"));
    v->body_sw          = GTK_SCROLLED_WINDOW(gtk_builder_get_object(builder, "body_scroller"));
    v->sidebar_sw       = GTK_SCROLLED_WINDOW(gtk_builder_get_object(builder, "sidebar_scroller"));

    /* 进度轴：在装填 track 行之前构造，以便后者拿到 content_width */
    v->axis = progress_axis_new(GTK_DRAWING_AREA(v->axis_canvas), v->album);
    progress_axis_refresh(v->axis);
    /* 播放头拖动手势（点击/拖动进度轴移动播放头） */
    progress_axis_setup_playhead(v->axis, v->track_container);

    /* 装填相册编辑视图（共享 album） */
    GtkWidget *alb_view = album_view_new(v->album);
    gtk_box_append(GTK_BOX(v->album_holder), alb_view);

    /* 注册变更通道：相册任何修改 → 进度轴重算 + 轨道行重填 */
    album_view_set_changed_cb(alb_view, on_album_changed, v);

    /* 滚动同步 + 首次轨道装填 */
    wire_scroll_sync(v);
    populate_tracks(v);

    /* 连接「+ 新建轨道」按钮 */
    GtkButton *add_btn = GTK_BUTTON(
        gtk_builder_get_object(builder, "track_add_button"));
    if (add_btn)
        g_signal_connect(add_btn, "clicked",
                         G_CALLBACK(on_track_add_clicked), v);

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
        "com.github.notework", G_APPLICATION_DEFAULT_FLAGS);

    g_signal_connect(app, "startup",  G_CALLBACK(on_startup),  NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
