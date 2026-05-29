/**
 * tokensam_main.c — notework-tokensam 独立可执行入口
 *
 * 仅做三件事：
 *   1) 启动时安装与主程序相同的 CSS（共用 style.css）；
 *   2) 激活时通过 tokensam_window_new() 构造主窗口；
 *   3) 把窗口与本 AdwApplication 关联并 present。
 */
#include <adwaita.h>
#include "tokensam.h"

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
    GtkWidget *win = tokensam_window_new();
    gtk_window_set_application(GTK_WINDOW(win), GTK_APPLICATION(app));
    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char *argv[]) {
    AdwApplication *app = adw_application_new(
        "com.github.notework.tokensam", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "startup",  G_CALLBACK(on_startup),  NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
