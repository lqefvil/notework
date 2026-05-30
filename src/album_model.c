/**
 * album_model.c — 相册数据模型
 *
 * 仅维护"页集合"，单页本身用 DoodleDoc。所有 doc/surface 由本模块拥有。
 */
#include "album.h"
#include <glib.h>

/* 继承链路深度软上限：超过则拒绝继续添加边或停止深入展开。 */
#define INHERIT_DEPTH_LIMIT 8

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
    a->latex  = NULL; /* 按需构造；latex_doc_new 在 latex_model.c 中 */
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
    /* latex 可能为 NULL；释放函数在 latex_model.c 中。
     * 使用弱声明 + dlsym 等价：直接调用 latex_doc_free 如未链接（独立
     * notework-album/-doodle/-snip/-tokensam 可执行中未含 LaTeX），需在该
     * 可执行中保证 a->latex == NULL。 */
    if (a->latex) {
        extern void latex_doc_free(struct LatexDoc *);
        latex_doc_free(a->latex);
        a->latex = NULL;
    }
    g_free(a);
}

/* LaTeX 文档按需创建。仅在主 notework 可执行中可用；在独立调试
 * 可执行（notework-album/-doodle）中未链接 latex_model.c，如调用将链
 * 接失败——该函数仅被主 notework 的 latex_view_new 路径调用。 */
struct LatexDoc *album_get_latex_doc(Album *a) {
    if (!a) return NULL;
    if (!a->latex) {
        extern struct LatexDoc *latex_doc_new(void);
        a->latex = latex_doc_new();
    }
    return a->latex;
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
    /* 单 pair 约束：每轨道仅允许一个激活区域。超出拒绝。 */
    if (t->pairs->len >= 1) return -1;
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

/* ─── 激活区域 ↔ 统一绑定（四类） ─────────────────────────────── */

static HandlePair *album_pair_at(Album *a, int track_idx, int pair_idx) {
    Track *t = album_track_at(a, track_idx);
    if (!t || !t->pairs) return NULL;
    if (pair_idx < 0 || (guint)pair_idx >= t->pairs->len) return NULL;
    return &g_array_index(t->pairs, HandlePair, (guint)pair_idx);
}

static gboolean binding_equal(const PairBinding *x, const PairBinding *y) {
    if (!x || !y) return FALSE;
    if (x->kind != y->kind) return FALSE;
    switch (x->kind) {
        case BIND_HL_ENV:
        case BIND_HL_VAR:
            return x->payload.hl_id == y->payload.hl_id;
        case BIND_LATEX_RANGE:
            return x->payload.range.start  == y->payload.range.start &&
                   x->payload.range.length == y->payload.range.length;
        case BIND_INHERIT_TRACK:
            return x->payload.inherit.track_idx == y->payload.inherit.track_idx;
        case BIND_INHERIT_MASK: {
            if (x->payload.mask.target_kind != y->payload.mask.target_kind)
                return FALSE;
            switch (x->payload.mask.target_kind) {
                case BIND_HL_ENV:
                case BIND_HL_VAR:
                    return x->payload.mask.target.hl_id ==
                           y->payload.mask.target.hl_id;
                case BIND_LATEX_RANGE:
                    return x->payload.mask.target.range.start  ==
                           y->payload.mask.target.range.start &&
                           x->payload.mask.target.range.length ==
                           y->payload.mask.target.range.length;
                case BIND_INHERIT_TRACK:
                    return x->payload.mask.target.track_idx ==
                           y->payload.mask.target.track_idx;
                default:
                    return FALSE;
            }
        }
    }
    return FALSE;
}

static int find_binding_index(const HandlePair *p, const PairBinding *bnd) {
    if (!p || !p->bindings || !bnd) return -1;
    for (guint i = 0; i < p->bindings->len; i++) {
        const PairBinding *cur = &g_array_index(p->bindings, PairBinding, i);
        if (binding_equal(cur, bnd)) return (int)i;
    }
    return -1;
}

static int find_hl_binding_index(const HandlePair *p, guint64 hl_id) {
    if (!p || !p->bindings) return -1;
    for (guint i = 0; i < p->bindings->len; i++) {
        const PairBinding *cur = &g_array_index(p->bindings, PairBinding, i);
        if ((cur->kind == BIND_HL_ENV || cur->kind == BIND_HL_VAR) &&
            cur->payload.hl_id == hl_id)
            return (int)i;
    }
    return -1;
}

static GArray *ensure_bindings(HandlePair *p) {
    if (!p) return NULL;
    if (!p->bindings)
        p->bindings = g_array_new(FALSE, FALSE, sizeof(PairBinding));
    return p->bindings;
}

gboolean album_pair_bind_hl(Album *a, int track_idx, int pair_idx,
                             guint64 hl_id, BindKind kind) {
    HandlePair *p = album_pair_at(a, track_idx, pair_idx);
    if (!p) return FALSE;
    if (hl_id == 0) return FALSE;
    if (kind != BIND_HL_ENV && kind != BIND_HL_VAR) return FALSE;
    ensure_bindings(p);
    if (find_hl_binding_index(p, hl_id) >= 0) return TRUE; /* 幂等 */
    PairBinding nb = { .kind = kind };
    nb.payload.hl_id = hl_id;
    g_array_append_val(p->bindings, nb);
    return TRUE;
}

gboolean album_pair_bind_latex_range(Album *a, int track_idx, int pair_idx,
                                       int start, int length) {
    HandlePair *p = album_pair_at(a, track_idx, pair_idx);
    if (!p) return FALSE;
    if (start < 0 || length <= 0) return FALSE;
    ensure_bindings(p);
    PairBinding probe = { .kind = BIND_LATEX_RANGE };
    probe.payload.range.start  = start;
    probe.payload.range.length = length;
    if (find_binding_index(p, &probe) >= 0) return TRUE;
    g_array_append_val(p->bindings, probe);
    return TRUE;
}

gboolean album_pair_bind_inherit_track(Album *a, int track_idx, int pair_idx,
                                         int other_track_idx) {
    HandlePair *p = album_pair_at(a, track_idx, pair_idx);
    if (!p) return FALSE;
    if (other_track_idx == track_idx) return FALSE;
    if (!a || !a->tracks) return FALSE;
    if (other_track_idx < 0 ||
        (guint)other_track_idx >= a->tracks->len) return FALSE;
    /* 环检测 + 深度上限（由 album_inherit_can_bind 统一处理） */
    if (!album_inherit_can_bind(a, track_idx, other_track_idx)) return FALSE;
    ensure_bindings(p);
    PairBinding probe = { .kind = BIND_INHERIT_TRACK };
    probe.payload.inherit.track_idx = other_track_idx;
    if (find_binding_index(p, &probe) >= 0) return TRUE;
    g_array_append_val(p->bindings, probe);
    return TRUE;
}

gboolean album_pair_set_hl_kind(Album *a, int track_idx, int pair_idx,
                                  guint64 hl_id, BindKind new_kind) {
    HandlePair *p = album_pair_at(a, track_idx, pair_idx);
    if (!p || !p->bindings) return FALSE;
    if (new_kind != BIND_HL_ENV && new_kind != BIND_HL_VAR) return FALSE;
    int idx = find_hl_binding_index(p, hl_id);
    if (idx < 0) return FALSE;
    PairBinding *cur = &g_array_index(p->bindings, PairBinding, (guint)idx);
    cur->kind = new_kind;
    return TRUE;
}

gboolean album_pair_unbind(Album *a, int track_idx, int pair_idx,
                            const PairBinding *bnd) {
    HandlePair *p = album_pair_at(a, track_idx, pair_idx);
    if (!p || !p->bindings || !bnd) return FALSE;
    int idx = find_binding_index(p, bnd);
    if (idx < 0) return FALSE;
    g_array_remove_index(p->bindings, (guint)idx);
    return TRUE;
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

gboolean album_pair_has_hl_binding(Album *a, int track_idx, int pair_idx,
                                     guint64 hl_id) {
    HandlePair *p = album_pair_at(a, track_idx, pair_idx);
    return find_hl_binding_index(p, hl_id) >= 0;
}

/* ─── 继承：环检测 + mask + 终点夹钳级联 ─────────────────────── */

/* DFS 自 cur 起沿 INHERIT_TRACK 边前向遍历；若途中遇 target 则记 cycle=TRUE。
 * 同时记录最大深度。visited 用整数 → 整数集合（GINT_TO_POINTER）。 */
static gboolean dfs_inherit_forward(Album *a, int cur, int target,
                                     int depth, GHashTable *visited,
                                     int *out_max_depth) {
    if (depth > *out_max_depth) *out_max_depth = depth;
    if (cur == target) return TRUE;
    if (depth >= INHERIT_DEPTH_LIMIT) return FALSE;
    if (g_hash_table_contains(visited, GINT_TO_POINTER(cur))) return FALSE;
    g_hash_table_add(visited, GINT_TO_POINTER(cur));
    HandlePair *p = album_pair_at(a, cur, 0);
    if (!p || !p->bindings) return FALSE;
    for (guint i = 0; i < p->bindings->len; i++) {
        const PairBinding *b = &g_array_index(p->bindings, PairBinding, i);
        if (b->kind != BIND_INHERIT_TRACK) continue;
        if (dfs_inherit_forward(a, b->payload.inherit.track_idx,
                                 target, depth + 1, visited, out_max_depth))
            return TRUE;
    }
    return FALSE;
}

gboolean album_inherit_can_bind(Album *a, int self_track_idx,
                                 int candidate_track_idx) {
    if (!a || !a->tracks) return FALSE;
    if (self_track_idx == candidate_track_idx) return FALSE;
    if (self_track_idx < 0 || (guint)self_track_idx >= a->tracks->len) return FALSE;
    if (candidate_track_idx < 0 ||
        (guint)candidate_track_idx >= a->tracks->len) return FALSE;
    /* 自 candidate 前向 DFS：若能到达 self → 成环；同时记录最大深度。 */
    GHashTable *visited = g_hash_table_new(g_direct_hash, g_direct_equal);
    int max_depth = 0;
    gboolean cycle = dfs_inherit_forward(a, candidate_track_idx,
                                          self_track_idx, 0, visited,
                                          &max_depth);
    g_hash_table_destroy(visited);
    if (cycle) return FALSE;
    /* 添加新边后，自 self 出发的最长链 = 1 + max_depth；不超过 8。 */
    if (1 + max_depth > INHERIT_DEPTH_LIMIT) return FALSE;
    return TRUE;
}

/* ─── INHERIT_MASK 辅助：probe 构造 ──────────────────── */

/* 内部：根据 target (kind+payload) 构造 INHERIT_MASK probe；不支持 target_kind=INHERIT_MASK。 */
static gboolean build_mask_probe(const PairBinding *target, PairBinding *out) {
    if (!target || !out) return FALSE;
    if (target->kind == BIND_INHERIT_MASK) return FALSE;
    out->kind = BIND_INHERIT_MASK;
    out->payload.mask.target_kind = target->kind;
    switch (target->kind) {
        case BIND_HL_ENV:
        case BIND_HL_VAR:
            out->payload.mask.target.hl_id = target->payload.hl_id;
            return TRUE;
        case BIND_LATEX_RANGE:
            out->payload.mask.target.range.start  = target->payload.range.start;
            out->payload.mask.target.range.length = target->payload.range.length;
            return TRUE;
        case BIND_INHERIT_TRACK:
            out->payload.mask.target.track_idx = target->payload.inherit.track_idx;
            return TRUE;
        default:
            return FALSE;
    }
}

gboolean album_pair_set_inherit_mask(Album *a, int track_idx, int pair_idx,
                                      const PairBinding *target,
                                      gboolean masked) {
    HandlePair *p = album_pair_at(a, track_idx, pair_idx);
    if (!p) return FALSE;
    PairBinding probe;
    if (!build_mask_probe(target, &probe)) return FALSE;
    ensure_bindings(p);
    int idx = find_binding_index(p, &probe);
    if (masked) {
        if (idx >= 0) return TRUE;                /* 幂等 */
        g_array_append_val(p->bindings, probe);
        return TRUE;
    } else {
        if (idx < 0) return TRUE;
        g_array_remove_index(p->bindings, (guint)idx);
        return TRUE;
    }
}

gboolean album_pair_is_inherit_masked(Album *a, int track_idx, int pair_idx,
                                       const PairBinding *target) {
    HandlePair *p = album_pair_at(a, track_idx, pair_idx);
    if (!p || !p->bindings) return FALSE;
    PairBinding probe;
    if (!build_mask_probe(target, &probe)) return FALSE;
    return find_binding_index(p, &probe) >= 0;
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
