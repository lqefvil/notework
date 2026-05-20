/**
 * notework — 主窗口：基准行 + 虚拟列表（GtkListView）
 * 编译标准: C11，无 SIMD / 内联汇编 / 字节序假设
 *
 * 文件分工：
 *   src/window.ui   —— 主窗口骨架（GtkBuilder XML）
 *   src/row.ui      —— 数据行模板（GtkBuilderListItemFactory）
 *   src/style.css   —— 全局样式
 * 三者通过 GResource 在编译期嵌入到可执行文件中。
 *
 * 本文件只做四件事：
 *   1) 应用启动一次性安装 CSS（startup 信号）
 *   2) 激活时构建窗口、装入数据模型
 *   3) 把 +/- 按钮挂上模型增删回调
 *   4) 把基准行宽度 / 水平滚动位置与数据列表保持同步
 */

#include <adwaita.h>

/* 初始行数：演示用；GtkListView 是虚拟列表，把它改到 1_000_000 也能瞬开。 */
#define INITIAL_ROWS 1000

/* ─── 按钮回调：模型增删 ───────────────────────────────────────────── */

static void
on_add_row_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkStringList *list = GTK_STRING_LIST(user_data);

    char buf[64];
    g_snprintf(buf, sizeof buf, "数据行 #%u",
               g_list_model_get_n_items(G_LIST_MODEL(list)) + 1);
    gtk_string_list_append(list, buf);
}

static void
on_del_row_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkStringList *list = GTK_STRING_LIST(user_data);

    guint n = g_list_model_get_n_items(G_LIST_MODEL(list));
    if (n > 0)
        gtk_string_list_remove(list, n - 1);
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

/* ─── 构建初始数据模型 ───────────────────────────────────────────── */

static GtkStringList *
build_initial_model(void)
{
    GtkStringList *m = gtk_string_list_new(NULL);
    char buf[256];
    for (guint i = 1; i <= INITIAL_ROWS; i++) {
        if (i % 7 == 0)
            g_snprintf(buf, sizeof buf,
                "数据行 #%u — 超长内容示例：这一行文字特别长用于演示水平滚动条的出现与拖动", i);
        else
            g_snprintf(buf, sizeof buf, "数据行 #%u", i);
        gtk_string_list_append(m, buf);
    }
    return m;
}

/* ─── 把 +/- 按钮接到模型 ────────────────────────────────────────── */

static void
wire_actions(GtkBuilder *b, GListModel *model)
{
    g_signal_connect(gtk_builder_get_object(b, "add_row_btn"),
                     "clicked", G_CALLBACK(on_add_row_clicked), model);
    g_signal_connect(gtk_builder_get_object(b, "del_row_btn"),
                     "clicked", G_CALLBACK(on_del_row_clicked), model);
}

/* ─── 把基准行与数据列表的水平方向保持一致 ──────────────────────────
 *
 *   (a) baseline_label.width-request  ← body.hadjustment.upper
 *       表头内容宽度跟随数据区总宽度，否则 header 上限 = 0、滚不动
 *   (b) header.hadjustment.value      ← body.hadjustment.value
 *       拖动数据区水平滑块时，基准行同步平移；单向以避免反向夾紧
 */
static void
wire_horizontal_sync(GtkBuilder *b)
{
    GtkAdjustment *body_h = gtk_scrolled_window_get_hadjustment(
        GTK_SCROLLED_WINDOW(gtk_builder_get_object(b, "body_scroller")));
    GtkAdjustment *head_h = gtk_scrolled_window_get_hadjustment(
        GTK_SCROLLED_WINDOW(gtk_builder_get_object(b, "header_scroller")));
    GObject *baseline = gtk_builder_get_object(b, "baseline_label");

    g_object_bind_property(body_h, "upper",
                           baseline, "width-request",
                           G_BINDING_SYNC_CREATE);
    g_object_bind_property(body_h, "value",
                           head_h, "value",
                           G_BINDING_SYNC_CREATE);
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

    /* 数据模型注入到 XML 里预先存在的 GtkNoSelection */
    GtkStringList  *model = build_initial_model();
    GtkNoSelection *sel   = GTK_NO_SELECTION(
        gtk_builder_get_object(builder, "data_selection"));
    gtk_no_selection_set_model(sel, G_LIST_MODEL(model));
    g_object_unref(model);  /* selection 已接管引用 */

    wire_actions(builder, gtk_no_selection_get_model(sel));
    wire_horizontal_sync(builder);

    GtkWindow *win = GTK_WINDOW(
        gtk_builder_get_object(builder, "main_window"));
    gtk_window_set_application(win, GTK_APPLICATION(app));
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
