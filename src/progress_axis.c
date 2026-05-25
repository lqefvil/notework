/**
 * progress_axis.c — 进度轴实现
 *
 * 几何与坐标系：
 *   · 弧长域：0..total_arc，按 album 内所有 LAYER_DOODLE 层的 LINE/PATH
 *     弧长拼接而成。SHAPE_ARRAY 当前不计入。
 *   · 像素域：width_px = max(MIN_AXIS_WIDTH, total_arc * PX_PER_UNIT)。
 *     线性双向换算 px = arc * width_px / total_arc。
 *   · 当 total_arc == 0（空相册）时退化：width_px = MIN_AXIS_WIDTH。
 *
 * 渲染层叠（自下而上）：
 *   1. 高亮热力图色带（淡黄）
 *   2. 主轴线 + 分页竖线 + 页码 + 段长
 *
 * 本模块不处理把手/拖动/激活区域 —— 这些属于轨道行的职责。
 */

#include "progress_axis.h"

#include <math.h>
#include <string.h>

/* 1 单位弧长 → 像素 */
#define PX_PER_UNIT       1.0
/* 空/极短相册的最小轴宽，避免 hadjustment.upper=0 导致同步失效 */
#define MIN_AXIS_WIDTH    800
/* 主轴上下留白 */
#define AXIS_TOP_PAD      18
#define AXIS_BOTTOM_PAD   18

struct ProgressAxis {
    Album            *album;
    GtkDrawingArea   *canvas;

    int               width_px;
    double            total_arc;
    GArray           *page_offsets;  /* double, length = page_count + 1 */

    /* 播放头 */
    double            playhead_arc;  /* 当前播放头弧长位置 */
    GtkWidget        *body_container; /* 借用，用于播放头拖动时刷新轨道 */
};

/* ─── 几何辅助 ──────────────────────────────────────────────────── */

/* 把 arc 域线性映射到像素 */
static double
arc_to_px(ProgressAxis *pa, double arc)
{
    if (pa->total_arc <= 0.0) return 0.0;
    if (arc <= 0.0) return 0.0;
    if (arc >= pa->total_arc) return (double)pa->width_px;
    return arc * (double)pa->width_px / pa->total_arc;
}

/* 重算弧长几何：page_offsets[i] = 累计到第 i 页起的全局弧长；
 * page_offsets[N] = total_arc。 */
static void
recompute_geometry(ProgressAxis *pa)
{
    g_array_set_size(pa->page_offsets, 0);
    double cum = 0.0;
    g_array_append_val(pa->page_offsets, cum);

    int n = album_page_count(pa->album);
    for (int i = 0; i < n; i++) {
        AlbumPage *p = album_get_page(pa->album, i);
        double arc = (p && p->doc) ? doodle_doc_total_arc(p->doc) : 0.0;
        cum += arc;
        g_array_append_val(pa->page_offsets, cum);
    }
    pa->total_arc = cum;

    int px = (int)(cum * PX_PER_UNIT + 0.5);
    pa->width_px = px < MIN_AXIS_WIDTH ? MIN_AXIS_WIDTH : px;

    if (pa->canvas)
        gtk_drawing_area_set_content_width(pa->canvas, pa->width_px);
}

/* ─── 高亮热力图：求 HighlightRecord 在宿主弧长上的区间 ─────────── */

/* 在折线 path（n_pts 个点）上找点 q 的最近段索引与段内参数 t∈[0,1]，
 * 返回该点在折线上的弧长。 */
static double
project_point_to_polyline_arc(const DPoint *pts, gsize n_pts, DPoint q)
{
    if (n_pts < 2) return 0.0;
    double best_d2  = G_MAXDOUBLE;
    double best_arc = 0.0;
    double cum      = 0.0;
    for (gsize i = 1; i < n_pts; i++) {
        double ax = pts[i - 1].x, ay = pts[i - 1].y;
        double bx = pts[i].x,     by = pts[i].y;
        double dx = bx - ax, dy = by - ay;
        double seg_len2 = dx * dx + dy * dy;
        double t = 0.0;
        if (seg_len2 > 1e-12) {
            t = ((q.x - ax) * dx + (q.y - ay) * dy) / seg_len2;
            if (t < 0.0) t = 0.0;
            else if (t > 1.0) t = 1.0;
        }
        double px = ax + t * dx, py = ay + t * dy;
        double ddx = q.x - px,  ddy = q.y - py;
        double d2 = ddx * ddx + ddy * ddy;
        if (d2 < best_d2) {
            best_d2  = d2;
            best_arc = cum + t * sqrt(seg_len2);
        }
        cum += sqrt(seg_len2);
    }
    return best_arc;
}

/* 计算 HighlightRecord r 在宿主 host 上的弧长区间 (s_arc, e_arc)。
 * 当 host 不是 LINE/PATH 或 r 无足够采样点时返回 FALSE。 */
static gboolean
highlight_record_arc_span(const Shape *host, const HighlightRecord *r,
                          double *out_s, double *out_e)
{
    if (!host || !r || r->n < 2) return FALSE;

    DPoint head = r->pt[0];
    DPoint tail = r->pt[r->n - 1];
    double a = 0.0, b = 0.0;

    if (host->kind == SHAPE_LINE) {
        DPoint pts[2] = { host->u.line.a, host->u.line.b };
        a = project_point_to_polyline_arc(pts, 2, head);
        b = project_point_to_polyline_arc(pts, 2, tail);
    } else if (host->kind == SHAPE_PATH) {
        if (host->u.path.n < 2) return FALSE;
        a = project_point_to_polyline_arc(host->u.path.pt, host->u.path.n, head);
        b = project_point_to_polyline_arc(host->u.path.pt, host->u.path.n, tail);
    } else {
        return FALSE;
    }
    if (a > b) { double t = a; a = b; b = t; }
    *out_s = a;
    *out_e = b;
    return TRUE;
}

/* 在某个 doc 内按 (host_layer_idx, host_shape_number) 找 host shape。
 * 返回 NULL 表示宿主不存在或类型不匹配。同时输出 host 在该层中的累计起点弧长，
 * 即 host 相对其所在层局部弧长域的起点。 */
static const Shape *
locate_host_shape(const DoodleDoc *doc, int layer_idx, int number,
                  double *out_layer_local_start)
{
    if (!doc || !doc->layers) return NULL;
    if (layer_idx < 0 || (guint)layer_idx >= doc->layers->len) return NULL;
    const Layer *L = &g_array_index(doc->layers, Layer, (guint)layer_idx);
    if (L->kind != LAYER_DOODLE) return NULL;

    double cum = 0.0;
    for (gsize k = 0; k < L->store.n; k++) {
        const Shape *s = L->store.items[k];
        if (s->number == number &&
            (s->kind == SHAPE_LINE || s->kind == SHAPE_PATH)) {
            *out_layer_local_start = cum;
            return s;
        }
        cum += shape_arc_length(s);
    }
    return NULL;
}

/* 算出某个 LAYER_DOODLE 层在该页内的"层起点"局部弧长（页内多个 doodle 层
 * 顺序拼接，与 doodle_doc_total_arc 的累加顺序一致）。 */
static double
layer_local_start_in_page(const DoodleDoc *doc, int target_layer_idx)
{
    if (!doc || !doc->layers) return 0.0;
    double cum = 0.0;
    for (guint i = 0; i < doc->layers->len; i++) {
        if ((int)i == target_layer_idx) break;
        const Layer *L = &g_array_index(doc->layers, Layer, i);
        if (L->kind != LAYER_DOODLE || !L->visible) continue;
        for (gsize k = 0; k < L->store.n; k++)
            cum += shape_arc_length(L->store.items[k]);
    }
    return cum;
}

/* ─── 渲染 ─────────────────────────────────────────────────────── */

static void
draw_heatmap(ProgressAxis *pa, cairo_t *cr, int height)
{
    double mid_y = (double)height * 0.5;

    int n = album_page_count(pa->album);
    for (int i = 0; i < n; i++) {
        AlbumPage *p = album_get_page(pa->album, i);
        if (!p || !p->doc) continue;
        double base = g_array_index(pa->page_offsets, double, i);

        DoodleDoc *doc = p->doc;
        if (!doc->layers) continue;

        for (guint li = 0; li < doc->layers->len; li++) {
            const Layer *L = &g_array_index(doc->layers, Layer, li);
            if (L->kind != LAYER_HIGHLIGHT) continue;
            if (!L->visible) continue;
            if (!L->highlights) continue;

            for (guint hi = 0; hi < L->highlights->len; hi++) {
                const HighlightRecord *r = g_ptr_array_index(L->highlights, hi);
                if (!r) continue;

                double layer_start = 0.0;
                const Shape *host = locate_host_shape(
                    doc, r->host_layer_idx, r->host_shape_number,
                    &layer_start);
                if (!host) continue;

                double s_local = 0.0, e_local = 0.0;
                if (!highlight_record_arc_span(host, r, &s_local, &e_local))
                    continue;

                double host_layer_pre = layer_local_start_in_page(
                    doc, r->host_layer_idx);

                double s_global = base + host_layer_pre + layer_start + s_local;
                double e_global = base + host_layer_pre + layer_start + e_local;

                double x0 = arc_to_px(pa, s_global);
                double x1 = arc_to_px(pa, e_global);
                if (x1 < x0) { double t = x0; x0 = x1; x1 = t; }
                if (x1 - x0 < 1.0) x1 = x0 + 1.0;  /* 至少 1px 可见 */

                cairo_set_source_rgba(cr, 1.0, 0.85, 0.20, 0.25);
                cairo_rectangle(cr, x0, mid_y - 10.0, x1 - x0, 20.0);
                cairo_fill(cr);
            }
        }
    }
}

static void
on_axis_draw(GtkDrawingArea *area, cairo_t *cr,
             int width, int height, gpointer user_data)
{
    (void)area;
    ProgressAxis *pa = user_data;
    if (!pa) return;

    /* 底层：高亮热力图 */
    draw_heatmap(pa, cr, height);

    /* 主轴线 */
    double mid_y = (double)height * 0.5;
    cairo_set_line_width(cr, 1.5);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
    cairo_move_to(cr, 0,     mid_y);
    cairo_line_to(cr, width, mid_y);
    cairo_stroke(cr);

    /* 分页竖线 + 页码 + 段长 */
    cairo_select_font_face(cr, "Sans",
                            CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);

    int n = album_page_count(pa->album);
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            double a0 = g_array_index(pa->page_offsets, double, i);
            double a1 = g_array_index(pa->page_offsets, double, i + 1);
            double x0_px, x1_px;
            if (pa->total_arc > 0.0) {
                x0_px = arc_to_px(pa, a0);
                x1_px = arc_to_px(pa, a1);
            } else {
                /* 空相册时按等分占位 */
                x0_px = (double)i        * (double)width / (double)n;
                x1_px = (double)(i + 1) * (double)width / (double)n;
            }

            /* 段起竖线 */
            cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, x0_px, AXIS_TOP_PAD - 4);
            cairo_line_to(cr, x0_px, height - AXIS_BOTTOM_PAD + 4);
            cairo_stroke(cr);

            /* 页码 */
            char buf[32];
            cairo_text_extents_t ext;
            g_snprintf(buf, sizeof buf, "P%d", i + 1);
            cairo_text_extents(cr, buf, &ext);
            double cx = (x0_px + x1_px) * 0.5 - ext.width * 0.5;
            cairo_set_source_rgba(cr, 0, 0, 0, 0.75);
            cairo_move_to(cr, cx, AXIS_TOP_PAD - 6);
            cairo_show_text(cr, buf);

            /* 段长数值 */
            double arc = a1 - a0;
            if (arc > 0.0) g_snprintf(buf, sizeof buf, "%.0f", arc);
            else           g_snprintf(buf, sizeof buf, "—");
            cairo_text_extents(cr, buf, &ext);
            cx = (x0_px + x1_px) * 0.5 - ext.width * 0.5;
            cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
            cairo_move_to(cr, cx, height - AXIS_BOTTOM_PAD + 14);
            cairo_show_text(cr, buf);
        }

        /* 末端竖线 */
        cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, width, AXIS_TOP_PAD - 4);
        cairo_line_to(cr, width, height - AXIS_BOTTOM_PAD + 4);
        cairo_stroke(cr);
    }

    /* 最后：播放头竖线（始终绘制，不依赖是否有页面） */
    {
        double ph_px = progress_axis_arc_to_px(pa, pa->playhead_arc);
        cairo_set_source_rgba(cr, 0.85, 0.15, 0.15, 0.90);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, ph_px, 0);
        cairo_line_to(cr, ph_px, height);
        cairo_stroke(cr);
        /* 顶部小三角 */
        cairo_move_to(cr, ph_px - 5, 0);
        cairo_line_to(cr, ph_px + 5, 0);
        cairo_line_to(cr, ph_px,     7);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
}

/* ─── 对外接口 ─────────────────────────────────────────────────── */

ProgressAxis *
progress_axis_new(GtkDrawingArea *canvas, Album *album)
{
    g_return_val_if_fail(GTK_IS_DRAWING_AREA(canvas), NULL);
    g_return_val_if_fail(album != NULL, NULL);

    ProgressAxis *pa = g_new0(ProgressAxis, 1);
    pa->album        = album;
    pa->canvas       = canvas;
    pa->page_offsets = g_array_new(FALSE, FALSE, sizeof(double));

    /* canvas 主由主窗口拥有；主窗口销毁时 canvas 先于
     * appview_free 被销毁。这里用 weak pointer 跟踪，canvas 销毁
     * 后 pa->canvas 自动置 NULL，progress_axis_free 中跳过接下来
     * 对 canvas 的调用。 */
    g_object_add_weak_pointer(G_OBJECT(canvas), (gpointer *)&pa->canvas);

    gtk_drawing_area_set_draw_func(canvas, on_axis_draw, pa, NULL);
    return pa;
}

void
progress_axis_free(ProgressAxis *pa)
{
    if (!pa) return;
    if (pa->canvas) {
        /* canvas 仍存活：解除 draw_func 并撤销 weak pointer。 */
        gtk_drawing_area_set_draw_func(pa->canvas, NULL, NULL, NULL);
        g_object_remove_weak_pointer(G_OBJECT(pa->canvas),
                                      (gpointer *)&pa->canvas);
        pa->canvas = NULL;
    }
    if (pa->page_offsets) g_array_free(pa->page_offsets, TRUE);
    g_free(pa);
}

void
progress_axis_refresh(ProgressAxis *pa)
{
    if (!pa) return;
    recompute_geometry(pa);
    if (pa->canvas) gtk_widget_queue_draw(GTK_WIDGET(pa->canvas));
}

int
progress_axis_get_content_width(ProgressAxis *pa)
{
    return pa ? pa->width_px : MIN_AXIS_WIDTH;
}

double
progress_axis_get_total_arc(ProgressAxis *pa)
{
    return pa ? pa->total_arc : 0.0;
}

double
progress_axis_get_drag_arc_max(ProgressAxis *pa)
{
    if (!pa) return 0.0;
    return pa->total_arc > 0.0 ? pa->total_arc : (double)pa->width_px;
}

double
progress_axis_arc_to_px(ProgressAxis *pa, double arc)
{
    if (!pa || pa->width_px <= 0) return 0.0;
    if (pa->total_arc <= 0.0) {
        /* 1:1 兑底（空相册）：arc 域 == px 域 */
        if (arc <= 0.0) return 0.0;
        if (arc >= (double)pa->width_px) return (double)pa->width_px;
        return arc;
    }
    return arc_to_px(pa, arc);
}

double
progress_axis_px_to_arc(ProgressAxis *pa, double px)
{
    if (!pa || pa->width_px <= 0) return 0.0;
    if (pa->total_arc <= 0.0) {
        if (px <= 0.0) return 0.0;
        if (px >= (double)pa->width_px) return (double)pa->width_px;
        return px;
    }
    if (px <= 0.0) return 0.0;
    if (px >= (double)pa->width_px) return pa->total_arc;
    return px * pa->total_arc / (double)pa->width_px;
}

/* ─── 吸附点收集 ─────────────────────────────────── */

static int
cmp_double(gconstpointer a, gconstpointer b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x < y) ? -1 : (x > y ? 1 : 0);
}

/* 返回该形状的弧长与首末端点的局部弧长（在 shape 内）。
 *   head_local = 0
 *   tail_local = arc_length （LINE/PATH） */
static gboolean
shape_endpoints_local(const Shape *s, double *head_local, double *tail_local)
{
    if (!s) return FALSE;
    if (s->kind == SHAPE_LINE) {
        *head_local = 0.0;
        *tail_local = shape_arc_length(s);
        return TRUE;
    }
    if (s->kind == SHAPE_PATH) {
        *head_local = 0.0;
        *tail_local = shape_arc_length(s);
        return TRUE;
    }
    return FALSE;
}

GArray *
progress_axis_collect_snap_arcs(ProgressAxis *pa)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(double));
    if (!pa) return out;

    /* (1) 分页边界。page_offsets 末元素即 total_arc。 */
    if (pa->page_offsets) {
        for (guint i = 0; i < pa->page_offsets->len; i++) {
            double v = g_array_index(pa->page_offsets, double, i);
            g_array_append_val(out, v);
        }
    }

    /* (2) 每页 LAYER_DOODLE 中 LINE/PATH 的首末端点。 */
    int n = album_page_count(pa->album);
    for (int i = 0; i < n; i++) {
        AlbumPage *p = album_get_page(pa->album, i);
        if (!p || !p->doc || !p->doc->layers) continue;
        double base = (pa->page_offsets && (guint)i < pa->page_offsets->len)
                      ? g_array_index(pa->page_offsets, double, i) : 0.0;

        double layer_pre = 0.0;  /* 页内多个 doodle 层的累计偏移 */
        for (guint li = 0; li < p->doc->layers->len; li++) {
            const Layer *L = &g_array_index(p->doc->layers, Layer, li);
            if (L->kind != LAYER_DOODLE || !L->visible) continue;
            double cum = 0.0;
            for (gsize k = 0; k < L->store.n; k++) {
                const Shape *s = L->store.items[k];
                double head_l = 0.0, tail_l = 0.0;
                if (shape_endpoints_local(s, &head_l, &tail_l)) {
                    double head_g = base + layer_pre + cum + head_l;
                    double tail_g = base + layer_pre + cum + tail_l;
                    g_array_append_val(out, head_g);
                    g_array_append_val(out, tail_g);
                }
                cum += shape_arc_length(s);
            }
            layer_pre += cum;
        }
    }

    /* 排序 + 粗略去重（1e-6 阈值） */
    g_array_sort(out, cmp_double);
    if (out->len > 1) {
        guint w = 1;
        for (guint r = 1; r < out->len; r++) {
            double prev = g_array_index(out, double, w - 1);
            double cur  = g_array_index(out, double, r);
            if (cur - prev > 1e-6) {
                g_array_index(out, double, w) = cur;
                w++;
            }
        }
        g_array_set_size(out, w);
    }
    return out;
}

/* ─── 播放头拖动手势 ───────────────────────────────────── */

static void
playhead_update_to_px(ProgressAxis *pa, double px)
{
    double arc = progress_axis_px_to_arc(pa, px);
    double max_arc = progress_axis_get_drag_arc_max(pa);
    if (arc < 0.0) arc = 0.0;
    if (arc > max_arc) arc = max_arc;
    pa->playhead_arc = arc;
    if (pa->canvas)
        gtk_widget_queue_draw(GTK_WIDGET(pa->canvas));
    /* 逐个刷新轨道 bar（GtkDrawingArea 需要显式 invalidate） */
    if (pa->body_container) {
        for (GtkWidget *c = gtk_widget_get_first_child(pa->body_container);
             c; c = gtk_widget_get_next_sibling(c))
            gtk_widget_queue_draw(c);
    }
}

static void
on_playhead_drag_begin(GtkGestureDrag *g, double start_x, double start_y,
                       gpointer user_data)
{
    (void)start_y;
    ProgressAxis *pa = user_data;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    playhead_update_to_px(pa, start_x);
}

static void
on_playhead_drag_update(GtkGestureDrag *g, double offset_x, double offset_y,
                        gpointer user_data)
{
    (void)offset_y;
    ProgressAxis *pa = user_data;
    double sx, sy;
    gtk_gesture_drag_get_start_point(g, &sx, &sy);
    playhead_update_to_px(pa, sx + offset_x);
}

/* ─── 播放头对外接口 ───────────────────────────────────── */

double
progress_axis_get_playhead_px(ProgressAxis *pa)
{
    if (!pa) return 0.0;
    return progress_axis_arc_to_px(pa, pa->playhead_arc);
}

void
progress_axis_set_playhead_arc(ProgressAxis *pa, double arc)
{
    if (!pa) return;
    pa->playhead_arc = arc;
    if (pa->canvas)
        gtk_widget_queue_draw(GTK_WIDGET(pa->canvas));
    if (pa->body_container) {
        for (GtkWidget *c = gtk_widget_get_first_child(pa->body_container);
             c; c = gtk_widget_get_next_sibling(c))
            gtk_widget_queue_draw(c);
    }
}

/* 将播放头拖动手势绑定到 canvas，并记录 body_container 供联动刷新 */
void
progress_axis_setup_playhead(ProgressAxis *pa, GtkWidget *body_container)
{
    if (!pa || !pa->canvas) return;
    pa->body_container = body_container;

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin",
                     G_CALLBACK(on_playhead_drag_begin), pa);
    g_signal_connect(drag, "drag-update",
                     G_CALLBACK(on_playhead_drag_update), pa);
    gtk_widget_add_controller(GTK_WIDGET(pa->canvas),
                              GTK_EVENT_CONTROLLER(drag));
}
