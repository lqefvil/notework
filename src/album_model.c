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

static void album_pair_clear(HandlePair *p) {
    if (!p) return;
    if (p->bindings) {
        g_array_free(p->bindings, TRUE);
        p->bindings = NULL;
    }
}

static void album_track_clear(Track *t) {
    if (!t) return;
    g_free(t->name); t->name = NULL;
    if (t->pairs) {
        for (guint i = 0; i < t->pairs->len; i++)
            album_pair_clear(&g_array_index(t->pairs, HandlePair, i));
        g_array_free(t->pairs, TRUE);
        t->pairs = NULL;
    }
}

Album *album_new(void) {
    Album *a = g_new0(Album, 1);
    a->pages  = g_array_new(FALSE, FALSE, sizeof(AlbumPage));
    a->active = -1;
    /* 轨道集合：默认空。轨道由用户在 UI 中显式创建。 */
    a->tracks = g_array_new(FALSE, FALSE, sizeof(Track));
    return a;
}

void album_free(Album *a) {
    if (!a) return;
    for (guint i = 0; i < a->pages->len; i++)
        album_page_clear(&g_array_index(a->pages, AlbumPage, i));
    g_array_free(a->pages, TRUE);
    if (a->tracks) {
        for (guint i = 0; i < a->tracks->len; i++)
            album_track_clear(&g_array_index(a->tracks, Track, i));
        g_array_free(a->tracks, TRUE);
    }
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

/* 本期返回一个静态快照型 GtkStringList：按创建时 album->tracks
 * 的名称填充。后期增加轨道 CRUD 后，需要用自定义 GListModel
 * 实现动态增删与项目修改通知。 */
GListModel *album_create_track_model(Album *a) {
    GtkStringList *m = gtk_string_list_new(NULL);
    if (!a || !a->tracks) return G_LIST_MODEL(m);
    for (guint i = 0; i < a->tracks->len; i++) {
        const Track *t = &g_array_index(a->tracks, Track, i);
        gtk_string_list_append(m, t->name ? t->name : "");
    }
    return G_LIST_MODEL(m);
}

int album_track_append(Album *a, const char *name) {
    g_return_val_if_fail(a && a->tracks, -1);
    Track t = { .name = NULL, .pairs = NULL };
    if (name && *name) {
        t.name = g_strdup(name);
    } else {
        t.name = g_strdup_printf("轨道 %u", a->tracks->len + 1);
    }
    t.pairs = g_array_new(FALSE, FALSE, sizeof(HandlePair));
    g_array_append_val(a->tracks, t);
    return (int)a->tracks->len - 1;
}

gboolean album_track_remove(Album *a, int idx) {
    g_return_val_if_fail(a && a->tracks, FALSE);
    if (idx < 0 || (guint)idx >= a->tracks->len) return FALSE;
    Track *t = &g_array_index(a->tracks, Track, (guint)idx);
    album_track_clear(t);
    g_array_remove_index(a->tracks, (guint)idx);
    return TRUE;
}

/* ─── 轨道把手对 ───────────────────────────────── */

static Track *album_track_at(Album *a, int idx) {
    if (!a || !a->tracks) return NULL;
    if (idx < 0 || (guint)idx >= a->tracks->len) return NULL;
    return &g_array_index(a->tracks, Track, (guint)idx);
}

static inline void normalize_pair(double *a, double *b) {
    if (*a > *b) { double t = *a; *a = *b; *b = t; }
}

int album_track_add_pair(Album *a, int track_idx,
                         double a_arc, double b_arc) {
    Track *t = album_track_at(a, track_idx);
    if (!t) return -1;
    if (!t->pairs) t->pairs = g_array_new(FALSE, FALSE, sizeof(HandlePair));
    normalize_pair(&a_arc, &b_arc);
    HandlePair p = { .a_arc = a_arc, .b_arc = b_arc, .bindings = NULL };
    g_array_append_val(t->pairs, p);
    return (int)t->pairs->len - 1;
}

gboolean album_track_remove_pair(Album *a, int track_idx, int pair_idx) {
    Track *t = album_track_at(a, track_idx);
    if (!t || !t->pairs) return FALSE;
    if (pair_idx < 0 || (guint)pair_idx >= t->pairs->len) return FALSE;
    /* 释放该 pair 的 bindings GArray，避免内存泄漏 */
    album_pair_clear(&g_array_index(t->pairs, HandlePair, (guint)pair_idx));
    g_array_remove_index(t->pairs, (guint)pair_idx);
    return TRUE;
}

gboolean album_track_update_pair(Album *a, int track_idx, int pair_idx,
                                 double a_arc, double b_arc) {
    Track *t = album_track_at(a, track_idx);
    if (!t || !t->pairs) return FALSE;
    if (pair_idx < 0 || (guint)pair_idx >= t->pairs->len) return FALSE;
    normalize_pair(&a_arc, &b_arc);
    HandlePair *p = &g_array_index(t->pairs, HandlePair, (guint)pair_idx);
    p->a_arc = a_arc;
    p->b_arc = b_arc;
    return TRUE;
}

/* ─── 激活区域 ↔ 高亮绑定 ───────────────────────────────── */

static HandlePair *album_pair_at(Album *a, int track_idx, int pair_idx) {
    Track *t = album_track_at(a, track_idx);
    if (!t || !t->pairs) return NULL;
    if (pair_idx < 0 || (guint)pair_idx >= t->pairs->len) return NULL;
    return &g_array_index(t->pairs, HandlePair, (guint)pair_idx);
}

static gboolean pair_bindings_contains(const HandlePair *p, guint64 hl_id) {
    if (!p || !p->bindings) return FALSE;
    for (guint i = 0; i < p->bindings->len; i++)
        if (g_array_index(p->bindings, guint64, i) == hl_id) return TRUE;
    return FALSE;
}

gboolean album_pair_bind_highlight(Album *a, int track_idx, int pair_idx,
                                    guint64 hl_id) {
    HandlePair *p = album_pair_at(a, track_idx, pair_idx);
    if (!p) return FALSE;
    if (hl_id == 0) return FALSE;
    if (!p->bindings) p->bindings = g_array_new(FALSE, FALSE, sizeof(guint64));
    if (pair_bindings_contains(p, hl_id)) return TRUE; /* 幂等 */
    g_array_append_val(p->bindings, hl_id);
    return TRUE;
}

gboolean album_pair_unbind_highlight(Album *a, int track_idx, int pair_idx,
                                      guint64 hl_id) {
    HandlePair *p = album_pair_at(a, track_idx, pair_idx);
    if (!p || !p->bindings) return FALSE;
    for (guint i = 0; i < p->bindings->len; i++) {
        if (g_array_index(p->bindings, guint64, i) == hl_id) {
            g_array_remove_index(p->bindings, i);
            return TRUE;
        }
    }
    return FALSE;
}

gboolean album_pair_clear_bindings(Album *a, int track_idx, int pair_idx) {
    HandlePair *p = album_pair_at(a, track_idx, pair_idx);
    if (!p) return FALSE;
    if (p->bindings && p->bindings->len > 0)
        g_array_set_size(p->bindings, 0);
    return TRUE;
}

GArray *album_pair_get_bindings(Album *a, int track_idx, int pair_idx) {
    HandlePair *p = album_pair_at(a, track_idx, pair_idx);
    return p ? p->bindings : NULL;
}

gboolean album_pair_has_binding(Album *a, int track_idx, int pair_idx,
                                 guint64 hl_id) {
    HandlePair *p = album_pair_at(a, track_idx, pair_idx);
    return pair_bindings_contains(p, hl_id);
}

/* 全局反查：遍历所有页 → 所有图层（仅 LAYER_HIGHLIGHT） → 所有记录。
 * 未命中返回 FALSE；out_* 可传 NULL。 */
gboolean album_find_highlight_by_id(Album *a, guint64 hl_id,
                                     int *out_page, int *out_layer, int *out_rec) {
    if (!a || hl_id == 0) return FALSE;
    int n_pages = album_page_count(a);
    for (int pi = 0; pi < n_pages; pi++) {
        AlbumPage *page = album_get_page(a, pi);
        if (!page || !page->doc || !page->doc->layers) continue;
        int n_layers = (int)page->doc->layers->len;
        for (int li = 0; li < n_layers; li++) {
            Layer *L = &g_array_index(page->doc->layers, Layer, li);
            if (L->kind != LAYER_HIGHLIGHT || !L->highlights) continue;
            for (guint ri = 0; ri < L->highlights->len; ri++) {
                HighlightRecord *r = g_ptr_array_index(L->highlights, ri);
                if (r && r->id == hl_id) {
                    if (out_page)  *out_page  = pi;
                    if (out_layer) *out_layer = li;
                    if (out_rec)   *out_rec   = (int)ri;
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}
