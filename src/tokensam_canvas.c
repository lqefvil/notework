/**
 * tokensam_canvas.c — DAG 画布（GtkDrawingArea + Cairo + Pango 自绘）
 *
 * 视觉风格（深色主题）：
 *   - 背景：深蓝黑 #0E1D2E
 *   - 节点：透明圆 + 白色描边 + 白色编号；
 *           初始节点（#0）        → 蓝色描边
 *           last 节点 / clone 节点 → 紫色描边 + 半透明紫填充
 *           hover 节点             → 青色描边
 *   - trans 边：手挑 7 色高对比调色板，按 class_id 散列
 *   - 边标签：单色文字，无背景框，颜色随边
 *   - suffix link：浅紫色半透明虚线（可开关）
 *
 * 中文文本一律使用 Pango Cairo 渲染。
 */
#include <adwaita.h>
#include "tokensam.h"

#include <math.h>
#include <pango/pangocairo.h>

#define NODE_R       20.0
#define MARGIN_X     50.0
#define MARGIN_Y     50.0
#define ARROW_LEN    12.0
#define ARROW_W       7.0

/* 深色主题配色 */
#define BG_R   0.055
#define BG_G   0.114
#define BG_B   0.180

typedef struct {
    GtkDrawingArea  *area;
    const Sam       *sam;
    const Lexicon   *lex;
    SamLayout       *layout;
    gboolean         show_suffix_link;
    int              hover_node;     /* -1: none */

    /* —— 交互状态 —— */
    double           user_zoom;      /* 用户额外缩放系数（在自适应缩放之上）；默认 1.0 */
    double           pan_x, pan_y;   /* 屏幕像素的平移偏移；默认 (0,0) */
    int              selected_node;  /* -1: none */
    int              selected_edge;  /* -1: none；为 layout->edges 索引 */

    /* 拖动累计（drag-update 给的是相对开始点的偏移） */
    double           drag_anchor_x, drag_anchor_y;

    /* 最近一次指针在 widget 内的位置（供 scroll 缩放锚点用） */
    double           last_ptr_x, last_ptr_y;
    gboolean         has_ptr;

    /* —— 异步布局状态 —— */
    int              gen_seq;        /* 主线程：每次 set_data 自增 */
    int              gen_displayed;  /* 当前 layout 对应的 generation */
    gboolean         computing;      /* 是否有 worker 在跑 */
    int              compute_n;      /* 当前任务节点数（banner 用） */
} Canvas;

/* 全局：dot 引擎不并发安全，所有布局任务必须串行 */
static GMutex       g_dot_mutex;
static GThreadPool *g_layout_pool = NULL;
static GOnce        g_layout_pool_once = G_ONCE_INIT;

typedef struct {
    Sam        *snapshot;     /* worker 释放 */
    int         generation;
    GtkWidget  *area;         /* g_object_ref 持有，回调中 unref */
    SamLayout  *result;       /* worker 填充，主线程接管 */
    int         n_for_log;
} LayoutJob;

static void canvas_free(gpointer p) {
    Canvas *c = (Canvas *)p;
    if (!c) return;
    if (c->layout) sam_layout_free(c->layout);
    g_free(c);
}

static Canvas *canvas_get(GtkWidget *w) {
    if (!w || !G_IS_OBJECT(w)) return NULL;
    return (Canvas *)g_object_get_data(G_OBJECT(w), "tokensam-canvas");
}

/* ─── Pango 渲染辅助 ─────────────────────────────────────────── */

static PangoLayout *make_layout(cairo_t *cr, const char *text,
                                double font_size, gboolean bold,
                                int *out_w, int *out_h) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text, -1);
    char desc_buf[64];
    g_snprintf(desc_buf, sizeof desc_buf, "Sans %s%g",
               bold ? "Bold " : "", font_size);
    PangoFontDescription *desc = pango_font_description_from_string(desc_buf);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    int pw = 0, ph = 0;
    pango_layout_get_pixel_size(layout, &pw, &ph);
    if (out_w) *out_w = pw;
    if (out_h) *out_h = ph;
    return layout;
}

/* 手挑高对比调色板，深底上的鲜艳色（含微调饱和度避免刺眼）。
 * 顺序：蓝、橙、绿、粉、青、黄、紫 */
static const double EDGE_PALETTE[][3] = {
    {0.290, 0.620, 1.000},   /* #4A9EFF */
    {1.000, 0.624, 0.259},   /* #FF9F43 */
    {0.290, 0.871, 0.502},   /* #4ADE80 */
    {0.957, 0.447, 0.714},   /* #F472B6 */
    {0.133, 0.827, 0.933},   /* #22D3EE */
    {0.984, 0.749, 0.141},   /* #FBBF24 */
    {0.753, 0.518, 0.988},   /* #C084FC */
};
#define EDGE_PALETTE_N 7

static void color_for_class(int cid, double *r, double *g, double *b) {
    if (cid < 0) cid = 0;
    /* 简单散列，让相近 class_id 也能取到差异较大的色 */
    guint idx = ((guint)cid * 5u + 2u) % EDGE_PALETTE_N;
    *r = EDGE_PALETTE[idx][0];
    *g = EDGE_PALETTE[idx][1];
    *b = EDGE_PALETTE[idx][2];
}

/* 在 (tx, ty) 处画指向 (tx, ty)、方向为 (dx, dy) 的实心箭头 */
static void draw_arrow_head_at(cairo_t *cr, double tx, double ty,
                               double dx, double dy) {
    double L = hypot(dx, dy);
    if (L < 1e-3) return;
    double ux = dx / L, uy = dy / L;
    double px = -uy, py = ux;
    double bx = tx - ux * ARROW_LEN, by = ty - uy * ARROW_LEN;
    cairo_move_to(cr, tx, ty);
    cairo_line_to(cr, bx + px * ARROW_W, by + py * ARROW_W);
    cairo_line_to(cr, bx - px * ARROW_W, by - py * ARROW_W);
    cairo_close_path(cr);
    cairo_fill(cr);
}

/* 把样条点序列转化为 cairo 路径：moveTo(p0) + curveTo 每三个。
 * 若提供 override_p0 != NULL，则用其替代第一个控制点（用于把起点投影到真实节点圆环上）。 */
static void path_spline(cairo_t *cr, const double *pts, int n_pts,
                        const double *override_p0) {
    if (n_pts < 2) return;
    double x0 = override_p0 ? override_p0[0] : pts[0];
    double y0 = override_p0 ? override_p0[1] : pts[1];
    cairo_move_to(cr, x0, y0);
    if (n_pts == 2) {
        cairo_line_to(cr, pts[2], pts[3]);
        return;
    }
    int n_seg = (n_pts - 1) / 3;
    if (n_seg <= 0) {
        cairo_line_to(cr, pts[2*(n_pts-1)], pts[2*(n_pts-1)+1]);
        return;
    }
    for (int s = 0; s < n_seg; s++) {
        int base = s * 3;
        cairo_curve_to(cr,
                       pts[2*(base+1)],   pts[2*(base+1)+1],
                       pts[2*(base+2)],   pts[2*(base+2)+1],
                       pts[2*(base+3)],   pts[2*(base+3)+1]);
    }
}

/* 把 (px, py) 投影到以 (cx, cy) 为圆心、半径 r 的圆周上，沿原方向。
 * 若 (px, py) ≈ (cx, cy) 则不动（极少出现）。 */
static void project_to_ring(double cx, double cy, double r,
                            double px, double py,
                            double *ox, double *oy) {
    double dx = px - cx, dy = py - cy;
    double L = hypot(dx, dy);
    if (L < 1e-6) { *ox = px; *oy = py; return; }
    *ox = cx + dx / L * r;
    *oy = cy + dy / L * r;
}

/* 取样条「最后一小段」的方向 */
static void spline_end_dir(const double *pts, int n_pts,
                           double tx, double ty,
                           double *dx, double *dy) {
    if (n_pts >= 2) {
        double px = pts[2*(n_pts-1)];
        double py = pts[2*(n_pts-1) + 1];
        *dx = tx - px;
        *dy = ty - py;
        if (hypot(*dx, *dy) < 1e-3 && n_pts >= 3) {
            px = pts[2*(n_pts-2)];
            py = pts[2*(n_pts-2) + 1];
            *dx = tx - px;
            *dy = ty - py;
        }
    } else {
        *dx = 1; *dy = 0;
    }
}

/* 在 (mx, my) 沿单位法线 (-dy, dx) 偏移 offset，得到标签锚点 */
static void label_offset(double mx, double my,
                         double dx, double dy, double offset,
                         double *out_x, double *out_y) {
    double L = hypot(dx, dy);
    if (L < 1e-3) { *out_x = mx; *out_y = my - offset; return; }
    /* 法线方向（左手），y 在屏幕坐标系下「往上」对应 -y */
    double nx = -dy / L, ny = dx / L;
    /* 让标签倾向于在边的「上方」：若 ny>0 则反向 */
    if (ny > 0) { nx = -nx; ny = -ny; }
    *out_x = mx + nx * offset;
    *out_y = my + ny * offset;
}

/* ─── 视口变换 ────────────────────────────────────────────────
 * 逻辑坐标（dot 翻转后） → 屏幕像素：
 *   sx = ox + lx * scale
 *   sy = oy + ly * scale
 * 其中 scale = base_scale * user_zoom，
 *      ox/oy 包含「居中偏移 + 内边距 + 用户拖动 pan」。 */
typedef struct {
    double scale;
    double ox, oy;
} ViewXf;

static ViewXf compute_xf(const Canvas *c, int W, int H) {
    ViewXf xf = {1.0, 0.0, 0.0};
    if (!c || !c->layout) return xf;
    double need_w = c->layout->width  + 2*MARGIN_X;
    double need_h = c->layout->height + 2*MARGIN_Y;
    double sx = (double)W / need_w, sy = (double)H / need_h;
    double base = (sx < sy) ? sx : sy;
    if (base > 1.5) base = 1.5;
    if (base < 0.1) base = 0.1;
    double zoom = (c->user_zoom > 0.05) ? c->user_zoom : 0.05;
    if (zoom > 50.0) zoom = 50.0;
    xf.scale = base * zoom;
    xf.ox = ((double)W - need_w * xf.scale) / 2.0 + MARGIN_X * xf.scale + c->pan_x;
    xf.oy = ((double)H - need_h * xf.scale) / 2.0 + MARGIN_Y * xf.scale + c->pan_y;
    return xf;
}

static inline void screen_to_canvas(const ViewXf *xf, double sx, double sy,
                                    double *lx, double *ly) {
    *lx = (sx - xf->ox) / xf->scale;
    *ly = (sy - xf->oy) / xf->scale;
}

/* 点 (px,py) 到线段 (ax,ay)-(bx,by) 的最近距离 */
static double dist_point_segment(double px, double py,
                                 double ax, double ay,
                                 double bx, double by) {
    double dx = bx - ax, dy = by - ay;
    double L2 = dx*dx + dy*dy;
    double t = (L2 > 1e-9) ? ((px - ax)*dx + (py - ay)*dy) / L2 : 0.0;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    double cx = ax + t*dx, cy = ay + t*dy;
    return hypot(px - cx, py - cy);
}

/* 命中节点：返回索引，未命中 -1 */
static int hit_node(const Canvas *c, double lx, double ly) {
    if (!c->layout) return -1;
    double r2 = NODE_R * NODE_R;
    for (int i = 0; i < c->layout->n; i++) {
        double dx = lx - c->layout->pos[i].x;
        double dy = ly - c->layout->pos[i].y;
        if (dx*dx + dy*dy <= r2) return i;
    }
    return -1;
}

/* 命中边：返回 layout->edges 索引；未命中 -1。
 * trans 边按样条点折线最近距离；suffix link 按中心-中心直线段最近距离。
 * 阈值 hit_tol（逻辑坐标系下）。show_suffix=FALSE 时跳过 suffix 边。 */
static int hit_edge(const Canvas *c, double lx, double ly, double hit_tol) {
    if (!c->layout) return -1;
    int best = -1;
    double best_d = hit_tol;
    for (int i = 0; i < c->layout->n_edges; i++) {
        EdgeGeom *eg = &c->layout->edges[i];
        if (eg->is_suffix && !c->show_suffix_link) continue;
        if (eg->is_suffix) {
            if (eg->u == eg->v) continue;
            double ax = c->layout->pos[eg->u].x;
            double ay = c->layout->pos[eg->u].y;
            double bx = c->layout->pos[eg->v].x;
            double by = c->layout->pos[eg->v].y;
            double d = dist_point_segment(lx, ly, ax, ay, bx, by);
            if (d < best_d) { best_d = d; best = i; }
        } else {
            for (int k = 0; k + 1 < eg->n_pts; k++) {
                double ax = eg->pts[2*k],     ay = eg->pts[2*k+1];
                double bx = eg->pts[2*(k+1)], by = eg->pts[2*(k+1)+1];
                double d = dist_point_segment(lx, ly, ax, ay, bx, by);
                if (d < best_d) { best_d = d; best = i; }
            }
            /* 末段：list[n_pts-1] → ep */
            double ax = eg->pts[2*(eg->n_pts-1)];
            double ay = eg->pts[2*(eg->n_pts-1)+1];
            double d = dist_point_segment(lx, ly, ax, ay, eg->ex, eg->ey);
            if (d < best_d) { best_d = d; best = i; }
        }
    }
    return best;
}

/* 节点 i 是否在选中边的端点上 */
static gboolean node_is_endpoint(const Canvas *c, int i) {
    if (c->selected_edge < 0 || !c->layout) return FALSE;
    EdgeGeom *eg = &c->layout->edges[c->selected_edge];
    return (eg->u == i || eg->v == i);
}

/* 选中节点时，边 e 是否与之邻接（含 suffix） */
static gboolean edge_touches_selected_node(const Canvas *c, const EdgeGeom *eg) {
    if (c->selected_node < 0) return FALSE;
    return (eg->u == c->selected_node || eg->v == c->selected_node);
}

/* 等待期 banner：顶部居中，半透明深色底 + 白字 */
static void draw_busy_banner(cairo_t *cr, int W, int compute_n) {
    char buf[128];
    g_snprintf(buf, sizeof buf, "布局计算中…  %d 节点", compute_n);
    int tw, th;
    PangoLayout *L = make_layout(cr, buf, 12, TRUE, &tw, &th);
    int pad_x = 14, pad_y = 6;
    int bw = tw + pad_x*2, bh = th + pad_y*2;
    int bx = (W - bw)/2, by = 12;
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.65);
    cairo_rectangle(cr, bx, by, bw, bh);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.55, 0.78, 1.0, 0.9);
    cairo_set_line_width(cr, 1.2);
    cairo_rectangle(cr, bx+0.5, by+0.5, bw-1, bh-1);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_move_to(cr, bx + pad_x, by + pad_y);
    pango_cairo_show_layout(cr, L);
    g_object_unref(L);
}

static void draw_func(GtkDrawingArea *area, cairo_t *cr,
                      int width, int height, gpointer user_data) {
    (void)area;
    Canvas *c = (Canvas *)user_data;

    /* 深色背景 */
    cairo_set_source_rgb(cr, BG_R, BG_G, BG_B);
    cairo_paint(cr);

    if (!c || !c->sam || !c->layout || c->layout->n == 0) {
        if (c && c->computing) {
            const char *msg = "首次布局计算中，请稍候…";
            int tw, th;
            PangoLayout *L = make_layout(cr, msg, 12, FALSE, &tw, &th);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.65);
            cairo_move_to(cr, (width - tw) / 2.0, (height - th) / 2.0);
            pango_cairo_show_layout(cr, L);
            g_object_unref(L);
            draw_busy_banner(cr, width, c->compute_n);
            return;
        }
        const char *msg = "在左侧编辑词典并输入文本，点击「构建 SAM」查看 DAG。";
        int tw, th;
        PangoLayout *L = make_layout(cr, msg, 12, FALSE, &tw, &th);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
        cairo_move_to(cr, (width - tw) / 2.0, (height - th) / 2.0);
        pango_cairo_show_layout(cr, L);
        g_object_unref(L);
        return;
    }

    /* 居中并整体缩放，确保布局完整可见 */
    int W = width, H = height;
    ViewXf xf = compute_xf(c, W, H);
    double scale = xf.scale;

    cairo_save(cr);
    cairo_translate(cr, xf.ox, xf.oy);
    cairo_scale(cr, scale, scale);

    gboolean has_sel = (c->selected_node >= 0 || c->selected_edge >= 0);

    /* 1) suffix link（虚线） — 优先画，作为底层
     * 注：dot 对 constraint=false 边输出的样条端点不可靠（可能落入节点内部），
     * 所以 suffix link 直接用「源节点中心 → 目标节点中心」的直线，
     * 两端投影到真实圆环上，确保始终精确连接。 */
    if (c->show_suffix_link) {
        double dashes[] = { 5.0, 5.0 };
        cairo_save(cr);
        cairo_set_dash(cr, dashes, 2, 0.0);
        for (int i = 0; i < c->layout->n_edges; i++) {
            EdgeGeom *eg = &c->layout->edges[i];
            if (!eg->is_suffix) continue;
            if (eg->u == eg->v) continue;   /* 自环不画 */
            gboolean is_sel    = (i == c->selected_edge);
            gboolean is_neigh  = edge_touches_selected_node(c, eg);
            gboolean dim       = has_sel && !is_sel && !is_neigh;
            double alpha = dim ? 0.12 : (is_sel ? 0.95 : 0.55);
            cairo_set_source_rgba(cr, 0.66, 0.69, 0.85, alpha);
            cairo_set_line_width(cr, is_sel ? 2.4 : 1.0);
            double ux = c->layout->pos[eg->u].x;
            double uy = c->layout->pos[eg->u].y;
            double vx = c->layout->pos[eg->v].x;
            double vy = c->layout->pos[eg->v].y;
            double ssx, ssy, ttx, tty;
            project_to_ring(ux, uy, NODE_R, vx, vy, &ssx, &ssy);
            project_to_ring(vx, vy, NODE_R, ux, uy, &ttx, &tty);
            cairo_move_to(cr, ssx, ssy);
            cairo_line_to(cr, ttx, tty);
            cairo_stroke(cr);
        }
        cairo_restore(cr);
    }

    /* 2) trans 边 + 边标签
     * 不对端点做投影修正——dot 节点尺寸已精确匹配 Cairo 渲染尺寸(0.556inch=NODE_R*2/72)，
     * dot 原始的 spline 起点在源节点边界，ep 在目标节点边界。
     * 样条 list[size-1] 到 ep 的间隙正好由箭头三角形覆盖。 */
    for (int i = 0; i < c->layout->n_edges; i++) {
        EdgeGeom *eg = &c->layout->edges[i];
        if (eg->is_suffix) continue;
        gboolean is_sel   = (i == c->selected_edge);
        gboolean is_neigh = edge_touches_selected_node(c, eg);
        gboolean dim      = has_sel && !is_sel && !is_neigh;
        double r,g,b;
        color_for_class(eg->class_id, &r,&g,&b);
        double alpha = dim ? 0.18 : 1.0;
        cairo_set_source_rgba(cr, r, g, b, alpha);
        cairo_set_line_width(cr, is_sel ? 3.4 : (is_neigh ? 2.6 : 1.8));

        /* 直接使用 dot 原始坐标画样条 */
        cairo_new_sub_path(cr);
        path_spline(cr, eg->pts, eg->n_pts, NULL);
        cairo_stroke(cr);

        /* 箭头：从最后一小段方向指向 ep（目标节点边界） */
        double dx, dy;
        spline_end_dir(eg->pts, eg->n_pts, eg->ex, eg->ey, &dx, &dy);
        cairo_set_source_rgba(cr, r, g, b, alpha);
        draw_arrow_head_at(cr, eg->ex, eg->ey, dx, dy);

        /* 边标签：等价类内全部代表文本，单色无背景 */
        if (c->lex) {
            char *label = lexicon_class_label(c->lex, eg->class_id);
            if (label) {
                int tw, th;
                PangoLayout *L = make_layout(cr, label, 11, TRUE, &tw, &th);
                /* 标签锚点：在样条中点法线方向偏移，倾向边的上方 */
                double lx, ly;
                label_offset(eg->label_x, eg->label_y,
                             dx, dy, 11.0, &lx, &ly);
                cairo_set_source_rgba(cr, r, g, b, dim ? 0.30 : 1.0);
                cairo_move_to(cr, lx - tw/2.0, ly - th/2.0);
                pango_cairo_show_layout(cr, L);
                g_object_unref(L);
                g_free(label);
            }
        }
    }

    /* 3) 节点 */
    for (int i = 0; i < c->layout->n; i++) {
        const SamNode *n = &g_array_index(c->sam->nodes, SamNode, i);
        double x = c->layout->pos[i].x, y = c->layout->pos[i].y;
        gboolean is_init  = (i == 0);
        gboolean is_last  = (i == c->sam->last);
        gboolean is_clone = n->is_clone;
        gboolean is_hover = (i == c->hover_node);
        gboolean is_sel   = (i == c->selected_node);
        gboolean is_neigh = (c->selected_node >= 0 &&
                             (i == c->selected_node)) ||
                            node_is_endpoint(c, i);
        /* 选中节点时，邻居 = 与之有 trans/suffix 边相连的另一端 */
        if (!is_neigh && c->selected_node >= 0) {
            for (int k = 0; k < c->layout->n_edges; k++) {
                EdgeGeom *eg = &c->layout->edges[k];
                if (eg->is_suffix && !c->show_suffix_link) continue;
                if ((eg->u == c->selected_node && eg->v == i) ||
                    (eg->v == c->selected_node && eg->u == i)) {
                    is_neigh = TRUE; break;
                }
            }
        }
        gboolean dim = has_sel && !is_sel && !is_neigh;

        /* 填充：仅 clone 用半透明紫色（标识 SAM 算法的 clone 状态）；
         * 其余节点（含 last）几乎透明，让深色背景透出。 */
        if (is_clone) {
            cairo_set_source_rgba(cr, 0.471, 0.341, 0.616, dim ? 0.12 : 0.40);
        } else {
            cairo_set_source_rgba(cr, 1, 1, 1, dim ? 0.02 : 0.04);
        }
        /* ★ 关键：必须用 cairo_new_sub_path 断开前一次 pango_cairo_show_layout
         * 留下的 current point，否则 cairo_arc 会隐式从该点画一条直线连到圆弧起点，
         * 产生随机方向的白色/紫色"幽灵线"。 */
        cairo_new_sub_path(cr);
        cairo_arc(cr, x, y, NODE_R, 0, 2*G_PI);
        cairo_fill_preserve(cr);

        /* 描边色优先级：selected > hover > 初始（蓝） > clone（紫） > 默认（白） */
        double sa = dim ? 0.30 : 1.0;
        if (is_sel) {
            cairo_set_source_rgba(cr, 1.000, 0.847, 0.243, 1.0);   /* 选中：金黄 */
        } else if (is_hover) {
            cairo_set_source_rgba(cr, 0.36, 0.89, 0.93, sa);
        } else if (is_init) {
            cairo_set_source_rgba(cr, 0.36, 0.64, 0.96, sa);
        } else if (is_clone) {
            cairo_set_source_rgba(cr, 0.753, 0.518, 0.988, sa);
        } else {
            cairo_set_source_rgba(cr, 1, 1, 1, dim ? 0.30 : 0.85);
        }
        cairo_set_line_width(cr, is_sel ? 3.4 :
                                  (is_hover ? 2.6 : (is_last ? 2.6 : 2.0)));
        cairo_stroke(cr);

        /* 节点编号：白色 Bold */
        char buf[32];
        g_snprintf(buf, sizeof buf, "%d", i);
        int tw, th;
        PangoLayout *L = make_layout(cr, buf, 13, TRUE, &tw, &th);
        cairo_set_source_rgba(cr, 1, 1, 1, dim ? 0.35 : 0.95);
        cairo_move_to(cr, x - tw/2.0, y - th/2.0);
        pango_cairo_show_layout(cr, L);
        g_object_unref(L);
    }

    /* 4) 信息提示：选中 > hover */
    int info_node = (c->selected_node >= 0) ? c->selected_node : c->hover_node;
    if (info_node >= 0 && info_node < c->layout->n) {
        int i = info_node;
        const SamNode *n = &g_array_index(c->sam->nodes, SamNode, i);
        char buf[200];
        g_snprintf(buf, sizeof buf,
                   "节点 #%d   len=%d   link=%d   出度=%u%s%s",
                   i, n->len, n->link, g_hash_table_size(n->trans),
                   n->is_clone ? "   [clone]" : "",
                   (i == c->selected_node) ? "   [selected]" : "");
        int tw, th;
        PangoLayout *L = make_layout(cr, buf, 10, FALSE, &tw, &th);
        double bx = c->layout->pos[i].x + NODE_R + 6;
        double by = c->layout->pos[i].y - NODE_R;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
        cairo_rectangle(cr, bx, by, tw + 14, th + 8);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.10, 0.10, 0.15);
        cairo_move_to(cr, bx + 7, by + 4);
        pango_cairo_show_layout(cr, L);
        g_object_unref(L);
    } else if (c->selected_edge >= 0 && c->selected_edge < c->layout->n_edges) {
        EdgeGeom *eg = &c->layout->edges[c->selected_edge];
        char *label = c->lex ? lexicon_class_label(c->lex, eg->class_id) : NULL;
        char buf[256];
        if (eg->is_suffix) {
            g_snprintf(buf, sizeof buf,
                       "suffix link   %d → %d", eg->u, eg->v);
        } else {
            g_snprintf(buf, sizeof buf,
                       "trans   %d → %d   class=%d   token=%s",
                       eg->u, eg->v, eg->class_id, label ? label : "?");
        }
        if (label) g_free(label);
        int tw, th;
        PangoLayout *L = make_layout(cr, buf, 10, FALSE, &tw, &th);
        double bx = eg->label_x;
        double by = eg->label_y - 18;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
        cairo_rectangle(cr, bx, by, tw + 14, th + 8);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.10, 0.10, 0.15);
        cairo_move_to(cr, bx + 7, by + 4);
        pango_cairo_show_layout(cr, L);
        g_object_unref(L);
    }

    cairo_restore(cr);

    /* worker 正在算新布局：在旧图上叠 banner，告诉用户后台还在算 */
    if (c->computing) draw_busy_banner(cr, width, c->compute_n);
}

/* 鼠标移动：找到命中节点（圆内）作为 hover */
static void on_motion(GtkEventControllerMotion *ctl, double x, double y,
                      gpointer user_data) {
    (void)ctl;
    GtkWidget *w = GTK_WIDGET(user_data);
    Canvas *c = canvas_get(w);
    if (!c || !c->layout || c->layout->n == 0) return;

    int W = gtk_widget_get_width(w), H = gtk_widget_get_height(w);
    ViewXf xf = compute_xf(c, W, H);
    double lx, ly;
    screen_to_canvas(&xf, x, y, &lx, &ly);

    c->last_ptr_x = x; c->last_ptr_y = y; c->has_ptr = TRUE;

    int hit = hit_node(c, lx, ly);
    if (hit != c->hover_node) {
        c->hover_node = hit;
        gtk_widget_queue_draw(w);
    }
}

static void on_leave(GtkEventControllerMotion *ctl, gpointer user_data) {
    (void)ctl;
    GtkWidget *w = GTK_WIDGET(user_data);
    Canvas *c = canvas_get(w);
    if (!c) return;
    if (c->hover_node != -1) { c->hover_node = -1; gtk_widget_queue_draw(w); }
}

/* 鼠标点击：优先节点；否则就近边；否则清空选中 */
static void on_click_pressed(GtkGestureClick *g, int n_press,
                             double x, double y, gpointer user_data) {
    (void)g; (void)n_press;
    GtkWidget *w = GTK_WIDGET(user_data);
    Canvas *c = canvas_get(w);
    if (!c || !c->layout || c->layout->n == 0) return;

    int W = gtk_widget_get_width(w), H = gtk_widget_get_height(w);
    ViewXf xf = compute_xf(c, W, H);
    double lx, ly;
    screen_to_canvas(&xf, x, y, &lx, &ly);

    int hn = hit_node(c, lx, ly);
    if (hn >= 0) {
        if (c->selected_node == hn && c->selected_edge < 0) {
            c->selected_node = -1;   /* 再次点击同一节点：取消选择 */
        } else {
            c->selected_node = hn;
            c->selected_edge = -1;
        }
        gtk_widget_queue_draw(w);
        return;
    }
    /* 边的命中阈值：8px / scale */
    double tol = 8.0 / xf.scale;
    int he = hit_edge(c, lx, ly, tol);
    if (he >= 0) {
        if (c->selected_edge == he) {
            c->selected_edge = -1;
        } else {
            c->selected_edge = he;
            c->selected_node = -1;
        }
        gtk_widget_queue_draw(w);
        return;
    }
    /* 点空白：清除选中 */
    if (c->selected_node >= 0 || c->selected_edge >= 0) {
        c->selected_node = -1;
        c->selected_edge = -1;
        gtk_widget_queue_draw(w);
    }
}

/* 拖动平移（左键） */
static void on_drag_begin(GtkGestureDrag *g, double sx, double sy,
                          gpointer user_data) {
    (void)g; (void)sx; (void)sy;
    GtkWidget *w = GTK_WIDGET(user_data);
    Canvas *c = canvas_get(w);
    if (!c) return;
    c->drag_anchor_x = c->pan_x;
    c->drag_anchor_y = c->pan_y;
}

static void on_drag_update(GtkGestureDrag *g, double dx, double dy,
                           gpointer user_data) {
    (void)g;
    GtkWidget *w = GTK_WIDGET(user_data);
    Canvas *c = canvas_get(w);
    if (!c) return;
    c->pan_x = c->drag_anchor_x + dx;
    c->pan_y = c->drag_anchor_y + dy;
    gtk_widget_queue_draw(w);
}

/* 滚轮缩放：以鼠标光标为锚点，保持光标下逻辑点不动 */
static gboolean on_scroll(GtkEventControllerScroll *ctl,
                          double dx, double dy, gpointer user_data) {
    (void)ctl; (void)dx;
    GtkWidget *w = GTK_WIDGET(user_data);
    Canvas *c = canvas_get(w);
    if (!c || !c->layout) return FALSE;

    int W = gtk_widget_get_width(w), H = gtk_widget_get_height(w);
    /* 锚点：最近一次 motion 记录的指针位置；若无则取中心 */
    double mx = c->has_ptr ? c->last_ptr_x : W / 2.0;
    double my = c->has_ptr ? c->last_ptr_y : H / 2.0;

    ViewXf xf0 = compute_xf(c, W, H);
    double lx, ly;
    screen_to_canvas(&xf0, mx, my, &lx, &ly);

    /* dy>0：向下滚 → 缩小；dy<0：向上 → 放大 */
    double factor = (dy > 0) ? (1.0 / 1.20) : 1.20;
    double new_zoom = c->user_zoom * factor;
    if (new_zoom < 0.05) new_zoom = 0.05;
    if (new_zoom > 50.0) new_zoom = 50.0;
    if (new_zoom == c->user_zoom) return TRUE;
    c->user_zoom = new_zoom;

    /* 调整 pan，保持 (mx,my) 处的逻辑点不变 */
    ViewXf xf1 = compute_xf(c, W, H);
    double sx_now = xf1.ox + lx * xf1.scale;
    double sy_now = xf1.oy + ly * xf1.scale;
    c->pan_x += (mx - sx_now);
    c->pan_y += (my - sy_now);

    gtk_widget_queue_draw(w);
    return TRUE;
}

GtkWidget *tokensam_canvas_new(void) {
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);

    Canvas *c = g_new0(Canvas, 1);
    c->area = GTK_DRAWING_AREA(area);
    c->sam = NULL; c->lex = NULL;
    c->layout = NULL;
    c->show_suffix_link = FALSE;
    c->hover_node = -1;
    c->user_zoom = 1.0;
    c->pan_x = c->pan_y = 0;
    c->selected_node = -1;
    c->selected_edge = -1;

    g_object_set_data_full(G_OBJECT(area), "tokensam-canvas", c, canvas_free);

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area),
                                   draw_func, c, NULL);

    /* hover */
    GtkEventController *mc = gtk_event_controller_motion_new();
    g_signal_connect(mc, "motion", G_CALLBACK(on_motion), area);
    g_signal_connect(mc, "leave",  G_CALLBACK(on_leave),  area);
    gtk_widget_add_controller(area, mc);

    /* 左键点击：选中节点/边 */
    GtkGesture *gc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), GDK_BUTTON_PRIMARY);
    g_signal_connect(gc, "pressed", G_CALLBACK(on_click_pressed), area);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(gc));

    /* 左键拖动：平移画布 */
    GtkGesture *dg = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dg), GDK_BUTTON_PRIMARY);
    g_signal_connect(dg, "drag-begin",  G_CALLBACK(on_drag_begin),  area);
    g_signal_connect(dg, "drag-update", G_CALLBACK(on_drag_update), area);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(dg));

    /* 滚轮：缩放 */
    GtkEventController *sc = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(sc, "scroll", G_CALLBACK(on_scroll), area);
    gtk_widget_add_controller(area, sc);

    return area;
}

/* ─── 异步布局：worker / 回调 / 提交 ─────────────────────────── */

static gboolean on_layout_ready(gpointer user_data);

static void layout_worker(gpointer data, gpointer pool_user) {
    (void)pool_user;
    LayoutJob *job = (LayoutJob *)data;
    /* dot/cgraph 不并发安全：所有布局任务必须串行 */
    g_mutex_lock(&g_dot_mutex);
    job->result = sam_layout_compute(job->snapshot, 90.0, 100.0);
    g_mutex_unlock(&g_dot_mutex);
    /* snapshot 仅 worker 私有，用完即可释放 */
    if (job->snapshot) { sam_free(job->snapshot); job->snapshot = NULL; }
    /* 回主线程 */
    g_idle_add(on_layout_ready, job);
}

static gpointer init_layout_pool(gpointer _) {
    (void)_;
    g_mutex_init(&g_dot_mutex);
    /* 单 worker：dot 本身串行了，多 worker 也只能等锁 */
    g_layout_pool = g_thread_pool_new(layout_worker, NULL,
                                      1, FALSE, NULL);
    return NULL;
}

static gboolean on_layout_ready(gpointer user_data) {
    LayoutJob *job = (LayoutJob *)user_data;
    GtkWidget *w = job->area;

    Canvas *c = canvas_get(w);
    if (!c) {
        /* widget 已死，丢弃结果 */
        if (job->result) sam_layout_free(job->result);
    } else if (job->generation != c->gen_seq) {
        /* 已被新任务覆盖，丢弃旧结果 */
        if (job->result) sam_layout_free(job->result);
    } else {
        /* 接管新 layout */
        if (c->layout) sam_layout_free(c->layout);
        c->layout = job->result;
        job->result = NULL;
        c->gen_displayed = job->generation;
        c->computing = FALSE;
        /* 切到新 layout 时复位视图 */
        c->user_zoom = 1.0;
        c->pan_x = c->pan_y = 0;
        c->hover_node = -1;
        c->selected_node = -1;
        c->selected_edge = -1;
        gtk_widget_queue_draw(w);
    }

    /* 释放 widget 引用与 job */
    g_object_unref(w);
    g_free(job);
    return G_SOURCE_REMOVE;
}

void tokensam_canvas_set_data(GtkWidget *w, const Sam *sam, const Lexicon *lex) {
    Canvas *c = canvas_get(w);
    if (!c) return;
    /* 保存当前 sam/lex 引用（hover/click 等仍可读旧引用；layout 异步替换） */
    c->sam = sam;
    c->lex = lex;

    /* 自增 generation：使任何在途旧任务的回调到达时被识别为过期并丢弃 */
    c->gen_seq += 1;

    if (!sam || sam_node_count(sam) == 0) {
        /* 直接清空：不需要 dot */
        if (c->layout) { sam_layout_free(c->layout); c->layout = NULL; }
        c->gen_displayed = c->gen_seq;
        c->computing = FALSE;
        c->compute_n = 0;
        c->hover_node = -1;
        c->selected_node = -1;
        c->selected_edge = -1;
        c->user_zoom = 1.0;
        c->pan_x = c->pan_y = 0;
        gtk_widget_queue_draw(w);
        return;
    }

    /* 懒初始化线程池 */
    g_once(&g_layout_pool_once, init_layout_pool, NULL);

    /* 提交异步任务：拷贝 sam 给 worker，旧 layout 暂保留供等待期渲染 */
    LayoutJob *job = g_new0(LayoutJob, 1);
    job->snapshot   = sam_copy(sam);
    job->generation = c->gen_seq;
    job->area       = g_object_ref(w);  /* 保活到回调 */
    job->result     = NULL;
    job->n_for_log  = sam_node_count(sam);

    c->computing = TRUE;
    c->compute_n = job->n_for_log;

    g_thread_pool_push(g_layout_pool, job, NULL);
    gtk_widget_queue_draw(w);
}

void tokensam_canvas_set_show_suffix_link(GtkWidget *w, gboolean on) {
    Canvas *c = canvas_get(w);
    if (!c) return;
    if (c->show_suffix_link != on) {
        c->show_suffix_link = on;
        gtk_widget_queue_draw(w);
    }
}