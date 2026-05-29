/**
 * track_row.c — 轨道 bar 实现
 *
 * 几何：bar 的 widget 局部坐标系 [0..width] 与进度轴的全局弧长域
 * [0..total_arc] 通过 progress_axis_arc_to_px / px_to_arc 双向换算。
 *
 * 交互：
 *   · drag-begin：从所有 HandlePair 的 A、B 端点中找最近一个 ≤12px 的，
 *     若命中则进入「移动现有把手」；否则在落点 arc 处插入一对新 (a,b) 都
 *     等于 arc 的 HandlePair，进入「创建新把手对」。
 *   · drag-update：候选 arc = px_to_arc(start_x + offset_x)，先做 8px 像素
 *     阈值的吸附（吸附目标由 progress_axis_collect_snap_arcs 提供：分页边界
 *     + LINE/PATH 首末端点），再 clamp 到 [0, drag_arc_max]。
 *       - 创建模式：B 跟随光标，A 保持落点（允许 B<A，由 drag-end 归一）；
 *       - 移动模式（A）：clamp 到 [0, drag_initial_b]；
 *       - 移动模式（B）：clamp 到 [drag_initial_a, drag_arc_max]。
 *   · drag-end：a/b 顺序归一；若是「创建模式」且最终 b-a < 1e-6，删除该
 *     把手对（视为单击，无意义）。
 *
 * 渲染：
 *   · 激活区色块：rgba(0.20, 0.45, 0.85, 0.25)，纵向贯穿 bar；
 *   · 把手三角：在 bar 顶端水平居中于 a/b，深蓝色填充；
 *   · 端点细竖线：贯穿 bar 高度，便于视觉对齐。
 *
 * 生命周期：TrackRow 控制器以 g_object_set_data_full(bar, "track-row")
 * 挂在返回的 bar 上，bar 被容器销毁时 qdata 自动 free。
 */

#include "track_row.h"

#include <math.h>

/* 命中半径：拖动起点距把手 ≤ 这个像素则视为命中该把手 */
#define HANDLE_HIT_PX     12
/* 吸附阈值：候选位置距某个吸附点 ≤ 这个像素则吸过去 */
#define SNAP_THRESHOLD_PX 8
/* 把手三角宽/高（半宽与高度，分别是水平与竖直方向） */
#define HANDLE_HALF_W     6
#define HANDLE_HEIGHT     10

/* 单击阈值：拖拽总偏移 < 该像素视为“单击”，触发选中回调 */
#define CLICK_MOVE_PX     3.0

typedef struct TrackRow {
    Album          *album;       /* 借用 */
    ProgressAxis   *axis;        /* 借用 */
    int             track_idx;
    GtkDrawingArea *bar;         /* weak pointer：随 bar 销毁置 NULL */

    /* 拖动状态。drag_pair_idx == -1 表示空闲。 */
    int             drag_pair_idx;
    int             drag_side;        /* 0=A, 1=B */
    gboolean        drag_creating;
    gboolean        drag_playhead;    /* TRUE = 正在拖动播放头 */
    double          drag_initial_a;
    double          drag_initial_b;
    double          drag_start_x;     /* 落点 widget 局部 x */

    /* 选中 / 单击语义 */
    int             selected_pair_idx;     /* -1 表示无选中，仅用于渲染 */
    int             candidate_pair_idx;    /* drag_begin 时色块命中，供 drag_end 判定 */
    TrackPairSelectedFn pair_selected_cb;
    gpointer            pair_selected_data;
} TrackRow;

/* ─── 数据访问辅助 ─────────────────────────────────────────────── */

static Track *
track_row_get_track(TrackRow *tr)
{
    if (!tr || !tr->album || !tr->album->tracks) return NULL;
    if (tr->track_idx < 0 ||
        (guint)tr->track_idx >= tr->album->tracks->len) return NULL;
    return &g_array_index(tr->album->tracks, Track, (guint)tr->track_idx);
}

/* ─── 渲染 ───────────────────────────────────────────────────── */

static void
draw_handle_triangle(cairo_t *cr, double x, double height)
{
    /* 顶部朝下三角（顶在 (x, HANDLE_HEIGHT)），底边贴 bar 顶 (y=0) */
    cairo_move_to(cr, x - HANDLE_HALF_W, 0);
    cairo_line_to(cr, x + HANDLE_HALF_W, 0);
    cairo_line_to(cr, x,                  HANDLE_HEIGHT);
    cairo_close_path(cr);
    cairo_fill(cr);

    /* 端点细竖线，贯穿整个 bar，方便视觉对齐 */
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x, 0);
    cairo_line_to(cr, x, height);
    cairo_stroke(cr);
}

static void
on_bar_draw(GtkDrawingArea *area, cairo_t *cr,
            int width, int height, gpointer user_data)
{
    (void)area; (void)width;
    TrackRow *tr = user_data;
    if (!tr) return;
    Track *t = track_row_get_track(tr);
    if (!t || !t->pairs) return;

    for (guint i = 0; i < t->pairs->len; i++) {
        const HandlePair *p = &g_array_index(t->pairs, HandlePair, i);
        double a = p->a_arc, b = p->b_arc;
        if (a > b) { double tmp = a; a = b; b = tmp; }

        double xa = progress_axis_arc_to_px(tr->axis, a);
        double xb = progress_axis_arc_to_px(tr->axis, b);
        if (xb < xa) { double tmp = xa; xa = xb; xb = tmp; }

        gboolean is_sel = ((int)i == tr->selected_pair_idx);

        /* 激活区色块：选中时提升 alpha + 1.5px 深蓝描边 */
        if (is_sel) {
            cairo_set_source_rgba(cr, 0.20, 0.45, 0.85, 0.40);
        } else {
            cairo_set_source_rgba(cr, 0.20, 0.45, 0.85, 0.25);
        }
        cairo_rectangle(cr, xa, 0, xb - xa, (double)height);
        cairo_fill(cr);

        if (is_sel && (xb - xa) > 1.5) {
            cairo_set_source_rgba(cr, 0.10, 0.30, 0.70, 1.0);
            cairo_set_line_width(cr, 1.5);
            cairo_rectangle(cr, xa + 0.75, 0.75,
                            xb - xa - 1.5, (double)height - 1.5);
            cairo_stroke(cr);
        }

        /* 把手 */
        cairo_set_source_rgba(cr, 0.10, 0.30, 0.70, 0.95);
        draw_handle_triangle(cr, xa, (double)height);
        draw_handle_triangle(cr, xb, (double)height);
    }

    /* 播放头竖线（贯穿所有轨道） */
    double ph_px = progress_axis_get_playhead_px(tr->axis);
    cairo_set_source_rgba(cr, 0.85, 0.15, 0.15, 0.90);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, ph_px, 0);
    cairo_line_to(cr, ph_px, (double)height);
    cairo_stroke(cr);
}

/* ─── 吸附 ───────────────────────────────────────────────────── */

static double
apply_snap(TrackRow *tr, double candidate_arc)
{
    GArray *snaps = progress_axis_collect_snap_arcs(tr->axis);
    if (!snaps || snaps->len == 0) {
        if (snaps) g_array_unref(snaps);
        return candidate_arc;
    }

    double cand_px  = progress_axis_arc_to_px(tr->axis, candidate_arc);
    double best_d   = (double)SNAP_THRESHOLD_PX;
    double best_arc = candidate_arc;
    for (guint i = 0; i < snaps->len; i++) {
        double a  = g_array_index(snaps, double, i);
        double px = progress_axis_arc_to_px(tr->axis, a);
        double d  = fabs(px - cand_px);
        if (d <= best_d) {
            best_d   = d;
            best_arc = a;
        }
    }
    g_array_unref(snaps);
    return best_arc;
}

/* ─── 拖动交互 ───────────────────────────────────────────────── */

static void
on_drag_begin(GtkGestureDrag *g, double start_x, double start_y,
              gpointer user_data)
{
    (void)start_y;
    TrackRow *tr = user_data;
    if (!tr) return;

    Track *t = track_row_get_track(tr);
    if (!t) {
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }
    /* 空相册也允许交互：progress_axis 在 total_arc<=0 时提供了
     * 1:1 像素兑底，轨道行在这个兑底域上运行。 */

    /* 命中检测：所有 pair 的 A、B 端点中最近的且 ≤ HANDLE_HIT_PX */
    double best_d    = (double)HANDLE_HIT_PX;
    int    best_pair = -1;
    int    best_side = 0;
    if (t->pairs) {
        for (guint i = 0; i < t->pairs->len; i++) {
            const HandlePair *p = &g_array_index(t->pairs, HandlePair, i);
            double xa = progress_axis_arc_to_px(tr->axis, p->a_arc);
            double xb = progress_axis_arc_to_px(tr->axis, p->b_arc);
            double da = fabs(start_x - xa);
            double db = fabs(start_x - xb);
            if (da <= best_d) { best_d = da; best_pair = (int)i; best_side = 0; }
            if (db <= best_d) { best_d = db; best_pair = (int)i; best_side = 1; }
        }
    }

    if (best_pair >= 0) {
        /* 移动现有把手 */
        const HandlePair *p = &g_array_index(t->pairs, HandlePair,
                                              (guint)best_pair);
        tr->drag_pair_idx  = best_pair;
        tr->drag_side      = best_side;
        tr->drag_creating  = FALSE;
        tr->drag_playhead  = FALSE;
        tr->drag_initial_a = p->a_arc;
        tr->drag_initial_b = p->b_arc;
        tr->drag_start_x   = start_x;
        tr->candidate_pair_idx = -1;
    } else {
        /* 检查是否命中播放头线（优先于新建把手对） */
        double ph_px = progress_axis_get_playhead_px(tr->axis);
        if (fabs(start_x - ph_px) <= HANDLE_HIT_PX) {
            tr->drag_pair_idx = -1;
            tr->drag_playhead = TRUE;
            tr->drag_start_x  = start_x;
            tr->candidate_pair_idx = -1;
            progress_axis_set_playhead_arc(tr->axis,
                progress_axis_px_to_arc(tr->axis, start_x));
        } else {
            /* 未命中把手/播放头：先检查是否点中某条“激活区色块”（xa<x<xb），
             * 记录候选供 drag_end 判定单击；drag_end 会在偏移<3px 时触发选中回调。
             * 该判定不阻断原有“创建新对”路径（偏移大时仍可拖出新对，
             * 零宽重叠对会在 drag_end 被现有逻辑自动删除）。 */
            int hit_pair = -1;
            if (t->pairs) {
                for (guint i = 0; i < t->pairs->len; i++) {
                    const HandlePair *p = &g_array_index(t->pairs, HandlePair, i);
                    double xa = progress_axis_arc_to_px(tr->axis, p->a_arc);
                    double xb = progress_axis_arc_to_px(tr->axis, p->b_arc);
                    if (xa > xb) { double tmp = xa; xa = xb; xb = tmp; }
                    if (start_x > xa && start_x < xb) { hit_pair = (int)i; break; }
                }
            }
            tr->candidate_pair_idx = hit_pair;

            /* 空白处：创建新把手对，A 落在落点，B 跟随光标 */
            double arc     = progress_axis_px_to_arc(tr->axis, start_x);
            int    new_idx = album_track_add_pair(tr->album, tr->track_idx,
                                                    arc, arc);
            if (new_idx < 0) {
                gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
                return;
            }
            tr->drag_pair_idx  = new_idx;
            tr->drag_side      = 1;       /* B 跟随光标 */
            tr->drag_creating  = TRUE;
            tr->drag_playhead  = FALSE;
            tr->drag_initial_a = arc;
            tr->drag_initial_b = arc;
            tr->drag_start_x   = start_x;
            if (tr->bar) gtk_widget_queue_draw(GTK_WIDGET(tr->bar));
        }
    }
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_drag_update(GtkGestureDrag *g, double offset_x, double offset_y,
               gpointer user_data)
{
    (void)g; (void)offset_y;
    TrackRow *tr = user_data;
    if (!tr) return;

    /* 播放头拖动模式 */
    if (tr->drag_playhead) {
        progress_axis_set_playhead_arc(tr->axis,
            progress_axis_px_to_arc(tr->axis, tr->drag_start_x + offset_x));
        return;
    }

    if (tr->drag_pair_idx < 0) return;

    double total = progress_axis_get_drag_arc_max(tr->axis);
    if (total <= 0.0) return;

    double cand = progress_axis_px_to_arc(tr->axis,
                                            tr->drag_start_x + offset_x);
    cand = apply_snap(tr, cand);
    if (cand < 0.0)   cand = 0.0;
    if (cand > total) cand = total;

    double a = tr->drag_initial_a;
    double b = tr->drag_initial_b;
    if (tr->drag_creating) {
        /* B 跟随光标；A 维持落点。允许 B<A，drag-end 时归一。 */
        b = cand;
    } else if (tr->drag_side == 0) {
        a = cand;
        if (a > tr->drag_initial_b) a = tr->drag_initial_b;
    } else {
        b = cand;
        if (b < tr->drag_initial_a) b = tr->drag_initial_a;
    }

    album_track_update_pair(tr->album, tr->track_idx,
                             tr->drag_pair_idx, a, b);
    if (tr->bar) gtk_widget_queue_draw(GTK_WIDGET(tr->bar));
}

static void
on_drag_end(GtkGestureDrag *g, double offset_x, double offset_y,
            gpointer user_data)
{
    (void)g;
    TrackRow *tr = user_data;
    if (!tr) return;

    /* 播放头拖动结束 */
    if (tr->drag_playhead) {
        tr->drag_playhead = FALSE;
        tr->candidate_pair_idx = -1;
        return;
    }

    /* 判定“单击”：总偏移 < CLICK_MOVE_PX 且色块命中候选有效 → 触发选中回调。
     * 该检测位于原“创建新对”逻辑之前，以便零宽重叠对仍可被下面的
     * 逻辑清除。candidate_pair_idx 是取自拖拽前的快照，不受末尾新增
     * pair 影响（末尾删除不会动中间下标）。 */
    if (tr->candidate_pair_idx >= 0) {
        double dx = offset_x; if (dx < 0) dx = -dx;
        double dy = offset_y; if (dy < 0) dy = -dy;
        if (dx < CLICK_MOVE_PX && dy < CLICK_MOVE_PX && tr->pair_selected_cb) {
            tr->pair_selected_cb(tr->track_idx,
                                  tr->candidate_pair_idx,
                                  tr->pair_selected_data);
        }
    }
    tr->candidate_pair_idx = -1;

    if (tr->drag_pair_idx < 0) return;

    Track *t = track_row_get_track(tr);
    if (t && t->pairs &&
        tr->drag_pair_idx < (int)t->pairs->len) {
        HandlePair *p = &g_array_index(t->pairs, HandlePair,
                                        (guint)tr->drag_pair_idx);
        /* 归一 a<=b */
        if (p->a_arc > p->b_arc) {
            double tmp = p->a_arc; p->a_arc = p->b_arc; p->b_arc = tmp;
        }
        /* 创建模式下若最终零宽（纯点击），删除该对 */
        if (tr->drag_creating && (p->b_arc - p->a_arc) < 1e-6) {
            album_track_remove_pair(tr->album, tr->track_idx,
                                     tr->drag_pair_idx);
        }
    }

    tr->drag_pair_idx = -1;
    tr->drag_creating = FALSE;
    if (tr->bar) gtk_widget_queue_draw(GTK_WIDGET(tr->bar));
}

/* ─── 控制器析构（挂在 bar 上） ─────────────────────────────── */

static void
track_row_destroy(gpointer data)
{
    TrackRow *tr = data;
    if (!tr) return;
    if (tr->bar) {
        g_object_remove_weak_pointer(G_OBJECT(tr->bar),
                                      (gpointer *)&tr->bar);
        tr->bar = NULL;
    }
    g_free(tr);
}

/* ─── 对外接口 ────────────────────────────────────────────── */

GtkWidget *
track_row_new(Album *album, ProgressAxis *axis,
               int track_idx, int bar_width_px)
{
    GtkBuilder *b = gtk_builder_new_from_resource(
        "/com/github/notework/track_row.ui");
    GtkDrawingArea *bar = GTK_DRAWING_AREA(
        gtk_builder_get_object(b, "track_bar"));

    /* bar 宽度（与进度轴 content_width 对齐）。
     * 注意不要用 gtk_widget_set_size_request，因为 height=-1 会把
     * .ui 里的 height-request=32 清掉，导致 bar 高度退化为 0。 */
    if (bar)
        gtk_drawing_area_set_content_width(bar, bar_width_px);

    /* 控制器 */
    TrackRow *tr = g_new0(TrackRow, 1);
    tr->album         = album;
    tr->axis          = axis;
    tr->track_idx     = track_idx;
    tr->bar           = bar;
    tr->drag_pair_idx = -1;
    tr->selected_pair_idx  = -1;
    tr->candidate_pair_idx = -1;

    if (bar) {
        g_object_add_weak_pointer(G_OBJECT(bar), (gpointer *)&tr->bar);
        gtk_drawing_area_set_draw_func(bar, on_bar_draw, tr, NULL);

        GtkGesture *drag = gtk_gesture_drag_new();
        g_signal_connect(drag, "drag-begin",  G_CALLBACK(on_drag_begin),  tr);
        g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), tr);
        g_signal_connect(drag, "drag-end",    G_CALLBACK(on_drag_end),    tr);
        gtk_widget_add_controller(GTK_WIDGET(bar),
                                   GTK_EVENT_CONTROLLER(drag));
    }

    /* bar 销毁时自动释放控制器 */
    g_object_set_data_full(G_OBJECT(bar), "track-row",
                            tr, track_row_destroy);

    /* bar 在 builder 中被强持（1 ref）。补上一份引用，builder 释放后
     * bar 的引用计数仍为 1（我们这份，transfer-full 交给调用方）。 */
    g_object_ref(bar);
    g_object_unref(b);
    return GTK_WIDGET(bar);
}
/* ─── 选中态 / 单击回调 setter ──────────────────────────── */

static TrackRow *
track_row_from_widget(GtkWidget *bar)
{
    if (!GTK_IS_WIDGET(bar)) return NULL;
    return (TrackRow *)g_object_get_data(G_OBJECT(bar), "track-row");
}

void
track_row_set_pair_selected_cb(GtkWidget *bar,
                                TrackPairSelectedFn cb,
                                gpointer user_data)
{
    TrackRow *tr = track_row_from_widget(bar);
    if (!tr) return;
    tr->pair_selected_cb   = cb;
    tr->pair_selected_data = user_data;
}

void
track_row_set_selected_pair(GtkWidget *bar, int pair_idx)
{
    TrackRow *tr = track_row_from_widget(bar);
    if (!tr) return;
    Track *t = track_row_get_track(tr);
    int n = (t && t->pairs) ? (int)t->pairs->len : 0;
    if (pair_idx < 0 || pair_idx >= n) pair_idx = -1;
    if (tr->selected_pair_idx != pair_idx) {
        tr->selected_pair_idx = pair_idx;
        if (tr->bar) gtk_widget_queue_draw(GTK_WIDGET(tr->bar));
    }
}
