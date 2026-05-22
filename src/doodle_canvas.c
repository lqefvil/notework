/**
 * doodle_canvas.c — 基于 GtkDrawingArea + Cairo 的画布
 *
 * 工具状态机：line / path / erase / select；阵列预览作为独立模式覆盖工具行为。
 *  - 普通图形以 (s.dx, s.dy) 平移；
 *  - 阵列组的每个副本在 (group.dx + col*gap_x + base.dx, group.dy + row*gap_y + base.dy)
 *    处绘制；副本之间空隙不视为命中（按副本几何 hit-test）；
 *  - 选择支持 Ctrl/Shift+点击多选，拖动整体移动所有选中；
 *  - 擦除工具下显示半径指示圆，便于知道擦除头位置。
 */
#include "doodle.h"
#include <math.h>

#define ERASE_RADIUS_DEFAULT 14.0
#define PATH_SAMPLE_THRESH    2.0
#define HIT_THRESH            8.0
#define MIN_LINE_LEN          3.0

typedef struct { int idx; double dx0, dy0; } SelOrig;

typedef struct {
    GtkWidget       *area;
    DoodleDoc       *doc;
    Tool             tool;

    /* 绘制中状态 */
    gboolean drawing_line;
    DPoint   line_start, line_cur;
    Shape   *path_pending;

    double   erase_radius;

    /* 选择 / 拖动 */
    int      selected_idx;     /* 主选中：-1 表示无 */
    GArray  *selected_extra;   /* int：额外选中（不含主） */
    gboolean drag_moving;
    GArray  *sel_origs;        /* SelOrig：进入拖动时记录每个选中图形的起始 dx/dy */

    /* 阵列预览 */
    gboolean array_active;
    GArray  *array_preview_idx;/* int：进入预览时的全部选中（按 store 顺序升序） */
    int      ar_rows, ar_cols;
    double   ar_gap_x, ar_gap_y;
    double   ar_dx, ar_dy;
    double   ar_drag_orig_dx, ar_drag_orig_dy;

    /* 鼠标位置（用于擦除光标） */
    gboolean has_pointer;
    DPoint   pointer_pos;

    /* 变更回调 */
    DoodleChangedFn changed_cb;
    gpointer        changed_data;
} CanvasCtx;

#define CTX_KEY "doodle-canvas-ctx"

static CanvasCtx *ctx_of(GtkWidget *w) {
    return (CanvasCtx *)g_object_get_data(G_OBJECT(w), CTX_KEY);
}

static void ctx_free(gpointer p) {
    CanvasCtx *c = p;
    if (c->path_pending) shape_free(c->path_pending);
    if (c->selected_extra)    g_array_free(c->selected_extra, TRUE);
    if (c->sel_origs)         g_array_free(c->sel_origs, TRUE);
    if (c->array_preview_idx) g_array_free(c->array_preview_idx, TRUE);
    g_free(c);
}

static void notify_changed(CanvasCtx *c) {
    gtk_widget_queue_draw(c->area);
    if (c->changed_cb) c->changed_cb(c->area, c->changed_data);
}

/* ─── 选中集合辅助 ────────────────────────────────────────────── */

static gboolean is_selected(const CanvasCtx *c, int idx) {
    if (c->selected_idx == idx) return TRUE;
    for (guint k = 0; k < c->selected_extra->len; k++)
        if (g_array_index(c->selected_extra, int, k) == idx) return TRUE;
    return FALSE;
}

static void selection_clear(CanvasCtx *c) {
    c->selected_idx = -1;
    g_array_set_size(c->selected_extra, 0);
}

static void selection_toggle(CanvasCtx *c, int idx) {
    if (c->selected_idx == idx) {
        if (c->selected_extra->len > 0) {
            c->selected_idx = g_array_index(c->selected_extra, int, 0);
            g_array_remove_index(c->selected_extra, 0);
        } else {
            c->selected_idx = -1;
        }
        return;
    }
    for (guint k = 0; k < c->selected_extra->len; k++) {
        if (g_array_index(c->selected_extra, int, k) == idx) {
            g_array_remove_index(c->selected_extra, k);
            return;
        }
    }
    if (c->selected_idx < 0) c->selected_idx = idx;
    else                     g_array_append_val(c->selected_extra, idx);
}

/* 收集所有选中索引并按升序排序，返回 GArray<int>，调用方释放 */
static GArray *selection_collect_sorted(const CanvasCtx *c) {
    GArray *out = g_array_new(FALSE, FALSE, sizeof(int));
    if (c->selected_idx >= 0) {
        int v = c->selected_idx;
        g_array_append_val(out, v);
    }
    for (guint k = 0; k < c->selected_extra->len; k++) {
        int v = g_array_index(c->selected_extra, int, k);
        g_array_append_val(out, v);
    }
    int *p = (int *)out->data;
    for (guint i = 0; i < out->len; i++)
        for (guint j = i + 1; j < out->len; j++)
            if (p[i] > p[j]) { int t = p[i]; p[i] = p[j]; p[j] = t; }
    return out;
}

/* ─── 几何辅助 ────────────────────────────────────────────────── */

static double dist_pt(double ax, double ay, double bx, double by) {
    return hypot(ax - bx, ay - by);
}

static double dist_pt_seg(double px, double py,
                          double ax, double ay,
                          double bx, double by) {
    double vx = bx - ax, vy = by - ay;
    double wx = px - ax, wy = py - ay;
    double c1 = vx * wx + vy * wy;
    if (c1 <= 0)   return dist_pt(px, py, ax, ay);
    double c2 = vx * vx + vy * vy;
    if (c2 <= c1)  return dist_pt(px, py, bx, by);
    double t = c1 / c2;
    return dist_pt(px, py, ax + t * vx, ay + t * vy);
}

static void shape_local_bbox(const Shape *s,
                             double *x0, double *y0,
                             double *x1, double *y1) {
    switch (s->kind) {
    case SHAPE_LINE:
        *x0 = MIN(s->u.line.a.x, s->u.line.b.x);
        *y0 = MIN(s->u.line.a.y, s->u.line.b.y);
        *x1 = MAX(s->u.line.a.x, s->u.line.b.x);
        *y1 = MAX(s->u.line.a.y, s->u.line.b.y);
        break;
    case SHAPE_PATH:
        if (s->u.path.n == 0) { *x0=*y0=*x1=*y1=0; break; }
        *x0 = *x1 = s->u.path.pt[0].x;
        *y0 = *y1 = s->u.path.pt[0].y;
        for (gsize i = 1; i < s->u.path.n; i++) {
            DPoint p = s->u.path.pt[i];
            if (p.x < *x0) *x0 = p.x;
            if (p.x > *x1) *x1 = p.x;
            if (p.y < *y0) *y0 = p.y;
            if (p.y > *y1) *y1 = p.y;
        }
        break;
    case SHAPE_ARRAY: {
        if (s->u.arr.n_bases == 0) { *x0=*y0=*x1=*y1=0; break; }
        double bx0 = 1e18, by0 = 1e18, bx1 = -1e18, by1 = -1e18;
        for (int b = 0; b < s->u.arr.n_bases; b++) {
            Shape *bb = s->u.arr.bases[b];
            double a0, c0, a1, c1;
            shape_local_bbox(bb, &a0, &c0, &a1, &c1);
            a0 += bb->dx; c0 += bb->dy;
            a1 += bb->dx; c1 += bb->dy;
            if (a0 < bx0) bx0 = a0;
            if (c0 < by0) by0 = c0;
            if (a1 > bx1) bx1 = a1;
            if (c1 > by1) by1 = c1;
        }
        *x0 = bx0;
        *y0 = by0;
        *x1 = bx1 + (s->u.arr.cols - 1) * s->u.arr.gap_x;
        *y1 = by1 + (s->u.arr.rows - 1) * s->u.arr.gap_y;
        break;
    }
    }
}

/* ─── 渲染 ────────────────────────────────────────────────────── */

static void cairo_path_for_path_shape(cairo_t *cr, const Shape *s,
                                      double ox, double oy) {
    if (s->u.path.n < 2) return;
    cairo_move_to(cr, s->u.path.pt[0].x + ox, s->u.path.pt[0].y + oy);
    for (gsize i = 1; i < s->u.path.n; i++)
        cairo_line_to(cr, s->u.path.pt[i].x + ox, s->u.path.pt[i].y + oy);
}

static void draw_shape_geom(cairo_t *cr, const Shape *s,
                            double ox, double oy) {
    cairo_set_line_width(cr, 2.0);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    switch (s->kind) {
    case SHAPE_LINE:
        cairo_move_to(cr, s->u.line.a.x + ox, s->u.line.a.y + oy);
        cairo_line_to(cr, s->u.line.b.x + ox, s->u.line.b.y + oy);
        cairo_stroke(cr);
        break;
    case SHAPE_PATH:
        cairo_path_for_path_shape(cr, s, ox, oy);
        cairo_stroke(cr);
        break;
    case SHAPE_ARRAY:
        break;
    }
}

static void draw_number_label(cairo_t *cr, int number, double x, double y) {
    char buf[32];
    g_snprintf(buf, sizeof buf, "%d", number);
    PangoLayout *pl = pango_cairo_create_layout(cr);
    PangoFontDescription *fd =
        pango_font_description_from_string("Sans Bold 8");
    pango_layout_set_font_description(pl, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(pl, buf, -1);
    int tw, th;
    pango_layout_get_pixel_size(pl, &tw, &th);

    cairo_save(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
    cairo_rectangle(cr, x - 1, y - 1, tw + 2, th + 2);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.78, 0.13, 0.13);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, pl);
    cairo_restore(cr);
    g_object_unref(pl);
}

static void shape_anchor(const Shape *s, double *ax, double *ay) {
    switch (s->kind) {
    case SHAPE_LINE: *ax = s->u.line.a.x; *ay = s->u.line.a.y; break;
    case SHAPE_PATH:
        if (s->u.path.n) { *ax = s->u.path.pt[0].x; *ay = s->u.path.pt[0].y; }
        else             { *ax = 0; *ay = 0; }
        break;
    case SHAPE_ARRAY:
        if (s->u.arr.n_bases > 0) {
            Shape *bb = s->u.arr.bases[0];
            shape_anchor(bb, ax, ay);
            *ax += bb->dx; *ay += bb->dy;
        } else { *ax = 0; *ay = 0; }
        break;
    }
}

static void draw_selection_box(cairo_t *cr, const Shape *s) {
    double x0, y0, x1, y1;
    shape_local_bbox(s, &x0, &y0, &x1, &y1);
    x0 += s->dx - 4; y0 += s->dy - 4;
    x1 += s->dx + 4; y1 += s->dy + 4;
    double dashes[] = { 4.0, 3.0 };
    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.13, 0.45, 0.85);
    cairo_set_line_width(cr, 1.0);
    cairo_set_dash(cr, dashes, 2, 0);
    cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static gboolean idx_in_array(GArray *a, int v) {
    if (!a) return FALSE;
    for (guint k = 0; k < a->len; k++)
        if (g_array_index(a, int, k) == v) return TRUE;
    return FALSE;
}

static void on_draw(GtkDrawingArea *area, cairo_t *cr,
                    int width, int height, gpointer user_data) {
    (void)area;
    CanvasCtx *c = user_data;

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, width - 1, height - 1);
    cairo_stroke(cr);

    if (!c->doc) return;

    int n_layers = doc_layer_count(c->doc);
    for (int li = 0; li < n_layers; li++) {
        Layer *L = &g_array_index(c->doc->layers, Layer, li);
        if (!L->visible) continue;
        if (L->kind != LAYER_DOODLE) continue;
        ShapeStore *st = &L->store;

        for (gsize i = 0; i < st->n; i++) {
            Shape *s = st->items[i];

            /* 阵列预览期间隐藏所有选中的基准 */
            if (c->array_active &&
                idx_in_array(c->array_preview_idx, (int)i)) continue;

            cairo_set_source_rgb(cr, 0.13, 0.13, 0.13);
            if (s->kind == SHAPE_ARRAY) {
                for (int r = 0; r < s->u.arr.rows; r++) {
                    for (int co = 0; co < s->u.arr.cols; co++) {
                        for (int b = 0; b < s->u.arr.n_bases; b++) {
                            Shape *bb = s->u.arr.bases[b];
                            double ox = s->dx + co * s->u.arr.gap_x + bb->dx;
                            double oy = s->dy + r  * s->u.arr.gap_y + bb->dy;
                            draw_shape_geom(cr, bb, ox, oy);
                            double ax, ay;
                            shape_anchor(bb, &ax, &ay);
                            gsize slot = ((gsize)r * s->u.arr.cols + co)
                                       * s->u.arr.n_bases + b;
                            draw_number_label(cr,
                                s->u.arr.child_numbers[slot],
                                ax + ox + 4, ay + oy - 14);
                        }
                    }
                }
                if (is_selected(c, (int)i)) draw_selection_box(cr, s);
            } else {
                draw_shape_geom(cr, s, s->dx, s->dy);
                double ax, ay;
                shape_anchor(s, &ax, &ay);
                draw_number_label(cr, s->number, ax + s->dx + 4,
                                  ay + s->dy - 14);
                if (is_selected(c, (int)i)) draw_selection_box(cr, s);
            }
        }
    }

    /* 直线橡皮筋 */
    if (c->drawing_line) {
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0.13, 0.45, 0.85, 0.8);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, c->line_start.x, c->line_start.y);
        cairo_line_to(cr, c->line_cur.x, c->line_cur.y);
        cairo_stroke(cr);
        cairo_restore(cr);
    }

    /* 路径未定型 */
    if (c->path_pending && c->path_pending->u.path.n >= 2) {
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0.13, 0.45, 0.85, 0.8);
        cairo_set_line_width(cr, 2.0);
        cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_path_for_path_shape(cr, c->path_pending, 0, 0);
        cairo_stroke(cr);
        cairo_restore(cr);
    }

    /* 阵列预览覆盖：多基所有选中图形按 (r,c) 副本绘制 */
    if (c->array_active && c->array_preview_idx) {
        ShapeStore *st = doc_active_store(c->doc);
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0.20, 0.65, 0.30, 0.55);
        for (int r = 0; r < c->ar_rows; r++) {
            for (int co = 0; co < c->ar_cols; co++) {
                for (guint k = 0; k < c->array_preview_idx->len; k++) {
                    int bi = g_array_index(c->array_preview_idx, int, k);
                    if (bi < 0 || (gsize)bi >= st->n) continue;
                    Shape *base = st->items[bi];
                    if (base->kind == SHAPE_ARRAY) continue;
                    double ox = base->dx + c->ar_dx + co * c->ar_gap_x;
                    double oy = base->dy + c->ar_dy + r  * c->ar_gap_y;
                    draw_shape_geom(cr, base, ox, oy);
                }
            }
        }
        cairo_restore(cr);
    }

    /* 擦除头光标 */
    if (c->tool == TOOL_ERASE && c->has_pointer && !c->array_active) {
        cairo_save(cr);
        double dashes[] = { 3.0, 2.0 };
        cairo_set_source_rgba(cr, 0.85, 0.20, 0.20, 0.9);
        cairo_set_line_width(cr, 1.5);
        cairo_set_dash(cr, dashes, 2, 0);
        cairo_arc(cr, c->pointer_pos.x, c->pointer_pos.y,
                  c->erase_radius, 0, 2 * G_PI);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
        cairo_set_source_rgba(cr, 0.85, 0.20, 0.20, 0.6);
        cairo_arc(cr, c->pointer_pos.x, c->pointer_pos.y,
                  1.5, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_restore(cr);
    }
}

/* ─── 命中测试 ────────────────────────────────────────────────── */

static double dist_to_shape(const Shape *s, double x, double y,
                            double dx, double dy) {
    double sx = x - dx, sy = y - dy;
    switch (s->kind) {
    case SHAPE_LINE:
        return dist_pt_seg(sx, sy,
            s->u.line.a.x, s->u.line.a.y,
            s->u.line.b.x, s->u.line.b.y);
    case SHAPE_PATH: {
        if (s->u.path.n == 0) return 1e18;
        if (s->u.path.n == 1) return dist_pt(sx, sy,
            s->u.path.pt[0].x, s->u.path.pt[0].y);
        double m = 1e18;
        for (gsize i = 1; i < s->u.path.n; i++) {
            double d = dist_pt_seg(sx, sy,
                s->u.path.pt[i-1].x, s->u.path.pt[i-1].y,
                s->u.path.pt[i  ].x, s->u.path.pt[i  ].y);
            if (d < m) m = d;
        }
        return m;
    }
    case SHAPE_ARRAY: {
        /* 按副本几何最短距离命中（不再用整体 bbox，避免空隙误命中） */
        double m = 1e18;
        for (int r = 0; r < s->u.arr.rows; r++) {
            for (int co = 0; co < s->u.arr.cols; co++) {
                for (int b = 0; b < s->u.arr.n_bases; b++) {
                    Shape *bb = s->u.arr.bases[b];
                    double bdx = s->dx + co * s->u.arr.gap_x + bb->dx;
                    double bdy = s->dy + r  * s->u.arr.gap_y + bb->dy;
                    double d = dist_to_shape(bb, x, y, bdx, bdy);
                    if (d < m) m = d;
                }
            }
        }
        return m;
    }
    }
    return 1e18;
}

static int hit_test(CanvasCtx *c, double x, double y, double thresh) {
    ShapeStore *st = doc_active_store(c->doc);
    for (gssize i = (gssize)st->n - 1; i >= 0; i--) {
        Shape *s = st->items[i];
        double d = (s->kind == SHAPE_ARRAY)
                 ? dist_to_shape(s, x, y, 0, 0)
                 : dist_to_shape(s, x, y, s->dx, s->dy);
        if (d <= thresh) return (int)i;
    }
    return -1;
}

/* ─── 擦除 ────────────────────────────────────────────────────── */

/* 圆-线段切分：返回 TRUE 表示线段被擦除头覆盖（可能保留 0/1/2 段）。
 * out_segs 仅追加保留段（局部坐标，未设 dx/dy）。 */
static gboolean erase_line_split(CanvasCtx *c, const Shape *s,
                                 double x, double y,
                                 GPtrArray *out_segs) {
    double ax = s->u.line.a.x + s->dx, ay = s->u.line.a.y + s->dy;
    double bx = s->u.line.b.x + s->dx, by = s->u.line.b.y + s->dy;
    double ex = bx - ax, ey = by - ay;
    double dxr = ax - x, dyr = ay - y;
    double E2 = ex * ex + ey * ey;
    if (E2 == 0) return FALSE;
    double DE = dxr * ex + dyr * ey;
    double D2 = dxr * dxr + dyr * dyr;
    double r  = c->erase_radius;
    double disc = DE * DE - E2 * (D2 - r * r);
    if (disc < 0) return FALSE;
    double sq  = sqrt(disc);
    double t1  = (-DE - sq) / E2;
    double t2  = (-DE + sq) / E2;
    double tlo = MAX(0.0, t1);
    double thi = MIN(1.0, t2);
    if (tlo >= thi) return FALSE;

    double total = sqrt(E2);
    double lax = s->u.line.a.x, lay = s->u.line.a.y;
    double lbx = s->u.line.b.x, lby = s->u.line.b.y;
    if (tlo > 0 && total * tlo >= MIN_LINE_LEN) {
        DPoint A = { lax, lay };
        DPoint B = { lax + tlo * (lbx - lax), lay + tlo * (lby - lay) };
        g_ptr_array_add(out_segs, shape_new_line(A, B));
    }
    if (thi < 1 && total * (1 - thi) >= MIN_LINE_LEN) {
        DPoint A = { lax + thi * (lbx - lax), lay + thi * (lby - lay) };
        DPoint B = { lbx, lby };
        g_ptr_array_add(out_segs, shape_new_line(A, B));
    }
    return TRUE;
}

static void erase_at(CanvasCtx *c, double x, double y) {
    ShapeStore *st = doc_active_store(c->doc);
    gboolean any = FALSE;

    /* 1) 把所有被擦除头碰到的阵列展开为独立副本。
     *    展开会改变 store 索引，每展开一次就 break 重新扫描。 */
    gboolean exploded;
    do {
        exploded = FALSE;
        for (gssize i = (gssize)st->n - 1; i >= 0; i--) {
            Shape *s = st->items[i];
            if (s->kind != SHAPE_ARRAY) continue;
            if (dist_to_shape(s, x, y, 0, 0) <= c->erase_radius) {
                doc_explode_array_at(c->doc, (gsize)i);
                exploded = TRUE;
                any = TRUE;
                break;
            }
        }
    } while (exploded);

    /* 2) 对线段/路径做擦除切分（反向遍历，便于一次擦除多个） */
    for (gssize i = (gssize)st->n - 1; i >= 0; i--) {
        Shape *s = st->items[i];
        if (s->kind == SHAPE_LINE) {
            GPtrArray *out = g_ptr_array_new();
            if (erase_line_split(c, s, x, y, out)) {
                doc_replace_shape_with_shapes(c->doc, (gsize)i,
                    (Shape **)out->pdata, out->len);
                any = TRUE;
            }
            g_ptr_array_free(out, TRUE);
        } else if (s->kind == SHAPE_PATH) {
            gsize n = s->u.path.n;
            if (n == 0) continue;
            gboolean any_cut = FALSE;
            gboolean *keep = g_new(gboolean, n);
            for (gsize k = 0; k < n; k++) {
                double px = s->u.path.pt[k].x + s->dx;
                double py = s->u.path.pt[k].y + s->dy;
                gboolean cut = dist_pt(px, py, x, y) <= c->erase_radius;
                keep[k] = !cut;
                if (cut) any_cut = TRUE;
            }
            if (!any_cut) { g_free(keep); continue; }

            GPtrArray *new_paths = g_ptr_array_new();
            gsize k = 0;
            while (k < n) {
                while (k < n && !keep[k]) k++;
                gsize a = k;
                while (k < n && keep[k]) k++;
                gsize b = k;
                if (b - a >= 2) {
                    Shape *np = shape_new_path();
                    for (gsize j = a; j < b; j++)
                        shape_path_add_point(np, s->u.path.pt[j]);
                    g_ptr_array_add(new_paths, np);
                }
            }
            g_free(keep);

            doc_replace_shape_with_shapes(c->doc, (gsize)i,
                (Shape **)new_paths->pdata, new_paths->len);
            g_ptr_array_free(new_paths, TRUE);
            any = TRUE;
        }
    }
    if (any) selection_clear(c);
}

/* ─── 手势事件 ───────────────────────────────────────────────── */

static void on_drag_begin(GtkGestureDrag *g, double sx, double sy,
                          gpointer data) {
    CanvasCtx *c = data;
    if (!c->doc) return;

    if (c->array_active) {
        c->ar_drag_orig_dx = c->ar_dx;
        c->ar_drag_orig_dy = c->ar_dy;
        return;
    }

    switch (c->tool) {
    case TOOL_LINE:
        c->drawing_line = TRUE;
        c->line_start.x = sx; c->line_start.y = sy;
        c->line_cur     = c->line_start;
        break;
    case TOOL_PATH:
        if (c->path_pending) shape_free(c->path_pending);
        c->path_pending = shape_new_path();
        shape_path_add_point(c->path_pending, (DPoint){ sx, sy });
        break;
    case TOOL_ERASE:
        erase_at(c, sx, sy);
        notify_changed(c);
        break;
    case TOOL_SELECT: {
        GdkModifierType mstate = gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(g));
        gboolean ctrl = (mstate & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) != 0;
        int hit = hit_test(c, sx, sy, HIT_THRESH);
        if (ctrl) {
            if (hit >= 0) selection_toggle(c, hit);
            c->drag_moving = FALSE;
        } else {
            if (hit >= 0) {
                if (!is_selected(c, hit)) {
                    selection_clear(c);
                    c->selected_idx = hit;
                }
                /* 准备拖动整组 */
                c->drag_moving = TRUE;
                g_array_set_size(c->sel_origs, 0);
                ShapeStore *st = doc_active_store(c->doc);
                if (c->selected_idx >= 0 &&
                    (gsize)c->selected_idx < st->n) {
                    Shape *ss = st->items[c->selected_idx];
                    SelOrig so = { c->selected_idx, ss->dx, ss->dy };
                    g_array_append_val(c->sel_origs, so);
                }
                for (guint k = 0; k < c->selected_extra->len; k++) {
                    int xi = g_array_index(c->selected_extra, int, k);
                    if ((gsize)xi < st->n) {
                        Shape *ss = st->items[xi];
                        SelOrig so = { xi, ss->dx, ss->dy };
                        g_array_append_val(c->sel_origs, so);
                    }
                }
            } else {
                selection_clear(c);
                c->drag_moving = FALSE;
            }
        }
        notify_changed(c);
        break;
    }
    }
}

static void on_drag_update(GtkGestureDrag *g, double ox, double oy,
                           gpointer data) {
    CanvasCtx *c = data;
    if (!c->doc) return;

    double sx, sy;
    gtk_gesture_drag_get_start_point(g, &sx, &sy);
    double cx = sx + ox, cy = sy + oy;

    if (c->array_active) {
        c->ar_dx = c->ar_drag_orig_dx + ox;
        c->ar_dy = c->ar_drag_orig_dy + oy;
        gtk_widget_queue_draw(c->area);
        return;
    }

    switch (c->tool) {
    case TOOL_LINE:
        if (c->drawing_line) {
            c->line_cur.x = cx; c->line_cur.y = cy;
            gtk_widget_queue_draw(c->area);
        }
        break;
    case TOOL_PATH:
        if (c->path_pending) {
            DPoint last = c->path_pending->u.path.pt[
                c->path_pending->u.path.n - 1];
            if (dist_pt(cx, cy, last.x, last.y) >= PATH_SAMPLE_THRESH) {
                shape_path_add_point(c->path_pending, (DPoint){ cx, cy });
                gtk_widget_queue_draw(c->area);
            }
        }
        break;
    case TOOL_ERASE:
        erase_at(c, cx, cy);
        notify_changed(c);
        break;
    case TOOL_SELECT:
        if (c->drag_moving && c->sel_origs->len > 0) {
            ShapeStore *st = doc_active_store(c->doc);
            for (guint k = 0; k < c->sel_origs->len; k++) {
                SelOrig *so = &g_array_index(c->sel_origs, SelOrig, k);
                if ((gsize)so->idx < st->n) {
                    Shape *ss = st->items[so->idx];
                    ss->dx = so->dx0 + ox;
                    ss->dy = so->dy0 + oy;
                }
            }
            gtk_widget_queue_draw(c->area);
        }
        break;
    }
}

static void on_drag_end(GtkGestureDrag *g, double ox, double oy,
                        gpointer data) {
    CanvasCtx *c = data;
    if (!c->doc) return;

    double sx, sy;
    gtk_gesture_drag_get_start_point(g, &sx, &sy);
    double cx = sx + ox, cy = sy + oy;

    if (c->array_active) return;

    switch (c->tool) {
    case TOOL_LINE:
        if (c->drawing_line) {
            c->drawing_line = FALSE;
            if (dist_pt(c->line_start.x, c->line_start.y, cx, cy)
                    >= MIN_LINE_LEN) {
                Shape *s = shape_new_line(c->line_start, (DPoint){cx, cy});
                doc_add_shape(c->doc, s);
                notify_changed(c);
            } else {
                gtk_widget_queue_draw(c->area);
            }
        }
        break;
    case TOOL_PATH:
        if (c->path_pending) {
            if (c->path_pending->u.path.n >= 2) {
                doc_add_shape(c->doc, c->path_pending);
                c->path_pending = NULL;
                notify_changed(c);
            } else {
                shape_free(c->path_pending);
                c->path_pending = NULL;
                gtk_widget_queue_draw(c->area);
            }
        }
        break;
    case TOOL_ERASE:
        notify_changed(c);
        break;
    case TOOL_SELECT:
        c->drag_moving = FALSE;
        g_array_set_size(c->sel_origs, 0);
        notify_changed(c);
        break;
    }
}

/* ─── 鼠标位置（擦除光标） ─────────────────────────────────────── */

static void on_motion(GtkEventControllerMotion *m,
                      double x, double y, gpointer data) {
    (void)m;
    CanvasCtx *c = data;
    c->has_pointer = TRUE;
    c->pointer_pos.x = x; c->pointer_pos.y = y;
    if (c->tool == TOOL_ERASE) gtk_widget_queue_draw(c->area);
}

static void on_motion_leave(GtkEventControllerMotion *m, gpointer data) {
    (void)m;
    CanvasCtx *c = data;
    c->has_pointer = FALSE;
    if (c->tool == TOOL_ERASE) gtk_widget_queue_draw(c->area);
}

/* ─── 公共 API ───────────────────────────────────────────────── */

GtkWidget *doodle_canvas_new(DoodleDoc *doc) {
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);
    gtk_widget_add_css_class(area, "doodle-canvas");

    CanvasCtx *c = g_new0(CanvasCtx, 1);
    c->area              = area;
    c->doc               = doc;
    c->tool              = TOOL_SELECT;
    c->selected_idx      = -1;
    c->selected_extra    = g_array_new(FALSE, FALSE, sizeof(int));
    c->sel_origs         = g_array_new(FALSE, FALSE, sizeof(SelOrig));
    c->array_preview_idx = g_array_new(FALSE, FALSE, sizeof(int));
    c->erase_radius      = ERASE_RADIUS_DEFAULT;
    c->ar_rows           = 2;
    c->ar_cols           = 3;
    c->ar_gap_x          = 80;
    c->ar_gap_y          = 60;
    g_object_set_data_full(G_OBJECT(area), CTX_KEY, c, ctx_free);

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), on_draw, c, NULL);

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin",  G_CALLBACK(on_drag_begin),  c);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), c);
    g_signal_connect(drag, "drag-end",    G_CALLBACK(on_drag_end),    c);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(drag));

    GtkEventController *mc = gtk_event_controller_motion_new();
    g_signal_connect(mc, "motion", G_CALLBACK(on_motion), c);
    g_signal_connect(mc, "leave",  G_CALLBACK(on_motion_leave), c);
    gtk_widget_add_controller(area, mc);

    return area;
}

DoodleDoc *doodle_canvas_get_doc(GtkWidget *w) {
    return ctx_of(w)->doc;
}

void doodle_canvas_set_tool(GtkWidget *w, Tool t) {
    CanvasCtx *c = ctx_of(w);
    c->tool = t;
    c->drawing_line = FALSE;
    if (c->path_pending) { shape_free(c->path_pending); c->path_pending = NULL; }
    c->drag_moving = FALSE;
    g_array_set_size(c->sel_origs, 0);
    gtk_widget_queue_draw(w);
}

Tool doodle_canvas_get_tool(GtkWidget *w) { return ctx_of(w)->tool; }

int doodle_canvas_get_selected_index(GtkWidget *w) {
    return ctx_of(w)->selected_idx;
}

void doodle_canvas_set_selected_index(GtkWidget *w, int idx) {
    CanvasCtx *c = ctx_of(w);
    selection_clear(c);
    c->selected_idx = idx;
    gtk_widget_queue_draw(w);
}

gboolean doodle_canvas_selection_array_eligible(GtkWidget *w) {
    CanvasCtx *c = ctx_of(w);
    if (c->array_active) return FALSE;
    GArray *sel = selection_collect_sorted(c);
    if (sel->len == 0) { g_array_free(sel, TRUE); return FALSE; }
    ShapeStore *st = doc_active_store(c->doc);
    gboolean ok = TRUE;
    for (guint k = 0; k < sel->len && ok; k++) {
        int xi = g_array_index(sel, int, k);
        if ((gsize)xi >= st->n || st->items[xi]->kind == SHAPE_ARRAY) ok = FALSE;
    }
    g_array_free(sel, TRUE);
    return ok;
}

int doodle_canvas_selection_count(GtkWidget *w) {
    CanvasCtx *c = ctx_of(w);
    int n = c->selected_idx >= 0 ? 1 : 0;
    n += (int)c->selected_extra->len;
    return n;
}

void doodle_canvas_request_redraw(GtkWidget *w) {
    gtk_widget_queue_draw(w);
}

void doodle_canvas_set_changed_cb(GtkWidget *w,
                                  DoodleChangedFn cb,
                                  gpointer user_data) {
    CanvasCtx *c = ctx_of(w);
    c->changed_cb   = cb;
    c->changed_data = user_data;
}

gboolean doodle_canvas_begin_array_preview(GtkWidget *w,
                                           int rows, int cols,
                                           double gap_x, double gap_y) {
    CanvasCtx *c = ctx_of(w);
    if (c->array_active) return FALSE;
    GArray *sel = selection_collect_sorted(c);
    if (sel->len == 0) { g_array_free(sel, TRUE); return FALSE; }
    ShapeStore *st = doc_active_store(c->doc);
    for (guint k = 0; k < sel->len; k++) {
        int xi = g_array_index(sel, int, k);
        if ((gsize)xi >= st->n || st->items[xi]->kind == SHAPE_ARRAY) {
            g_array_free(sel, TRUE);
            return FALSE;
        }
    }

    c->array_active = TRUE;
    g_array_set_size(c->array_preview_idx, 0);
    g_array_append_vals(c->array_preview_idx, sel->data, sel->len);
    g_array_free(sel, TRUE);
    c->ar_rows = rows; c->ar_cols = cols;
    c->ar_gap_x = gap_x; c->ar_gap_y = gap_y;
    c->ar_dx = c->ar_dy = 0;
    notify_changed(c);
    return TRUE;
}

void doodle_canvas_set_array_params(GtkWidget *w,
                                    int rows, int cols,
                                    double gap_x, double gap_y) {
    CanvasCtx *c = ctx_of(w);
    if (rows > 0) c->ar_rows = rows;
    if (cols > 0) c->ar_cols = cols;
    c->ar_gap_x = gap_x;
    c->ar_gap_y = gap_y;
    if (c->array_active) gtk_widget_queue_draw(w);
}

gboolean doodle_canvas_is_array_active(GtkWidget *w) {
    return ctx_of(w)->array_active;
}

void doodle_canvas_apply_array(GtkWidget *w) {
    CanvasCtx *c = ctx_of(w);
    if (!c->array_active) return;
    if (c->array_preview_idx && c->array_preview_idx->len > 0) {
        doc_apply_array(c->doc,
                        (const int *)c->array_preview_idx->data,
                        (int)c->array_preview_idx->len,
                        c->ar_rows, c->ar_cols,
                        c->ar_gap_x, c->ar_gap_y,
                        c->ar_dx, c->ar_dy);
    }
    c->array_active = FALSE;
    g_array_set_size(c->array_preview_idx, 0);
    c->ar_dx = c->ar_dy = 0;

    /* 选中刚生成的阵列组（位于 store 末尾） */
    selection_clear(c);
    ShapeStore *st = doc_active_store(c->doc);
    if (st->n > 0) c->selected_idx = (int)st->n - 1;
    notify_changed(c);
}

void doodle_canvas_cancel_array(GtkWidget *w) {
    CanvasCtx *c = ctx_of(w);
    if (!c->array_active) return;
    c->array_active = FALSE;
    g_array_set_size(c->array_preview_idx, 0);
    c->ar_dx = c->ar_dy = 0;
    notify_changed(c);
}
