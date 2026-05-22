/**
 * doodle_model.c — 数据模型 / 编号管理
 *
 * 设计要点：
 *   - ShapeStore 顺序 = 绘制叠放顺序，新增图形 append 到末尾。
 *   - 编号空间是"全局编号"：直线 / 路径 各占 1 个；阵列组顶层不占用，
 *     其 rows*cols 个子项各占 1 个、按行优先连续分配。
 *   - 自动操作（增加/删除/擦除分裂/阵列）始终保持 1..N 紧凑；
 *     用户手动 shape_set_number() 改号才可能造成空缺/重复，由 UI
 *     层显示提示。
 */
#include "doodle.h"
#include <string.h>

/* ─── Shape 构造/释放 ───────────────────────────────────────────── */

Shape *shape_new_line(DPoint a, DPoint b) {
    Shape *s = g_new0(Shape, 1);
    s->kind = SHAPE_LINE;
    s->u.line.a = a;
    s->u.line.b = b;
    return s;
}

Shape *shape_new_path(void) {
    Shape *s = g_new0(Shape, 1);
    s->kind = SHAPE_PATH;
    s->u.path.cap = 16;
    s->u.path.pt  = g_new(DPoint, s->u.path.cap);
    s->u.path.n   = 0;
    return s;
}

void shape_path_add_point(Shape *s, DPoint p) {
    g_assert(s->kind == SHAPE_PATH);
    if (s->u.path.n == s->u.path.cap) {
        s->u.path.cap *= 2;
        s->u.path.pt = g_renew(DPoint, s->u.path.pt, s->u.path.cap);
    }
    s->u.path.pt[s->u.path.n++] = p;
}

Shape *shape_clone(const Shape *s) {
    Shape *c = g_new0(Shape, 1);
    c->kind   = s->kind;
    c->number = s->number;
    c->dx     = s->dx;
    c->dy     = s->dy;
    switch (s->kind) {
    case SHAPE_LINE:
        c->u.line = s->u.line;
        break;
    case SHAPE_PATH:
        c->u.path.n   = s->u.path.n;
        c->u.path.cap = s->u.path.n > 0 ? s->u.path.n : 1;
        c->u.path.pt  = g_new(DPoint, c->u.path.cap);
        if (s->u.path.n > 0)
            memcpy(c->u.path.pt, s->u.path.pt,
                   sizeof(DPoint) * s->u.path.n);
        break;
    case SHAPE_ARRAY: {
        int nb = s->u.arr.n_bases;
        gsize k = (gsize)s->u.arr.rows * (gsize)s->u.arr.cols * (gsize)nb;
        c->u.arr.rows    = s->u.arr.rows;
        c->u.arr.cols    = s->u.arr.cols;
        c->u.arr.gap_x   = s->u.arr.gap_x;
        c->u.arr.gap_y   = s->u.arr.gap_y;
        c->u.arr.n_bases = nb;
        c->u.arr.bases   = g_new(Shape *, nb);
        for (int b = 0; b < nb; b++)
            c->u.arr.bases[b] = shape_clone(s->u.arr.bases[b]);
        c->u.arr.child_numbers = g_new(int, k);
        memcpy(c->u.arr.child_numbers, s->u.arr.child_numbers,
               sizeof(int) * k);
        break;
    }
    }
    return c;
}

void shape_free(Shape *s) {
    if (!s) return;
    switch (s->kind) {
    case SHAPE_LINE:
        break;
    case SHAPE_PATH:
        g_free(s->u.path.pt);
        break;
    case SHAPE_ARRAY:
        for (int b = 0; b < s->u.arr.n_bases; b++)
            shape_free(s->u.arr.bases[b]);
        g_free(s->u.arr.bases);
        g_free(s->u.arr.child_numbers);
        break;
    }
    g_free(s);
}

/* ─── ShapeStore ────────────────────────────────────────────────── */

static void store_init(ShapeStore *st) {
    st->items = NULL; st->n = 0; st->cap = 0;
}

static void store_clear(ShapeStore *st) {
    for (gsize i = 0; i < st->n; i++) shape_free(st->items[i]);
    g_free(st->items);
    store_init(st);
}

static void store_grow(ShapeStore *st) {
    gsize nc = st->cap == 0 ? 8 : st->cap * 2;
    st->items = g_renew(Shape *, st->items, nc);
    st->cap = nc;
}

static void store_append(ShapeStore *st, Shape *s) {
    if (st->n == st->cap) store_grow(st);
    st->items[st->n++] = s;
}

static void store_insert_at(ShapeStore *st, gsize idx, Shape *s) {
    if (st->n == st->cap) store_grow(st);
    if (idx > st->n) idx = st->n;
    memmove(&st->items[idx + 1], &st->items[idx],
            sizeof(Shape *) * (st->n - idx));
    st->items[idx] = s;
    st->n++;
}

static void store_remove_at(ShapeStore *st, gsize idx) {
    if (idx >= st->n) return;
    shape_free(st->items[idx]);
    if (idx + 1 < st->n)
        memmove(&st->items[idx], &st->items[idx + 1],
                sizeof(Shape *) * (st->n - idx - 1));
    st->n--;
}

/* ─── 文档 ──────────────────────────────────────────────────────── */

static void layer_clear(Layer *l) {
    if (l->kind == LAYER_DOODLE) store_clear(&l->store);
    g_free(l->name);
}

DoodleDoc *doodle_doc_new(void) {
    DoodleDoc *d = g_new0(DoodleDoc, 1);
    d->layers = g_array_new(FALSE, FALSE, sizeof(Layer));
    Layer dl = { 0 };
    dl.kind = LAYER_DOODLE;
    dl.visible = TRUE;
    dl.name = g_strdup("涂鸦层");
    store_init(&dl.store);
    g_array_append_val(d->layers, dl);
    d->active_layer = 0;
    return d;
}

void doodle_doc_free(DoodleDoc *doc) {
    if (!doc) return;
    for (guint i = 0; i < doc->layers->len; i++)
        layer_clear(&g_array_index(doc->layers, Layer, i));
    g_array_free(doc->layers, TRUE);
    g_free(doc);
}

Layer *doc_active_layer(DoodleDoc *doc) {
    return &g_array_index(doc->layers, Layer, doc->active_layer);
}

ShapeStore *doc_active_store(DoodleDoc *doc) {
    Layer *l = doc_active_layer(doc);
    g_return_val_if_fail(l->kind == LAYER_DOODLE, NULL);
    return &l->store;
}

int doc_layer_count(const DoodleDoc *doc) {
    return (int)doc->layers->len;
}

/* ─── 编号管理 ──────────────────────────────────────────────────── */

static int store_max_number(const ShapeStore *st) {
    int m = 0;
    for (gsize i = 0; i < st->n; i++) {
        Shape *s = st->items[i];
        if (s->kind == SHAPE_ARRAY) {
            gsize k = (gsize)s->u.arr.rows * (gsize)s->u.arr.cols *
                      (gsize)s->u.arr.n_bases;
            for (gsize j = 0; j < k; j++)
                if (s->u.arr.child_numbers[j] > m)
                    m = s->u.arr.child_numbers[j];
        } else if (s->number > m) {
            m = s->number;
        }
    }
    return m;
}

int doc_max_number(const DoodleDoc *doc) {
    return store_max_number(doc_active_store((DoodleDoc *)doc));
}

/* 把所有 number >= threshold 的整数加 delta（delta 可负） */
static void store_shift_numbers_ge(ShapeStore *st, int threshold, int delta) {
    if (delta == 0) return;
    for (gsize i = 0; i < st->n; i++) {
        Shape *s = st->items[i];
        if (s->kind == SHAPE_ARRAY) {
            gsize k = (gsize)s->u.arr.rows * (gsize)s->u.arr.cols *
                      (gsize)s->u.arr.n_bases;
            for (gsize j = 0; j < k; j++)
                if (s->u.arr.child_numbers[j] >= threshold)
                    s->u.arr.child_numbers[j] += delta;
        } else if (s->number >= threshold) {
            s->number += delta;
        }
    }
}

void doc_add_shape(DoodleDoc *doc, Shape *s) {
    ShapeStore *st = doc_active_store(doc);
    s->number = store_max_number(st) + 1;
    store_append(st, s);
}

void doc_remove_shape_at(DoodleDoc *doc, gsize idx) {
    ShapeStore *st = doc_active_store(doc);
    if (idx >= st->n) return;
    Shape *s = st->items[idx];
    if (s->kind == SHAPE_ARRAY) {
        /* 子项编号是连续段 [lo..hi]（apply_array 时这样分配）。
         * 即便用户后来手动改过其中某个，仍按 (max-min+1) 视为 hi 之后的整段补位。 */
        gsize k = (gsize)s->u.arr.rows * (gsize)s->u.arr.cols *
                  (gsize)s->u.arr.n_bases;
        int lo = G_MAXINT, hi = 0;
        for (gsize j = 0; j < k; j++) {
            int v = s->u.arr.child_numbers[j];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        store_remove_at(st, idx);
        if (hi >= lo) store_shift_numbers_ge(st, hi + 1, -(int)k);
    } else {
        int n = s->number;
        store_remove_at(st, idx);
        store_shift_numbers_ge(st, n + 1, -1);
    }
}

void doc_replace_shape_with_shapes(DoodleDoc *doc, gsize idx,
                                   Shape **new_shapes, gsize n_new) {
    ShapeStore *st = doc_active_store(doc);
    g_return_if_fail(idx < st->n);
    Shape *orig = st->items[idx];

    if (n_new == 0) {
        doc_remove_shape_at(doc, idx);
        return;
    }

    /* 不能用于阵列（阵列子项编号是连续段，语义不同） */
    g_return_if_fail(orig->kind != SHAPE_ARRAY);

    int    orig_num = orig->number;
    double odx      = orig->dx;
    double ody      = orig->dy;

    /* 后续编号顺移 (n_new - 1) */
    int delta = (int)n_new - 1;
    if (delta > 0)
        store_shift_numbers_ge(st, orig_num + 1, delta);

    /* 第一条复用原编号；其余沿用相同 dx/dy 保证视觉位置不变 */
    new_shapes[0]->number = orig_num;
    new_shapes[0]->dx     = odx;
    new_shapes[0]->dy     = ody;
    shape_free(orig);
    st->items[idx] = new_shapes[0];

    for (gsize i = 1; i < n_new; i++) {
        new_shapes[i]->number = orig_num + (int)i;
        new_shapes[i]->dx     = odx;
        new_shapes[i]->dy     = ody;
        store_insert_at(st, idx + i, new_shapes[i]);
    }
}

void doc_explode_array_at(DoodleDoc *doc, gsize idx) {
    ShapeStore *st = doc_active_store(doc);
    g_return_if_fail(idx < st->n);
    Shape *grp = st->items[idx];
    g_return_if_fail(grp->kind == SHAPE_ARRAY);

    int rows = grp->u.arr.rows, cols = grp->u.arr.cols;
    int nb   = grp->u.arr.n_bases;
    double gdx = grp->dx, gdy = grp->dy;
    double gx  = grp->u.arr.gap_x, gy = grp->u.arr.gap_y;

    GPtrArray *out = g_ptr_array_new();
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            for (int b = 0; b < nb; b++) {
                Shape *bb = grp->u.arr.bases[b];
                Shape *clone = shape_clone(bb);
                /* 副本在画布的绝对偏移 → dx/dy */
                clone->dx = gdx + c * gx + bb->dx;
                clone->dy = gdy + r * gy + bb->dy;
                int slot = (r * cols + c) * nb + b;
                clone->number = grp->u.arr.child_numbers[slot];
                g_ptr_array_add(out, clone);
            }

    /* 总编号不变（k 个 child_number → k 个独立 number），无需 shift */
    shape_free(grp);
    g_assert(out->len > 0);
    st->items[idx] = (Shape *)out->pdata[0];
    for (guint i = 1; i < out->len; i++)
        store_insert_at(st, idx + i, (Shape *)out->pdata[i]);
    g_ptr_array_free(out, TRUE);
}

/* 取得 shape 的画布锚点（不含上层 group 偏移），用于多基阵列排序 */
static void shape_canvas_anchor(const Shape *s, double *ax, double *ay) {
    double x = 0, y = 0;
    switch (s->kind) {
    case SHAPE_LINE:  x = s->u.line.a.x; y = s->u.line.a.y; break;
    case SHAPE_PATH:
        if (s->u.path.n) { x = s->u.path.pt[0].x; y = s->u.path.pt[0].y; }
        break;
    case SHAPE_ARRAY: break;
    }
    *ax = x + s->dx; *ay = y + s->dy;
}

void doc_apply_array(DoodleDoc *doc,
                     const int *base_indices, int n_idx,
                     int rows, int cols, double gap_x, double gap_y,
                     double pre_dx, double pre_dy) {
    g_return_if_fail(rows > 0 && cols > 0);
    g_return_if_fail(n_idx > 0 && base_indices);
    ShapeStore *st = doc_active_store(doc);

    /* 校验：所有 idx 合法且非 ARRAY */
    for (int i = 0; i < n_idx; i++) {
        int idx = base_indices[i];
        if (idx < 0 || (gsize)idx >= st->n) return;
        if (st->items[idx]->kind == SHAPE_ARRAY) return;
    }

    /* 收集 base 指针 + 锚点，按 (y,x) 升序排序 */
    typedef struct { Shape *src; int idx; double ax, ay; } Entry;
    Entry *ent = g_new(Entry, n_idx);
    for (int i = 0; i < n_idx; i++) {
        ent[i].src = st->items[base_indices[i]];
        ent[i].idx = base_indices[i];
        shape_canvas_anchor(ent[i].src, &ent[i].ax, &ent[i].ay);
    }
    for (int i = 0; i < n_idx; i++)
        for (int j = i + 1; j < n_idx; j++)
            if (ent[i].ay > ent[j].ay ||
                (ent[i].ay == ent[j].ay && ent[i].ax > ent[j].ax)) {
                Entry t = ent[i]; ent[i] = ent[j]; ent[j] = t;
            }

    /* 克隆为 bases，保留 dx/dy 以维持原相对位置；副本(0,0)就出现在原位置 */
    Shape **bases = g_new(Shape *, n_idx);
    for (int i = 0; i < n_idx; i++) {
        Shape *c = shape_clone(ent[i].src);
        c->number = 0;
        bases[i] = c;
    }
    g_free(ent);

    /* 删除原 base 们：按 idx 降序，避免索引漂移；编号会自动收缩 */
    int *idx_desc = g_new(int, n_idx);
    memcpy(idx_desc, base_indices, sizeof(int) * n_idx);
    for (int i = 0; i < n_idx; i++)
        for (int j = i + 1; j < n_idx; j++)
            if (idx_desc[i] < idx_desc[j]) {
                int t = idx_desc[i]; idx_desc[i] = idx_desc[j]; idx_desc[j] = t;
            }
    for (int i = 0; i < n_idx; i++)
        doc_remove_shape_at(doc, (gsize)idx_desc[i]);
    g_free(idx_desc);

    /* 创建组并追加到 store 末尾 */
    Shape *grp = g_new0(Shape, 1);
    grp->kind   = SHAPE_ARRAY;
    grp->number = 0;
    grp->dx = pre_dx;
    grp->dy = pre_dy;
    grp->u.arr.rows    = rows;
    grp->u.arr.cols    = cols;
    grp->u.arr.gap_x   = gap_x;
    grp->u.arr.gap_y   = gap_y;
    grp->u.arr.n_bases = n_idx;
    grp->u.arr.bases   = bases;

    gsize k = (gsize)rows * (gsize)cols * (gsize)n_idx;
    grp->u.arr.child_numbers = g_new(int, k);
    int first = store_max_number(st) + 1;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            for (int b = 0; b < n_idx; b++) {
                gsize slot = ((gsize)r * cols + c) * n_idx + b;
                grp->u.arr.child_numbers[slot] = first + (int)slot;
            }
    store_append(st, grp);
}

void doc_compact_numbers(DoodleDoc *doc) {
    ShapeStore *st = doc_active_store(doc);
    int next = 1;
    for (gsize i = 0; i < st->n; i++) {
        Shape *s = st->items[i];
        if (s->kind == SHAPE_ARRAY) {
            gsize k = (gsize)s->u.arr.rows * (gsize)s->u.arr.cols *
                      (gsize)s->u.arr.n_bases;
            for (gsize j = 0; j < k; j++)
                s->u.arr.child_numbers[j] = next++;
        } else {
            s->number = next++;
        }
    }
}

void doc_insert_image_stub_below_active(DoodleDoc *doc) {
    /* 在当前 doodle 层"下方"插入：即在 active_layer 下标处插入，
     * 原 active 上移一格，active_layer 自增以保持指向同一个 doodle 层。 */
    Layer img = { 0 };
    img.kind    = LAYER_IMAGE_STUB;
    img.visible = TRUE;
    img.name    = g_strdup_printf("图片图层 %d", doc_layer_count(doc));
    g_array_insert_val(doc->layers, (guint)doc->active_layer, img);
    doc->active_layer++;
}

gboolean shape_set_number(Shape *s, int new_n) {
    if (new_n < 1) return FALSE;
    s->number = new_n;
    return TRUE;
}

GArray *doc_missing_numbers(const DoodleDoc *doc) {
    GArray *out = g_array_new(FALSE, FALSE, sizeof(int));
    int hi = doc_max_number(doc);
    if (hi <= 0) return out;

    GHashTable *used = g_hash_table_new(g_direct_hash, g_direct_equal);
    const ShapeStore *st = doc_active_store((DoodleDoc *)doc);
    for (gsize i = 0; i < st->n; i++) {
        Shape *s = st->items[i];
        if (s->kind == SHAPE_ARRAY) {
            gsize k = (gsize)s->u.arr.rows * (gsize)s->u.arr.cols *
                      (gsize)s->u.arr.n_bases;
            for (gsize j = 0; j < k; j++)
                g_hash_table_add(used,
                    GINT_TO_POINTER(s->u.arr.child_numbers[j]));
        } else {
            g_hash_table_add(used, GINT_TO_POINTER(s->number));
        }
    }
    for (int n = 1; n <= hi; n++)
        if (!g_hash_table_contains(used, GINT_TO_POINTER(n)))
            g_array_append_val(out, n);
    g_hash_table_destroy(used);
    return out;
}

gboolean doc_has_duplicate_numbers(const DoodleDoc *doc) {
    GHashTable *used = g_hash_table_new(g_direct_hash, g_direct_equal);
    gboolean dup = FALSE;
    const ShapeStore *st = doc_active_store((DoodleDoc *)doc);
    for (gsize i = 0; i < st->n && !dup; i++) {
        Shape *s = st->items[i];
        if (s->kind == SHAPE_ARRAY) {
            gsize k = (gsize)s->u.arr.rows * (gsize)s->u.arr.cols *
                      (gsize)s->u.arr.n_bases;
            for (gsize j = 0; j < k && !dup; j++) {
                gpointer key = GINT_TO_POINTER(s->u.arr.child_numbers[j]);
                if (g_hash_table_contains(used, key)) dup = TRUE;
                else g_hash_table_add(used, key);
            }
        } else {
            gpointer key = GINT_TO_POINTER(s->number);
            if (g_hash_table_contains(used, key)) dup = TRUE;
            else g_hash_table_add(used, key);
        }
    }
    g_hash_table_destroy(used);
    return dup;
}
