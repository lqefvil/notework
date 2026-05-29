/**
 * snip_main.c — notework-snip 独立可执行
 *
 * 功能：
 *   1) 「截图」按钮：调 xdg-desktop-portal Screenshot.Screenshot
 *      （interactive=true）让用户框选屏幕区域，回调拿到 PNG URI 后
 *      用 GdkTexture 加载到 GtkPicture 显示。
 *   2) 「打开图片」按钮：本地 jpg/png → GdkTexture。
 *   3) 「保存到相册」按钮：把当前 Texture 写为 PNG 到 ~/.cache/notework/
 *      snips/snip-<TS>.png，再 g_subprocess 启动 `notework-album <png>`，
 *      利用 album 的 G_APPLICATION_HANDLES_OPEN flag 投递文件。
 *   4) 「置顶」开关：X11 直接 set_keep_above；Wayland 受限，仅尝试。
 */
#include <gtk/gtk.h>
#include <adwaita.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xatom.h>
#endif

typedef struct {
    GtkBuilder *builder;
    GtkWidget  *swin;
    GtkWidget  *picture;
    GtkWidget  *capture_btn;
    GtkWidget  *open_btn;
    GtkWidget  *save_btn;
    GtkWidget  *ontop_btn;
    GtkWidget  *status_label;

    GdkTexture *current;          /* 当前显示图（持引用） */

    /* 置顶状态 */
    gboolean    keep_above;
    guint       raise_tick_id;    /* Wayland fallback：周期性 raise */

    /* portal 调用上下文 */
    GDBusConnection *bus;
    GDBusProxy      *portal;
    guint            response_sub_id;  /* Request.Response 信号订阅 */
} SnipState;

static void snip_state_free(gpointer p) {
    SnipState *s = p;
    if (s->raise_tick_id) g_source_remove(s->raise_tick_id);
    if (s->current) g_object_unref(s->current);
    if (s->portal) g_object_unref(s->portal);
    if (s->bus && s->response_sub_id)
        g_dbus_connection_signal_unsubscribe(s->bus, s->response_sub_id);
    if (s->bus) g_object_unref(s->bus);
    if (s->builder) g_object_unref(s->builder);
    g_free(s);
}

/* ─── 公共：把 GdkTexture 装到 GtkPicture + 状态条 ────────────── */
static void set_current_texture(SnipState *s, GdkTexture *tex,
                                const char *src_label) {
    if (s->current) g_object_unref(s->current);
    s->current = tex ? g_object_ref(tex) : NULL;
    gtk_picture_set_paintable(GTK_PICTURE(s->picture),
                              tex ? GDK_PAINTABLE(tex) : NULL);
    gtk_widget_set_sensitive(s->save_btn, tex != NULL);
    if (tex) {
        char *t = g_strdup_printf("%dx%d  ·  %s",
            gdk_texture_get_width(tex),
            gdk_texture_get_height(tex),
            src_label ? src_label : "");
        gtk_label_set_text(GTK_LABEL(s->status_label), t);
        g_free(t);
    } else {
        gtk_label_set_text(GTK_LABEL(s->status_label), "尚未加载图像");
    }
}

static GdkTexture *load_texture_from_uri(const char *uri, GError **err) {
    GFile *f = g_file_new_for_uri(uri);
    GdkTexture *tex = gdk_texture_new_from_file(f, err);
    g_object_unref(f);
    return tex;
}

/* ─── 「打开图片」 ─────────────────────────────────────────────── */
static void on_open_dialog_done(GObject *src, GAsyncResult *res, gpointer data) {
    SnipState *s = data;
    GError *err = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, &err);
    if (!file) { g_clear_error(&err); return; }
    GdkTexture *tex = gdk_texture_new_from_file(file, &err);
    if (!tex) {
        g_warning("加载图片失败: %s", err ? err->message : "");
        g_clear_error(&err);
    } else {
        char *base = g_file_get_basename(file);
        set_current_texture(s, tex, base);
        g_free(base);
        g_object_unref(tex);
    }
    g_object_unref(file);
}

static void on_open_clicked(GtkButton *b, gpointer data) {
    (void)b;
    SnipState *s = data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "选择图片");
    GtkFileFilter *fl = gtk_file_filter_new();
    gtk_file_filter_set_name(fl, "图片 (jpg/png)");
    gtk_file_filter_add_mime_type(fl, "image/jpeg");
    gtk_file_filter_add_mime_type(fl, "image/png");
    GListStore *fls = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(fls, fl);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(fls));
    gtk_file_dialog_open(dlg, GTK_WINDOW(s->swin), NULL,
                         on_open_dialog_done, s);
    g_object_unref(fl); g_object_unref(fls); g_object_unref(dlg);
}

/* ─── 「保存到相册」：写 PNG → spawn notework-album ────────────── */
static char *texture_save_png(GdkTexture *tex, GError **err) {
    /* ~/.cache/notework/snips/snip-<TS>.png */
    char *dir = g_build_filename(g_get_user_cache_dir(),
                                  "notework", "snips", NULL);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "无法创建缓存目录 %s", dir);
        g_free(dir); return NULL;
    }
    gint64 ts = g_get_real_time();  /* 微秒 */
    char *fname = g_strdup_printf("snip-%" G_GINT64_FORMAT ".png", ts);
    char *path  = g_build_filename(dir, fname, NULL);
    g_free(fname); g_free(dir);
    if (!gdk_texture_save_to_png(tex, path)) {
        g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "GdkTexture 写 PNG 失败");
        g_free(path); return NULL;
    }
    return path;  /* 调用方 g_free */
}

static void on_save_to_album_clicked(GtkButton *b, gpointer data) {
    (void)b;
    SnipState *s = data;
    if (!s->current) return;
    GError *err = NULL;
    char *path = texture_save_png(s->current, &err);
    if (!path) {
        gtk_label_set_text(GTK_LABEL(s->status_label),
            err ? err->message : "保存失败");
        g_clear_error(&err);
        return;
    }
    /* 启动 notework <path>，让当前正在运行的 notework 主程序通过
     * G_APPLICATION_HANDLES_OPEN 接收文件并导入到当前相册视图。
     * 优先 PATH；fallback 到开发期 ./build/notework。
     * 若都失败再 fallback 到独立 notework-album。 */
    const char *argv1[] = { "notework", path, NULL };
    GSubprocess *sp = g_subprocess_newv((const gchar * const *)argv1,
        G_SUBPROCESS_FLAGS_NONE, &err);
    if (!sp) {
        g_clear_error(&err);
        const char *argv2[] = { "./build/notework", path, NULL };
        sp = g_subprocess_newv((const gchar * const *)argv2,
            G_SUBPROCESS_FLAGS_NONE, &err);
    }
    if (!sp) {
        g_clear_error(&err);
        const char *argv3[] = { "notework-album", path, NULL };
        sp = g_subprocess_newv((const gchar * const *)argv3,
            G_SUBPROCESS_FLAGS_NONE, &err);
    }
    if (!sp) {
        g_clear_error(&err);
        const char *argv4[] = { "./build/notework-album", path, NULL };
        sp = g_subprocess_newv((const gchar * const *)argv4,
            G_SUBPROCESS_FLAGS_NONE, &err);
    }
    if (!sp) {
        char *t = g_strdup_printf("已保存 %s，但启动 notework 失败: %s",
            path, err ? err->message : "");
        gtk_label_set_text(GTK_LABEL(s->status_label), t);
        g_free(t);
        g_clear_error(&err);
    } else {
        char *t = g_strdup_printf("已投递到 notework: %s", path);
        gtk_label_set_text(GTK_LABEL(s->status_label), t);
        g_free(t);
        g_object_unref(sp);
    }
    g_free(path);
}

/* ─── 「置顶」 ─────────────────────────────────────────────────── */
/* X11：通过 _NET_WM_STATE ClientMessage 设置/清除 _NET_WM_STATE_ABOVE。
 * 返回 TRUE 表示已发送（仅说明协议层投递成功，最终是否生效取决于 WM）。 */
static gboolean snip_x11_set_keep_above(GtkWidget *win, gboolean above) {
#ifdef GDK_WINDOWING_X11
    GtkNative *nat = GTK_NATIVE(win);
    GdkSurface *surf = nat ? gtk_native_get_surface(nat) : NULL;
    if (!surf) return FALSE;
    GdkDisplay *display = gdk_surface_get_display(surf);
    if (!GDK_IS_X11_DISPLAY(display)) return FALSE;
    Display *xdpy = gdk_x11_display_get_xdisplay(display);
    Window xwin   = gdk_x11_surface_get_xid(surf);
    Atom net_wm_state =
        gdk_x11_get_xatom_by_name_for_display(display, "_NET_WM_STATE");
    Atom net_wm_state_above =
        gdk_x11_get_xatom_by_name_for_display(display, "_NET_WM_STATE_ABOVE");
    XEvent ev = {0};
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = xwin;
    ev.xclient.message_type = net_wm_state;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = above ? 1 : 0;  /* _NET_WM_STATE_ADD : REMOVE */
    ev.xclient.data.l[1]    = (long)net_wm_state_above;
    ev.xclient.data.l[2]    = 0;
    ev.xclient.data.l[3]    = 1;              /* source: application */
    ev.xclient.data.l[4]    = 0;
    XSendEvent(xdpy, DefaultRootWindow(xdpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(xdpy);
    return TRUE;
#else
    (void)win; (void)above;
    return FALSE;
#endif
}

/* Wayland fallback：每 500ms 把窗口 present，模拟 always-on-top。
 * 多数 compositor（Mutter/KWin/wlroots）会把 present 解释为 raise；用户
 * 输入焦点不会被夺走（present 不强制激活）。 */
static gboolean snip_raise_tick(gpointer data) {
    SnipState *s = data;
    if (!s->keep_above || !s->swin) return G_SOURCE_REMOVE;
    /* 仅 raise，不夺焦点（present 在 Wayland 上不一定窃焦） */
    gtk_window_present(GTK_WINDOW(s->swin));
    return G_SOURCE_CONTINUE;
}

/* 窗口被映射（surface 已就绪）时重新应用 keep-above。
 * 用于：
 * 1) 初次 present（默认 active=true 时 toggled 信号不会触发，依赖此回调
 *    把 _NET_WM_STATE_ABOVE 真正发出去）；
 * 2) 截图前 hide → portal 完成后 show，部分 WM 会重置 above 状态。 */
static void on_window_mapped(GtkWidget *widget, gpointer data) {
    (void)widget;
    SnipState *s = data;
    if (!s->keep_above) return;
    gboolean x11_ok = snip_x11_set_keep_above(s->swin, TRUE);
    if (!x11_ok && s->raise_tick_id == 0) {
        /* Wayland 后端：启动 500ms 周期 raise 兜底 */
        s->raise_tick_id = g_timeout_add(500, snip_raise_tick, s);
    }
}

static void on_ontop_toggled(GtkToggleButton *btn, gpointer data) {
    SnipState *s = data;
    s->keep_above = gtk_toggle_button_get_active(btn);
    gboolean x11_ok = snip_x11_set_keep_above(s->swin, s->keep_above);
    /* 取消旧的 fallback 计时 */
    if (s->raise_tick_id) {
        g_source_remove(s->raise_tick_id);
        s->raise_tick_id = 0;
    }
    if (s->keep_above) {
        if (x11_ok) {
            gtk_label_set_text(GTK_LABEL(s->status_label),
                "置顶: 已生效（X11 _NET_WM_STATE_ABOVE）");
        } else {
            /* Wayland 等无 keep-above 协议：开启 500ms 周期 raise 兜底 */
            s->raise_tick_id = g_timeout_add(500, snip_raise_tick, s);
            gtk_label_set_text(GTK_LABEL(s->status_label),
                "置顶: Wayland 无原生协议，已启用 500ms 周期 raise 兜底");
        }
    } else {
        gtk_label_set_text(GTK_LABEL(s->status_label),
            x11_ok ? "置顶: 已取消" : "置顶: 已取消（兜底已停止）");
    }
}

/* ─── 「截图」：xdg-desktop-portal Screenshot ──────────────────── */
/* 截图完成或失败/取消后恢复悬浮窗显示 */
static void restore_window_visible(SnipState *s) {
    if (!s || !s->swin) return;
    gtk_widget_set_visible(s->swin, TRUE);
    gtk_window_present(GTK_WINDOW(s->swin));
    /* 重新应用 keep-above：hide/show 后 X11 _NET_WM_STATE_ABOVE 可能被 WM
     * 清掉，map 后需要再发一次。 */
    if (s->keep_above)
        snip_x11_set_keep_above(s->swin, TRUE);
}

static void on_portal_response(GDBusConnection *conn, const char *sender,
                               const char *path, const char *iface,
                               const char *signal, GVariant *params,
                               gpointer user_data) {
    (void)conn; (void)sender; (void)path; (void)iface; (void)signal;
    SnipState *s = user_data;
    guint32 response = 1;
    GVariant *results = NULL;
    g_variant_get(params, "(u@a{sv})", &response, &results);
    if (response == 0 && results) {
        const char *uri = NULL;
        GVariant *uri_v = g_variant_lookup_value(results, "uri",
            G_VARIANT_TYPE_STRING);
        if (uri_v) uri = g_variant_get_string(uri_v, NULL);
        if (uri && *uri) {
            GError *err = NULL;
            GdkTexture *tex = load_texture_from_uri(uri, &err);
            if (tex) {
                set_current_texture(s, tex, "屏幕截图");
                g_object_unref(tex);
            } else {
                gtk_label_set_text(GTK_LABEL(s->status_label),
                    err ? err->message : "截图加载失败");
                g_clear_error(&err);
            }
        }
        if (uri_v) g_variant_unref(uri_v);
    } else {
        gtk_label_set_text(GTK_LABEL(s->status_label),
            response == 1 ? "用户取消截图" : "截图失败");
    }
    if (results) g_variant_unref(results);
    /* 一次性订阅：取消 */
    if (s->response_sub_id) {
        g_dbus_connection_signal_unsubscribe(s->bus, s->response_sub_id);
        s->response_sub_id = 0;
    }
    /* 截图流程结束：恢复悬浮窗显示 */
    restore_window_visible(s);
}

static void on_screenshot_call_done(GObject *src, GAsyncResult *res,
                                    gpointer data) {
    SnipState *s = data;
    GError *err = NULL;
    GVariant *r = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);
    if (!r) {
        gtk_label_set_text(GTK_LABEL(s->status_label),
            err ? err->message : "portal 调用失败");
        g_clear_error(&err);
        /* 调用失败：立即恢复悬浮窗显示（不会再有 Response 信号） */
        restore_window_visible(s);
        return;
    }
    const char *handle = NULL;
    g_variant_get(r, "(o)", &handle);
    /* 订阅 Request.Response 信号 */
    s->response_sub_id = g_dbus_connection_signal_subscribe(
        s->bus,
        "org.freedesktop.portal.Desktop",
        "org.freedesktop.portal.Request",
        "Response",
        handle,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_portal_response, s, NULL);
    g_variant_unref(r);
}

/* 隐藏悬浮窗后由 g_timeout 触发：真正发起 portal 截图请求 */
static gboolean do_capture_after_hide(gpointer data) {
    SnipState *s = data;
    if (!s->bus) {
        GError *err = NULL;
        s->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
        if (!s->bus) {
            gtk_label_set_text(GTK_LABEL(s->status_label),
                err ? err->message : "无法连接 session bus");
            g_clear_error(&err);
            restore_window_visible(s);
            return G_SOURCE_REMOVE;
        }
    }
    if (!s->portal) {
        GError *err = NULL;
        s->portal = g_dbus_proxy_new_sync(s->bus,
            G_DBUS_PROXY_FLAGS_NONE, NULL,
            "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Screenshot",
            NULL, &err);
        if (!s->portal) {
            gtk_label_set_text(GTK_LABEL(s->status_label),
                err ? err->message : "portal 不可用");
            g_clear_error(&err);
            restore_window_visible(s);
            return G_SOURCE_REMOVE;
        }
    }
    GVariantBuilder ob;
    g_variant_builder_init(&ob, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&ob, "{sv}", "interactive",
                          g_variant_new_boolean(TRUE));
    g_variant_builder_add(&ob, "{sv}", "modal",
                          g_variant_new_boolean(FALSE));
    /* parent_window: 留空（portal 接受 ""） */
    GVariant *args = g_variant_new("(sa{sv})", "", &ob);
    g_dbus_proxy_call(s->portal, "Screenshot", args,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL,
        on_screenshot_call_done, s);
    gtk_label_set_text(GTK_LABEL(s->status_label),
        "已发起截图请求，请框选屏幕区域…");
    return G_SOURCE_REMOVE;
}

static void on_capture_clicked(GtkButton *b, gpointer data) {
    (void)b;
    SnipState *s = data;
    /* 1) 先隐藏悬浮窗，避免遮挡截图区域 */
    gtk_widget_set_visible(s->swin, FALSE);
    gtk_label_set_text(GTK_LABEL(s->status_label),
        "正在隐藏悬浮窗后发起截图…");
    /* 2) 等 250ms 让 compositor 完成隐藏，再发起 portal 调用 */
    g_timeout_add(250, do_capture_after_hide, s);
}

/* ─── 装配 ─────────────────────────────────────────────────────── */
static GtkWidget *build_window(GApplication *app) {
    SnipState *s = g_new0(SnipState, 1);
    GtkBuilder *b = gtk_builder_new_from_resource(
        "/com/github/notework/snip/snip_window.ui");
    s->builder = b;
    s->swin    = GTK_WIDGET(gtk_builder_get_object(b, "swin"));
    s->picture = GTK_WIDGET(gtk_builder_get_object(b, "picture"));
    s->capture_btn = GTK_WIDGET(gtk_builder_get_object(b, "capture_btn"));
    s->open_btn    = GTK_WIDGET(gtk_builder_get_object(b, "open_btn"));
    s->save_btn    = GTK_WIDGET(gtk_builder_get_object(b, "save_to_album_btn"));
    s->ontop_btn   = GTK_WIDGET(gtk_builder_get_object(b, "ontop_btn"));
    s->status_label= GTK_WIDGET(gtk_builder_get_object(b, "status_label"));

    g_signal_connect(s->capture_btn, "clicked", G_CALLBACK(on_capture_clicked), s);
    g_signal_connect(s->open_btn,    "clicked", G_CALLBACK(on_open_clicked),    s);
    g_signal_connect(s->save_btn,    "clicked", G_CALLBACK(on_save_to_album_clicked), s);
    g_signal_connect(s->ontop_btn,   "toggled", G_CALLBACK(on_ontop_toggled),   s);

    /* 默认开启置顶（便签语义）。toggle 在 UI 中已 active=true，但 toggled
     * 信号在 ontop_btn 默认为 active 的初始构造期 *不会* 自动触发，需要
     * 显式同步 keep_above 状态：先记录意图，再在窗口 map 时由 on_window_mapped
     * 把 _NET_WM_STATE_ABOVE 真正发出去（surface 此时已就绪）。 */
    s->keep_above = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->ontop_btn));
    /* 每次窗口被映射时（含初次 present、hide/show）重新应用 keep-above。
     * X11 _NET_WM_STATE_ABOVE 必须在 surface 存在后发送才有效；hide/show 后
     * 部分 WM 也会清除 above 状态，map 时重发可保证持久。 */
    g_signal_connect(s->swin, "map", G_CALLBACK(on_window_mapped), s);

    g_object_set_data_full(G_OBJECT(s->swin), "snip-state", s, snip_state_free);
    gtk_window_set_application(GTK_WINDOW(s->swin), GTK_APPLICATION(app));
    return s->swin;
}

static void on_activate(GApplication *app, gpointer ud) {
    (void)ud;
    GtkWidget *w = build_window(app);
    gtk_window_present(GTK_WINDOW(w));
}

int main(int argc, char *argv[]) {
    /* 关键：强制走 X11（含 XWayland）后端。GNOME/KDE 等主流 compositor
     * 在 Wayland 会话下没有原生 keep-above 协议，但 XWayland 客户端的
     * _NET_WM_STATE_ABOVE 通常被 Mutter/KWin 尊重——这是当前我们能稳定
     * 实现「悬浮便签置顶」的最现实路径。纯 Wayland（sway/wlroots 无
     * XWayland）会自动回退到 wayland 后端，并启用 500ms 周期 raise 兜底。
     * gdk_set_allowed_backends 必须在任何 GDK display 创建前调用。 */
    gdk_set_allowed_backends("x11,wayland");
    AdwApplication *app = adw_application_new(
        "com.github.notework.snip", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
