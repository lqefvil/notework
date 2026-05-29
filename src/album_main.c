/**
 * album_main.c — notework-album 独立可执行入口
 *
 * 仅做三件事：
 *   1) 启动时安装与主程序相同的 CSS（共用 style.css）；
 *   2) 激活时通过 album_window_new() 构造相册窗口；
 *   3) 把窗口与本 AdwApplication 关联并 present。
 */
#include "album.h"
#include <string.h>

/* GTK4 中 GtkSpinButton 内嵌 GtkText 在多种焦点链路径下（popover autohide、
 * widget 重建销毁、HeaderBar spin 失焦等）会输出一条诊断性警告：
 *   "GtkText - did not receive a focus-out event."
 * 该警告不影响功能，本过滤器仅静默这一条，其他警告与错误不受影响。 */
static GLogWriterOutput log_writer_filter(GLogLevelFlags level,
                                          const GLogField *fields,
                                          gsize n_fields,
                                          gpointer user_data) {
    for (gsize i = 0; i < n_fields; i++) {
        if (g_strcmp0(fields[i].key, "MESSAGE") == 0 &&
            fields[i].value != NULL &&
            strstr((const char *)fields[i].value,
                   "did not receive a focus-out event") != NULL) {
            return G_LOG_WRITER_HANDLED;
        }
    }
    return g_log_writer_default(level, fields, n_fields, user_data);
}

static void install_app_css(void) {
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css,
        "/com/github/notework/style.css");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static void on_startup(GApplication *app, gpointer user_data) {
    (void)app; (void)user_data;
    install_app_css();
}

static void on_activate(GApplication *app, gpointer user_data) {
    (void)user_data;
    GtkWidget *win = album_window_new();
    gtk_window_set_application(GTK_WINDOW(win), GTK_APPLICATION(app));
    gtk_window_present(GTK_WINDOW(win));
}

/* 命令行 / 远程实例传入 GFile 列表时调用：
 * 复用 activate 创建窗口 → 通过 album_view_new()/album_window_new() 暴露
 * 的 view 取 album → album_import_files 导入。 */
static void on_open(GApplication *app, gpointer files_p, gint n_files,
                    const char *hint, gpointer user_data) {
    (void)hint; (void)user_data;
    /* 取得（或创建）相册主窗口 */
    GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));
    GtkWindow *win = windows ? GTK_WINDOW(windows->data) : NULL;
    if (!win) {
        GtkWidget *w = album_window_new();
        gtk_window_set_application(GTK_WINDOW(w), GTK_APPLICATION(app));
        win = GTK_WINDOW(w);
    }
    /* album_window_new 内部把 Album* 通过 g_object_set_data 挂在 window 上，
     * key 见 album_window.c 中 "album-state" 或类似；这里走 view API
     * 取不到，改为通过 album_view_get_album_from_window 公共 API。
     * 若该 API 缺失，先 present 窗口让用户用菜单导入 —— 当前最小实现。 */
    (void)files_p; (void)n_files;
    /* 直接借助 album_window_new 已挂载的 album 状态导入。
     * 实现见 album_window.c：暴露 album_window_get_album()。 */
    Album *a = album_window_get_album(GTK_WIDGET(win));
    if (a) {
        GFile **files = (GFile **)files_p;
        album_import_files(a, files, n_files, NULL, NULL);
        /* 触发界面刷新（importer 内部不会自动刷新缩略图） */
        album_window_refresh(GTK_WIDGET(win));
    }
    gtk_window_present(win);
}

int main(int argc, char *argv[]) {
    g_log_set_writer_func(log_writer_filter, NULL, NULL);
    AdwApplication *app = adw_application_new(
        "com.github.notework.album", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "startup",  G_CALLBACK(on_startup),  NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open",     G_CALLBACK(on_open),     NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
