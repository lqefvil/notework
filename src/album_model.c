/**
 * album_model.c — 相册数据模型
 *
 * 仅维护"页集合"，单页本身用 DoodleDoc。所有 doc/surface 由本模块拥有。
 */
#include "album.h"

static void album_page_clear(AlbumPage *p) {
    if (!p) return;
    if (p->doc) { doodle_doc_free(p->doc); p->doc = NULL; }
    g_free(p->src_uri); p->src_uri = NULL;
    g_free(p->title);   p->title   = NULL;
}

Album *album_new(void) {
    Album *a = g_new0(Album, 1);
    a->pages  = g_array_new(FALSE, FALSE, sizeof(AlbumPage));
    a->active = -1;
    return a;
}

void album_free(Album *a) {
    if (!a) return;
    for (guint i = 0; i < a->pages->len; i++)
        album_page_clear(&g_array_index(a->pages, AlbumPage, i));
    g_array_free(a->pages, TRUE);
    g_free(a);
}

int album_page_count(const Album *a) {
    return a ? (int)a->pages->len : 0;
}

AlbumPage *album_get_page(Album *a, int idx) {
    if (!a || idx < 0 || idx >= (int)a->pages->len) return NULL;
    return &g_array_index(a->pages, AlbumPage, idx);
}

AlbumPage *album_active_page(Album *a) {
    return album_get_page(a, a ? a->active : -1);
}

int album_append_page(Album *a, DoodleDoc *doc,
                      const char *title, const char *src_uri) {
    if (!a) return -1;
    AlbumPage p = { 0 };
    p.doc     = doc ? doc : doodle_doc_new();
    p.title   = g_strdup(title ? title : "未命名");
    p.src_uri = src_uri ? g_strdup(src_uri) : NULL;
    p.applied = FALSE;
    g_array_append_val(a->pages, p);
    if (a->active < 0) a->active = 0;
    return (int)a->pages->len - 1;
}

int album_append_page_from_surface(Album *a, cairo_surface_t *surface,
                                    const char *title,
                                    const char *src_uri) {
    /* 默认仅创建一个图片层。涂鸦层仅在用户进入 doodle 窗口
     * 并实际画了东西后才生成（“应用”语义）。 */
    DoodleDoc *doc = doodle_doc_new_empty();
    Layer img = layer_new_image_value(surface,
        title ? title : "图片");
    g_array_append_val(doc->layers, img);
    doc->active_layer = 0;  /* 指向唯一的图片层 */
    return album_append_page(a, doc, title, src_uri);
}

void album_remove_page(Album *a, int idx) {
    if (!a || idx < 0 || idx >= (int)a->pages->len) return;
    album_page_clear(&g_array_index(a->pages, AlbumPage, idx));
    g_array_remove_index(a->pages, (guint)idx);
    int n = (int)a->pages->len;
    if (n == 0)              a->active = -1;
    else if (a->active >= n) a->active = n - 1;
    else if (a->active > idx) a->active--;
}

void album_move_page(Album *a, int from, int to) {
    if (!a) return;
    int n = (int)a->pages->len;
    if (from < 0 || from >= n) return;
    if (to   < 0) to = 0;
    if (to   >= n) to = n - 1;
    if (from == to) return;
    AlbumPage tmp = g_array_index(a->pages, AlbumPage, from);
    g_array_remove_index(a->pages, (guint)from);
    g_array_insert_val (a->pages, (guint)to, tmp);
    if (a->active == from) a->active = to;
    else if (from < a->active && a->active <= to) a->active--;
    else if (to <= a->active && a->active < from) a->active++;
}

void album_set_active(Album *a, int idx) {
    if (!a) return;
    if (idx < 0 || idx >= (int)a->pages->len) return;
    a->active = idx;
}
