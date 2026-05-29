/**
 * tokensam_window.c — Token 级 SAM 主窗口
 *
 * 职责：
 *   - 加载 tokensam_window.ui 资源；
 *   - 维护 Lexicon、当前 SAM 及画布的生命周期；
 *   - 处理词典编辑（添加/删除/合并/拆分）；
 *   - 处理「构建 SAM」按钮：tokenize -> sam_build -> 推送到画布；
 *   - 在 status_label 显示节点/边数与分词错误；
 *   - 通过 GtkSwitch 控制 suffix link 显示。
 */
#include <adwaita.h>
#include "tokensam.h"

#include <string.h>

typedef struct {
    GtkWidget    *window;
    Lexicon      *lex;
    Sam          *sam;
    GtkListBox   *lex_list;
    GtkEntry     *lex_entry;
    GtkButton    *lex_add_btn;
    GtkButton    *lex_remove_btn;
    GtkButton    *lex_merge_btn;
    GtkButton    *lex_split_btn;
    GtkButton    *lex_select_all_btn;
    GtkButton    *lex_clear_sel_btn;
    GtkEntry     *sep_entry;
    GtkDropDown  *mode_dropdown;
    GtkCheckButton *autoadd_check;
    GtkTextView  *input_view;
    GtkTextBuffer*input_buffer;
    GtkButton    *build_btn;
    GtkButton    *append_btn;
    GtkSwitch    *suffix_switch;
    GtkLabel     *status_label;
    GtkBox       *canvas_holder;
    GtkWidget    *canvas;
    GtkTextTag   *err_tag;
    /* GSA 多批次累计统计 */
    int           batch_count;     /* 已构建/追加过的批次数 */
    gsize         total_tokens;    /* 累计 token 数 */
} TWin;

static void twin_free(gpointer p) {
    TWin *w = (TWin *)p;
    if (!w) return;
    /* 注意：本回调由 g_object_set_data_full 在 window dispose 阶段调用，
     * 此时 canvas 等子组件早已 dispose，canvas_free 已自动释放 layout。
     * 这里只能释放纯 C 数据，绝不可再访问 w->canvas（悬挂指针），
     * 否则会触发 GLib-GObject-CRITICAL: invalid unclassed pointer。 */
    if (w->sam) sam_free(w->sam);
    if (w->lex) lexicon_free(w->lex);
    g_free(w);
}

/* ─── 词典列表渲染 ──────────────────────────────────────────── */

static GtkWidget *make_lex_row(int idx, const LexEntry *e) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start (box, 6);
    gtk_widget_set_margin_end   (box, 6);
    gtk_widget_set_margin_top   (box, 4);
    gtk_widget_set_margin_bottom(box, 4);

    GtkWidget *text = gtk_label_new(e->text);
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);

    char cidbuf[32];
    g_snprintf(cidbuf, sizeof cidbuf, "类 #%d", e->class_id);
    GtkWidget *cidlbl = gtk_label_new(cidbuf);
    gtk_widget_add_css_class(cidlbl, "dim-label");

    gtk_box_append(GTK_BOX(box), text);
    gtk_box_append(GTK_BOX(box), cidlbl);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data(G_OBJECT(row), "lex-idx", GINT_TO_POINTER(idx));
    return row;
}

static void rebuild_lex_list(TWin *w) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(w->lex_list))) != NULL)
        gtk_list_box_remove(w->lex_list, child);
    int n = lexicon_size(w->lex);
    for (int i = 0; i < n; i++) {
        const LexEntry *e = lexicon_at(w->lex, i);
        gtk_list_box_append(w->lex_list, make_lex_row(i, e));
    }
}

/* 收集当前 lex_list 的多选行索引 */
static GArray *collect_selected_indices(TWin *w) {
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(int));
    GList *sel = gtk_list_box_get_selected_rows(w->lex_list);
    for (GList *p = sel; p; p = p->next) {
        int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(p->data), "lex-idx"));
        g_array_append_val(arr, idx);
    }
    g_list_free(sel);
    /* 升序排序，便于多选删除时倒序处理 */
    int *raw = (int *)arr->data;
    for (guint i = 1; i < arr->len; i++) {
        int v = raw[i], j = (int)i - 1;
        while (j >= 0 && raw[j] > v) { raw[j+1] = raw[j]; j--; }
        raw[j+1] = v;
    }
    return arr;
}

/* ─── 词典编辑回调 ──────────────────────────────────────────── */

static void on_add(GtkButton *btn, gpointer user) {
    (void)btn;
    TWin *w = (TWin *)user;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(w->lex_entry));
    if (!text || !*text) return;
    /* 简单 trim */
    while (*text == ' ' || *text == '\t') text++;
    char *t = g_strdup(text);
    int len = (int)strlen(t);
    while (len > 0 && (t[len-1] == ' ' || t[len-1] == '\t' ||
                        t[len-1] == '\n' || t[len-1] == '\r')) {
        t[--len] = 0;
    }
    if (*t) lexicon_add(w->lex, t);
    g_free(t);
    gtk_editable_set_text(GTK_EDITABLE(w->lex_entry), "");
    rebuild_lex_list(w);
}

static void on_lex_entry_activate(GtkEntry *entry, gpointer user) {
    (void)entry;
    on_add(NULL, user);
}

static void on_remove(GtkButton *btn, gpointer user) {
    (void)btn;
    TWin *w = (TWin *)user;
    GArray *idxs = collect_selected_indices(w);
    /* 倒序删除避免索引位移 */
    for (int i = (int)idxs->len - 1; i >= 0; i--) {
        int v = g_array_index(idxs, int, i);
        lexicon_remove_at(w->lex, v);
    }
    g_array_free(idxs, TRUE);
    rebuild_lex_list(w);
}

static void on_merge(GtkButton *btn, gpointer user) {
    (void)btn;
    TWin *w = (TWin *)user;
    GArray *idxs = collect_selected_indices(w);
    if (idxs->len >= 2) {
        lexicon_merge(w->lex, (const int *)idxs->data, (int)idxs->len);
    }
    g_array_free(idxs, TRUE);
    rebuild_lex_list(w);
}

static void on_split(GtkButton *btn, gpointer user) {
    (void)btn;
    TWin *w = (TWin *)user;
    GArray *idxs = collect_selected_indices(w);
    /* 逐个独立化 */
    for (guint i = 0; i < idxs->len; i++) {
        lexicon_split(w->lex, g_array_index(idxs, int, i));
    }
    g_array_free(idxs, TRUE);
    rebuild_lex_list(w);
}

/* 选中所有词典行 */
static void on_select_all(GtkButton *btn, gpointer user) {
    (void)btn;
    TWin *w = (TWin *)user;
    gtk_list_box_select_all(w->lex_list);
}

/* 清除当前选中 */
static void on_clear_sel(GtkButton *btn, gpointer user) {
    (void)btn;
    TWin *w = (TWin *)user;
    gtk_list_box_unselect_all(w->lex_list);
}

/* 快捷键回调：Ctrl+A 全选 / Esc 清除 */
static gboolean on_lex_shortcut_select_all(GtkWidget *widget,
                                           GVariant *args, gpointer user) {
    (void)widget; (void)args;
    on_select_all(NULL, user);
    return TRUE;
}
static gboolean on_lex_shortcut_clear(GtkWidget *widget,
                                      GVariant *args, gpointer user) {
    (void)widget; (void)args;
    on_clear_sel(NULL, user);
    return TRUE;
}

/* ─── 构建 SAM ──────────────────────────────────────────────── */

static void clear_input_error_tags(TWin *w) {
    GtkTextIter s, e;
    gtk_text_buffer_get_start_iter(w->input_buffer, &s);
    gtk_text_buffer_get_end_iter  (w->input_buffer, &e);
    gtk_text_buffer_remove_tag(w->input_buffer, w->err_tag, &s, &e);
}

static void mark_input_error(TWin *w, int byte_offset, int byte_len) {
    GtkTextIter b_start, b_end;
    gtk_text_buffer_get_start_iter(w->input_buffer, &b_start);
    gtk_text_buffer_get_end_iter  (w->input_buffer, &b_end);
    char *full = gtk_text_buffer_get_text(w->input_buffer, &b_start, &b_end, FALSE);
    if (!full) return;
    int total_bytes = (int)strlen(full);
    if (byte_offset < 0 || byte_offset >= total_bytes) { g_free(full); return; }
    int end_byte = byte_offset + byte_len;
    if (end_byte > total_bytes) end_byte = total_bytes;
    long s_chars = g_utf8_pointer_to_offset(full, full + byte_offset);
    long e_chars = g_utf8_pointer_to_offset(full, full + end_byte);
    g_free(full);
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_offset(w->input_buffer, &s, (int)s_chars);
    gtk_text_buffer_get_iter_at_offset(w->input_buffer, &e, (int)e_chars);
    gtk_text_buffer_apply_tag(w->input_buffer, w->err_tag, &s, &e);
}

/* 公共：把 tokenize 结果落盘成 SAM（is_append=FALSE 重建；TRUE 走 GSA 追加），
 * 同时刷新画布、错误高亮与状态栏。调用方负责释放 tr。 */
static void finalize_with_tr(TWin *w, TokenizeResult *tr, gboolean is_append) {
    if (!is_append) {
        if (w->sam) {
            tokensam_canvas_set_data(w->canvas, NULL, NULL);
            sam_free(w->sam); w->sam = NULL;
        }
        w->sam = sam_build(tr->ids, tr->n);
        w->batch_count = 1;
        w->total_tokens = tr->n;
        tokensam_canvas_set_data(w->canvas, w->sam, w->lex);
    } else {
        if (!w->sam) {
            w->sam = sam_new();
            w->batch_count = 0;
            w->total_tokens = 0;
        }
        /* 撤下画布对 sam 的引用，避免 append 期间 GArray realloc 让画布悬挂 */
        tokensam_canvas_set_data(w->canvas, NULL, NULL);
        sam_append_string(w->sam, tr->ids, tr->n);
        w->batch_count += 1;
        w->total_tokens += tr->n;
        tokensam_canvas_set_data(w->canvas, w->sam, w->lex);
    }

    GString *err_msg = g_string_new(NULL);
    if (tr->errors && tr->errors->len > 0) {
        g_string_append_printf(err_msg, "未识别 token %u 个: ", tr->errors->len);
        for (guint i = 0; i < tr->errors->len; i++) {
            const TokenizeError *te = &g_array_index(tr->errors, TokenizeError, i);
            mark_input_error(w, te->byte_offset, te->byte_len);
            if (i > 0) g_string_append(err_msg, ", ");
            if (i < 5) g_string_append_printf(err_msg, "\"%s\"", te->piece);
            else if (i == 5) { g_string_append(err_msg, "…"); break; }
        }
    }

    char status[320];
    if (!is_append) {
        g_snprintf(status, sizeof status,
                   "[SAM] 批次=1  tokens=%zu  类数=%d  节点=%d  边=%d%s%s",
                   tr->n, lexicon_class_count(w->lex),
                   sam_node_count(w->sam), sam_edge_count(w->sam),
                   (err_msg->len ? "  ⚠ " : ""), err_msg->str);
    } else {
        g_snprintf(status, sizeof status,
                   "[GSA] 批次=%d  本批tokens=%zu  累计=%zu  类数=%d  节点=%d  边=%d%s%s",
                   w->batch_count, tr->n, w->total_tokens,
                   lexicon_class_count(w->lex),
                   sam_node_count(w->sam), sam_edge_count(w->sam),
                   (err_msg->len ? "  ⚠ " : ""), err_msg->str);
    }
    gtk_label_set_text(w->status_label, status);
    g_string_free(err_msg, TRUE);
}

/* ─── 未登录词弹窗（仅 BY_LEXICON + auto_add=FALSE）───────── */

typedef struct { GtkWidget *check, *entry; } UnrecRow;

typedef struct {
    TWin           *w;
    char           *text;       /* 原文副本：response 时用于二次 tokenize */
    gboolean        is_append;
    GArray         *rows;       /* UnrecRow */
    GArray         *pieces;     /* char* 去重未登录段；ctx 释放 */
    TokenizeResult *tr;         /* 第一次 tr，response 时 free */
} UnrecCtx;

static void unrec_ctx_free(UnrecCtx *ctx) {
    if (!ctx) return;
    if (ctx->tr) tokenize_result_free(ctx->tr);
    if (ctx->pieces) {
        for (guint i = 0; i < ctx->pieces->len; i++)
            g_free(g_array_index(ctx->pieces, char *, i));
        g_array_free(ctx->pieces, TRUE);
    }
    if (ctx->rows) g_array_free(ctx->rows, TRUE);
    g_free(ctx->text);
    g_free(ctx);
}

static void on_unrec_dialog_response(AdwMessageDialog *dlg, const char *response,
                                     gpointer user_data) {
    UnrecCtx *ctx = (UnrecCtx *)user_data;
    TWin *w = ctx->w;

    if (g_strcmp0(response, "cancel") == 0) {
        /* 放弃本次构建：保留输入文本中的红色错误高亮以便定位 */
        unrec_ctx_free(ctx);
        gtk_window_destroy(GTK_WINDOW(dlg));
        return;
    }

    if (g_strcmp0(response, "confirm") == 0) {
        /* 把勾选行的（可能编辑过的）文本入典 */
        for (guint i = 0; i < ctx->rows->len; i++) {
            UnrecRow *ur = &g_array_index(ctx->rows, UnrecRow, i);
            if (gtk_check_button_get_active(GTK_CHECK_BUTTON(ur->check))) {
                const char *t = gtk_editable_get_text(GTK_EDITABLE(ur->entry));
                if (t && *t) lexicon_add(w->lex, t);
            }
        }
        rebuild_lex_list(w);
    }
    /* "skip" 分支：不入典，直接按当前词典再切一次（未识别仍会留 errors） */

    const char *sep = gtk_editable_get_text(GTK_EDITABLE(w->sep_entry));
    if (!sep) sep = "";
    clear_input_error_tags(w);
    TokenizeResult *tr2 = tokenize(w->lex, ctx->text, sep,
                                   TOKENIZE_BY_LEXICON, FALSE);
    finalize_with_tr(w, tr2, ctx->is_append);
    tokenize_result_free(tr2);

    unrec_ctx_free(ctx);
    gtk_window_destroy(GTK_WINDOW(dlg));
}

static void show_unrec_dialog(TWin *w, char *text_owned,
                              TokenizeResult *tr, gboolean is_append) {
    /* 去重 errors 中的 piece */
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GArray *pieces = g_array_new(FALSE, FALSE, sizeof(char *));
    for (guint i = 0; i < tr->errors->len; i++) {
        const TokenizeError *e = &g_array_index(tr->errors, TokenizeError, i);
        if (!e->piece || !*e->piece) continue;
        if (g_hash_table_contains(seen, e->piece)) continue;
        char *dup = g_strdup(e->piece);
        g_array_append_val(pieces, dup);
        g_hash_table_add(seen, dup);
    }
    g_hash_table_destroy(seen);

    /* 同步把错误高亮先标上，便于用户确认时回看输入位置 */
    for (guint i = 0; i < tr->errors->len; i++) {
        const TokenizeError *e = &g_array_index(tr->errors, TokenizeError, i);
        mark_input_error(w, e->byte_offset, e->byte_len);
    }

    GtkWidget *dlg = adw_message_dialog_new(GTK_WINDOW(w->window),
                                            "发现未登录词", NULL);
    char body[160];
    g_snprintf(body, sizeof body,
               "共 %u 个未登录段（已去重）。可编辑文本，勾选后将作为新条目加入词典。",
               pieces->len);
    adw_message_dialog_set_body(ADW_MESSAGE_DIALOG(dlg), body);
    adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dlg), "cancel",  "取消构建");
    adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dlg), "skip",    "全部跳过");
    adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dlg), "confirm", "加入勾选项");
    adw_message_dialog_set_response_appearance(ADW_MESSAGE_DIALOG(dlg),
        "confirm", ADW_RESPONSE_SUGGESTED);
    adw_message_dialog_set_default_response(ADW_MESSAGE_DIALOG(dlg), "confirm");

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_size_request(scrolled, 360, 240);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GArray *rows = g_array_new(FALSE, FALSE, sizeof(UnrecRow));
    for (guint i = 0; i < pieces->len; i++) {
        const char *piece = g_array_index(pieces, char *, i);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *check = gtk_check_button_new();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
        GtkWidget *entry = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(entry), piece);
        gtk_widget_set_hexpand(entry, TRUE);
        gtk_box_append(GTK_BOX(row), check);
        gtk_box_append(GTK_BOX(row), entry);
        gtk_box_append(GTK_BOX(list_box), row);
        UnrecRow ur = { check, entry };
        g_array_append_val(rows, ur);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list_box);
    adw_message_dialog_set_extra_child(ADW_MESSAGE_DIALOG(dlg), scrolled);

    UnrecCtx *ctx = g_new0(UnrecCtx, 1);
    ctx->w         = w;
    ctx->text      = text_owned;   /* 转移所有权 */
    ctx->is_append = is_append;
    ctx->rows      = rows;
    ctx->pieces    = pieces;
    ctx->tr        = tr;            /* 转移所有权 */

    g_signal_connect(dlg, "response",
                     G_CALLBACK(on_unrec_dialog_response), ctx);
    gtk_window_present(GTK_WINDOW(dlg));
}

/* 公共入口：on_build / on_append 都走这里 */
static void do_build_or_append(TWin *w, gboolean is_append) {
    clear_input_error_tags(w);

    const char *sep = gtk_editable_get_text(GTK_EDITABLE(w->sep_entry));
    if (!sep) sep = "";
    GtkTextIter s, e;
    gtk_text_buffer_get_start_iter(w->input_buffer, &s);
    gtk_text_buffer_get_end_iter  (w->input_buffer, &e);
    char *text = gtk_text_buffer_get_text(w->input_buffer, &s, &e, FALSE);

    guint sel = gtk_drop_down_get_selected(w->mode_dropdown);
    TokenizeMode mode = (sel == 1) ? TOKENIZE_BY_CHAR
                      : (sel == 2) ? TOKENIZE_BY_LEXICON
                                   : TOKENIZE_BY_DELIM;
    gboolean auto_add = gtk_check_button_get_active(w->autoadd_check);

    TokenizeResult *tr = tokenize(w->lex, text, sep, mode, auto_add);
    if (auto_add) rebuild_lex_list(w);

    /* 词典模式 + 不自动入典 + 有未识别段：弹窗让用户编辑/确认 */
    if (mode == TOKENIZE_BY_LEXICON && !auto_add &&
        tr->errors && tr->errors->len > 0) {
        show_unrec_dialog(w, text, tr, is_append);  /* text/tr 转给 ctx */
        return;
    }

    finalize_with_tr(w, tr, is_append);
    g_free(text);
    tokenize_result_free(tr);
}

static void on_build(GtkButton *btn, gpointer user) {
    (void)btn;
    do_build_or_append((TWin *)user, FALSE);
}

/* ─── 追加（GSA 增量）────────────────────────────────────────
 * 把当前输入追加到现有 SAM 上：复用 lex/sam，沿用相同的 tokenize 流程，
 * 但调用 sam_append_string 而非重建。多次追加得到与「全部输入拼接后
 * 一次性构造」结构等价的 GSA。 */
static void on_append(GtkButton *btn, gpointer user) {
    (void)btn;
    do_build_or_append((TWin *)user, TRUE);
}

/* ─── suffix link 开关 ─────────────────────────────────────── */

static gboolean on_suffix_switch_state(GtkSwitch *sw, gboolean state, gpointer user) {
    (void)sw;
    TWin *w = (TWin *)user;
    tokensam_canvas_set_show_suffix_link(w->canvas, state);
    return FALSE;   /* 让默认 handler 同步状态 */
}

/* ─── 构造窗口 ─────────────────────────────────────────────── */

GtkWidget *tokensam_window_new(void) {
    GtkBuilder *b = gtk_builder_new_from_resource(
        "/com/github/notework/tokensam/tokensam_window.ui");

    TWin *w = g_new0(TWin, 1);
    w->lex = lexicon_new();
    w->sam = NULL;

    w->window         = GTK_WIDGET(gtk_builder_get_object(b, "twin"));
    w->lex_entry      = GTK_ENTRY  (gtk_builder_get_object(b, "lex_entry"));
    w->lex_add_btn    = GTK_BUTTON (gtk_builder_get_object(b, "lex_add_btn"));
    w->lex_list       = GTK_LIST_BOX(gtk_builder_get_object(b, "lex_list"));
    w->lex_remove_btn = GTK_BUTTON (gtk_builder_get_object(b, "lex_remove_btn"));
    w->lex_merge_btn  = GTK_BUTTON (gtk_builder_get_object(b, "lex_merge_btn"));
    w->lex_split_btn  = GTK_BUTTON (gtk_builder_get_object(b, "lex_split_btn"));
    w->lex_select_all_btn = GTK_BUTTON(gtk_builder_get_object(b, "lex_select_all_btn"));
    w->lex_clear_sel_btn  = GTK_BUTTON(gtk_builder_get_object(b, "lex_clear_sel_btn"));
    w->sep_entry      = GTK_ENTRY  (gtk_builder_get_object(b, "sep_entry"));
    w->mode_dropdown  = GTK_DROP_DOWN(gtk_builder_get_object(b, "mode_dropdown"));
    w->autoadd_check  = GTK_CHECK_BUTTON(gtk_builder_get_object(b, "autoadd_check"));
    w->input_view     = GTK_TEXT_VIEW(gtk_builder_get_object(b, "input_view"));
    w->build_btn      = GTK_BUTTON (gtk_builder_get_object(b, "build_btn"));
    w->append_btn     = GTK_BUTTON (gtk_builder_get_object(b, "append_btn"));
    w->suffix_switch  = GTK_SWITCH (gtk_builder_get_object(b, "suffix_link_switch"));
    w->status_label   = GTK_LABEL  (gtk_builder_get_object(b, "status_label"));
    w->canvas_holder  = GTK_BOX    (gtk_builder_get_object(b, "canvas_holder"));

    /* 输入文本错误 tag（红色下划线） */
    w->input_buffer = gtk_text_view_get_buffer(w->input_view);
    w->err_tag = gtk_text_buffer_create_tag(w->input_buffer, "tokensam-error",
                                            "underline", PANGO_UNDERLINE_ERROR,
                                            "background", "#ffe6e6",
                                            NULL);

    /* 画布 */
    w->canvas = tokensam_canvas_new();
    gtk_box_append(w->canvas_holder, w->canvas);

    /* 默认填一组示例词典与文本：词典 a/b/c，输入 "a b c b c"
     * 等价类序列 [0,1,2,1,2]，会触发 2 次 SAM clone（节点 5、7），
     * 让 DAG 呈现典型的"双层错落"结构。 */
    lexicon_add(w->lex, "a");
    lexicon_add(w->lex, "b");
    lexicon_add(w->lex, "c");
    rebuild_lex_list(w);
    gtk_text_buffer_set_text(w->input_buffer, "a b c b c", -1);

    /* 信号绑定 */
    g_signal_connect(w->lex_add_btn,    "clicked",  G_CALLBACK(on_add),    w);
    g_signal_connect(w->lex_entry,      "activate", G_CALLBACK(on_lex_entry_activate), w);
    g_signal_connect(w->lex_remove_btn, "clicked",  G_CALLBACK(on_remove), w);
    g_signal_connect(w->lex_merge_btn,  "clicked",  G_CALLBACK(on_merge),  w);
    g_signal_connect(w->lex_split_btn,  "clicked",  G_CALLBACK(on_split),  w);
    g_signal_connect(w->lex_select_all_btn, "clicked", G_CALLBACK(on_select_all), w);
    g_signal_connect(w->lex_clear_sel_btn,  "clicked", G_CALLBACK(on_clear_sel),  w);
    g_signal_connect(w->build_btn,      "clicked",  G_CALLBACK(on_build),  w);
    g_signal_connect(w->append_btn,     "clicked",  G_CALLBACK(on_append), w);
    g_signal_connect(w->suffix_switch,  "state-set",
                     G_CALLBACK(on_suffix_switch_state), w);

    /* 词典区快捷键：Ctrl+A 全选 / Esc 清除 */
    GtkEventController *shortcut_ctl = gtk_shortcut_controller_new();
    gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(shortcut_ctl),
                                      GTK_SHORTCUT_SCOPE_LOCAL);
    GtkShortcut *sc1 = gtk_shortcut_new(
        gtk_shortcut_trigger_parse_string("<Control>a"),
        gtk_callback_action_new(on_lex_shortcut_select_all, w, NULL));
    GtkShortcut *sc2 = gtk_shortcut_new(
        gtk_shortcut_trigger_parse_string("Escape"),
        gtk_callback_action_new(on_lex_shortcut_clear, w, NULL));
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcut_ctl), sc1);
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(shortcut_ctl), sc2);
    gtk_widget_add_controller(GTK_WIDGET(w->lex_list), shortcut_ctl);

    /* 关联生命周期 */
    g_object_set_data_full(G_OBJECT(w->window), "tokensam-twin", w, twin_free);
    g_object_unref(b);
    return w->window;
}
