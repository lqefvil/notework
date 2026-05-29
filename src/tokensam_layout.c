/**
 * tokensam_layout.c — DAG 布局（Graphviz dot 引擎）
 *
 * 流程：
 *   1) 把 Sam 翻译为 cgraph 有向图：每个 SAM 状态 → Agnode_t；
 *      trans 边正常加入；suffix link 边加 constraint=false，不参与 rank 计算。
 *   2) 调用 gvLayout(gvc, g, "dot")。
 *   3) 从 ND_coord 取节点坐标；从 ED_spl 取每条边的三阶贝塞尔控制点序列。
 *   4) 翻转 y 轴（dot 数学坐标系 y 向上，GTK Cairo y 向下），
 *      做平移使整个布局左上角在原点。
 *
 * 边标签锚点：取样条参数 t=0.5 处的点（一段或多段累计弧长中点近似）。
 * 这样不依赖 dot 自带的 label 度量，避免它的字体度量与 Pango 不一致。
 */
#include "tokensam.h"

#include <math.h>
#include <string.h>

#include <graphviz/cgraph.h>
#include <graphviz/gvc.h>

/* ─── 释放 ───────────────────────────────────────────────────── */

void sam_layout_free(SamLayout *layout) {
    if (!layout) return;
    g_free(layout->pos);
    g_free(layout->layer_of);
    if (layout->edges) {
        for (int i = 0; i < layout->n_edges; i++)
            g_free(layout->edges[i].pts);
        g_free(layout->edges);
    }
    g_free(layout);
}

/* ─── 工具：De Casteljau 求三阶贝塞尔在 t 处的点 ────────────── */

static void cubic_eval(const double *p0, const double *p1,
                       const double *p2, const double *p3,
                       double t, double *out_x, double *out_y) {
    double u = 1.0 - t;
    double b0 = u*u*u;
    double b1 = 3*u*u*t;
    double b2 = 3*u*t*t;
    double b3 = t*t*t;
    *out_x = b0*p0[0] + b1*p1[0] + b2*p2[0] + b3*p3[0];
    *out_y = b0*p0[1] + b1*p1[1] + b2*p2[1] + b3*p3[1];
}

/* 取样条的近似中点（用作标签锚点）。
 * pts 含 n_pts = 3k+1 个点（x,y 交替），分 k 段三阶贝塞尔。
 * 取第 mid 段（k/2）的 t=0.5 处。 */
static void spline_midpoint(const double *pts, int n_pts,
                            double *mx, double *my) {
    if (n_pts < 4) {
        if (n_pts <= 0) { *mx = 0; *my = 0; return; }
        int last = n_pts - 1;
        *mx = (pts[0] + pts[2*last]) * 0.5;
        *my = (pts[1] + pts[2*last+1]) * 0.5;
        return;
    }
    int n_seg = (n_pts - 1) / 3;
    if (n_seg <= 0) n_seg = 1;
    int mid = n_seg / 2;
    int base = mid * 3;
    cubic_eval(&pts[2*base],     &pts[2*(base+1)],
               &pts[2*(base+2)], &pts[2*(base+3)],
               0.5, mx, my);
}

/* ─── 主入口 ───────────────────────────────────────────────── */

typedef struct {
    Agedge_t *e;
    int       u, v;
    int       class_id;     /* -1 表示 suffix link */
    gboolean  is_suffix;
} EdgeRef;

/* 当节点数超过此阈值时，跳过 Graphviz dot 布局（兜底，避免 dot 跑几小时）。
 * 注意：异步化后 dot 不再阻塞主线程，所以阈值大幅放宽——只在节点数极大
 * 时才走 fallback 兜底。日常使用（数千节点）一律走 dot。 */
#define DOT_MAX_NODES   50000

/* 估算 SAM 总边数（trans + suffix）作为辅助阈值 */
static int sam_total_edge_count(const Sam *sam, int n) {
    int e = 0;
    for (int i = 0; i < n; i++) {
        GHashTable *t = g_array_index(sam->nodes, SamNode, i).trans;
        if (t) e += (int)g_hash_table_size(t);
    }
    /* suffix link：除根节点外每个节点最多一条 */
    for (int i = 1; i < n; i++) {
        if (g_array_index(sam->nodes, SamNode, i).link >= 0) e++;
    }
    return e;
}

/* 快速回退布局：
 *   - 节点 x 坐标 = layer * COL_W
 *   - 节点 y 坐标 = (layer 内序号) * ROW_H
 *   - trans 边：源节点 → 目标节点的直线（n_pts=2）
 *   - suffix 边：同上，标记 is_suffix=true
 * 不调用 graphviz，O(n + e) 直接出图。 */
static void fallback_layout(SamLayout *L, const Sam *sam, int n) {
    const double COL_W = 90.0;
    const double ROW_H = 70.0;

    /* 统计每层节点数，分配 y 序号 */
    int max_layer = L->max_layer;
    int *count_in_layer = g_new0(int, max_layer + 1);
    int *idx_in_layer   = g_new0(int, n);
    for (int i = 0; i < n; i++) {
        int ly = L->layer_of[i];
        idx_in_layer[i] = count_in_layer[ly]++;
    }
    /* 求每层最大节点数，决定整体高度 */
    int max_count = 1;
    for (int l = 0; l <= max_layer; l++)
        if (count_in_layer[l] > max_count) max_count = count_in_layer[l];

    for (int i = 0; i < n; i++) {
        L->pos[i].x = L->layer_of[i] * COL_W;
        L->pos[i].y = idx_in_layer[i] * ROW_H;
    }
    L->width  = (max_layer + 1) * COL_W;
    L->height = max_count * ROW_H;
    g_free(count_in_layer);
    g_free(idx_in_layer);

    /* 收集边：trans + suffix */
    GArray *edges = g_array_new(FALSE, FALSE, sizeof(EdgeGeom));
    for (int u = 0; u < n; u++) {
        GHashTable *t = g_array_index(sam->nodes, SamNode, u).trans;
        if (!t) continue;
        GHashTableIter it; gpointer key, val;
        g_hash_table_iter_init(&it, t);
        while (g_hash_table_iter_next(&it, &key, &val)) {
            int cid = GPOINTER_TO_INT(key);
            int v   = GPOINTER_TO_INT(val);
            if (v < 0 || v >= n) continue;
            EdgeGeom eg = {0};
            eg.u = u; eg.v = v; eg.class_id = cid; eg.is_suffix = FALSE;
            eg.n_pts = 2;
            eg.pts = g_new(double, 4);
            eg.pts[0] = L->pos[u].x; eg.pts[1] = L->pos[u].y;
            eg.pts[2] = L->pos[v].x; eg.pts[3] = L->pos[v].y;
            eg.ex = eg.pts[2]; eg.ey = eg.pts[3];
            eg.label_x = (eg.pts[0] + eg.pts[2]) * 0.5;
            eg.label_y = (eg.pts[1] + eg.pts[3]) * 0.5;
            g_array_append_val(edges, eg);
        }
    }
    for (int u = 1; u < n; u++) {
        int link = g_array_index(sam->nodes, SamNode, u).link;
        if (link < 0) continue;
        EdgeGeom eg = {0};
        eg.u = u; eg.v = link; eg.class_id = -1; eg.is_suffix = TRUE;
        eg.n_pts = 2;
        eg.pts = g_new(double, 4);
        eg.pts[0] = L->pos[u].x;    eg.pts[1] = L->pos[u].y;
        eg.pts[2] = L->pos[link].x; eg.pts[3] = L->pos[link].y;
        eg.ex = eg.pts[2]; eg.ey = eg.pts[3];
        eg.label_x = (eg.pts[0] + eg.pts[2]) * 0.5;
        eg.label_y = (eg.pts[1] + eg.pts[3]) * 0.5;
        g_array_append_val(edges, eg);
    }
    L->n_edges = (int)edges->len;
    L->edges   = (EdgeGeom *)g_array_free(edges, FALSE);
}

SamLayout *sam_layout_compute(const Sam *sam, double col_w, double row_h) {
    (void)col_w; (void)row_h;

    int n = sam_node_count(sam);
    SamLayout *L = g_new0(SamLayout, 1);
    L->n        = n;
    L->pos      = g_new0(SamPos, n);
    L->layer_of = g_new0(int,    n);
    if (n == 0) return L;

    /* layer_of 仍按 len 填，便于 UI 调试显示；dot 自由分 rank */
    int max_layer = 0;
    for (int i = 0; i < n; i++) {
        int len = g_array_index(sam->nodes, SamNode, i).len;
        L->layer_of[i] = len;
        if (len > max_layer) max_layer = len;
    }
    L->max_layer = max_layer;

    /* 超大图：跳过 Graphviz，避免主线程卡死 */
    int total_edges = sam_total_edge_count(sam, n);
    if (n > DOT_MAX_NODES || total_edges > DOT_MAX_NODES * 4) {
        g_message("tokensam: 节点 %d 边 %d 超阈值，使用快速分层布局（跳过 graphviz dot）",
                  n, total_edges);
        fallback_layout(L, sam, n);
        return L;
    }

    /* ── 1) 建图 ─────────────────────────────────────────── */
    GVC_t *gvc = gvContext();
    /* AGDIGRAPHSTRICT：不允许重复边（同一 (u,v) 的多条边只保留一条）。
     * SAM 同一对节点最多一条 trans，suffix link 与 trans 不冲突，使用非 strict 即可。 */
    Agraph_t *g = agopen("sam", Agdirected, NULL);

    /* 全局属性必须先 declare 才能 set */
    agattr(g, AGRAPH, "rankdir",  "LR");      /* 左右展开，匹配 SAM 序列扩展语义 */
    agattr(g, AGRAPH, "splines",  "true");
    agattr(g, AGRAPH, "nodesep",  "0.50");
    agattr(g, AGRAPH, "ranksep",  "0.65");
    agattr(g, AGRAPH, "overlap",  "false");

    agattr(g, AGNODE, "shape",     "circle");
    agattr(g, AGNODE, "fixedsize", "true");
    /* dot 节点尺寸必须严格等于 Cairo 渲染时的真实直径，否则边端点会
     * 落在真实圆环之外（dot 节点偏大）或之内（dot 节点偏小）。
     * NODE_R=20px → 直径 40pt → 40/72 ≈ 0.556 inch。 */
    agattr(g, AGNODE, "width",     "0.556");
    agattr(g, AGNODE, "height",    "0.556");
    agattr(g, AGNODE, "label",     "");

    agattr(g, AGEDGE, "constraint", "true");
    agattr(g, AGEDGE, "style",      "");
    agattr(g, AGEDGE, "label",      "");

    Agnode_t **nodes = g_new0(Agnode_t*, n);
    for (int i = 0; i < n; i++) {
        char name[24];
        g_snprintf(name, sizeof name, "n%d", i);
        nodes[i] = agnode(g, name, 1);
    }

    GArray *edge_refs = g_array_new(FALSE, FALSE, sizeof(EdgeRef));

    /* trans 边 */
    for (int u = 0; u < n; u++) {
        GHashTable *t = g_array_index(sam->nodes, SamNode, u).trans;
        if (!t) continue;
        GHashTableIter it; gpointer key, val;
        g_hash_table_iter_init(&it, t);
        while (g_hash_table_iter_next(&it, &key, &val)) {
            int cid = GPOINTER_TO_INT(key);
            int v   = GPOINTER_TO_INT(val);
            if (v < 0 || v >= n) continue;
            /* 边 key 唯一标识：包含 class_id，避免与同终点的其他边重名冲突 */
            char ekey[32];
            g_snprintf(ekey, sizeof ekey, "t%d_%d_%d", u, v, cid);
            Agedge_t *e = agedge(g, nodes[u], nodes[v], ekey, 1);
            EdgeRef ref = { e, u, v, cid, FALSE };
            g_array_append_val(edge_refs, ref);
        }
    }
    /* suffix link 边：constraint=false 不参与 rank，dashed 提示 */
    for (int u = 1; u < n; u++) {
        int link = g_array_index(sam->nodes, SamNode, u).link;
        if (link < 0) continue;
        char ekey[32];
        g_snprintf(ekey, sizeof ekey, "s%d_%d", u, link);
        Agedge_t *e = agedge(g, nodes[u], nodes[link], ekey, 1);
        agsafeset(e, "constraint", "false", "");
        agsafeset(e, "style",      "dashed", "");
        EdgeRef ref = { e, u, link, -1, TRUE };
        g_array_append_val(edge_refs, ref);
    }

    /* ── 2) 布局 ─────────────────────────────────────────── */
    if (gvLayout(gvc, g, "dot") != 0) {
        /* 失败兜底：所有节点放原点 */
        L->width = 100; L->height = 100;
        g_free(nodes);
        g_array_free(edge_refs, TRUE);
        agclose(g);
        gvFreeContext(gvc);
        return L;
    }

    /* ── 3) 收集节点坐标，求 bounding box（dot 单位 = pt） ─── */
    double min_x =  G_MAXDOUBLE, max_x = -G_MAXDOUBLE;
    double min_y =  G_MAXDOUBLE, max_y = -G_MAXDOUBLE;

    for (int i = 0; i < n; i++) {
        pointf p = ND_coord(nodes[i]);
        L->pos[i].x = p.x;
        L->pos[i].y = p.y;
        if (p.x < min_x) min_x = p.x;
        if (p.x > max_x) max_x = p.x;
        if (p.y < min_y) min_y = p.y;
        if (p.y > max_y) max_y = p.y;
    }

    /* ── 4) 收集边样条 ────────────────────────────────────── */
    int ne = (int)edge_refs->len;
    L->n_edges = ne;
    L->edges   = g_new0(EdgeGeom, ne);
    for (int i = 0; i < ne; i++) {
        EdgeRef *r = &g_array_index(edge_refs, EdgeRef, i);
        EdgeGeom *eg = &L->edges[i];
        eg->u         = r->u;
        eg->v         = r->v;
        eg->class_id  = r->class_id;
        eg->is_suffix = r->is_suffix;

        splines *spl = ED_spl(r->e);
        if (!spl || spl->size < 1) {
            /* 兜底：直接用端点连直线 */
            eg->n_pts = 2;
            eg->pts   = g_new(double, 4);
            eg->pts[0] = L->pos[r->u].x; eg->pts[1] = L->pos[r->u].y;
            eg->pts[2] = L->pos[r->v].x; eg->pts[3] = L->pos[r->v].y;
            eg->ex = eg->pts[2]; eg->ey = eg->pts[3];
        } else {
            bezier *b = &spl->list[0];
            int np = b->size;
            eg->n_pts = np;
            eg->pts   = g_new(double, 2*np);
            for (int k = 0; k < np; k++) {
                eg->pts[2*k]   = b->list[k].x;
                eg->pts[2*k+1] = b->list[k].y;
                if (b->list[k].x < min_x) min_x = b->list[k].x;
                if (b->list[k].x > max_x) max_x = b->list[k].x;
                if (b->list[k].y < min_y) min_y = b->list[k].y;
                if (b->list[k].y > max_y) max_y = b->list[k].y;
            }
            if (b->eflag) {
                eg->ex = b->ep.x;
                eg->ey = b->ep.y;
                if (b->ep.x < min_x) min_x = b->ep.x;
                if (b->ep.x > max_x) max_x = b->ep.x;
                if (b->ep.y < min_y) min_y = b->ep.y;
                if (b->ep.y > max_y) max_y = b->ep.y;
            } else {
                eg->ex = eg->pts[2*(np-1)];
                eg->ey = eg->pts[2*(np-1)+1];
            }
        }
        /* 标签锚点：样条中点（先在 dot 坐标系下算，下面统一翻转） */
        spline_midpoint(eg->pts, eg->n_pts, &eg->label_x, &eg->label_y);
    }

    /* ── 5) 坐标系归一：翻转 y、平移到 (0,0) 起 ─────────────── */
    if (min_x > max_x) { min_x = max_x = 0; }
    if (min_y > max_y) { min_y = max_y = 0; }
    double span_x = max_x - min_x;
    double span_y = max_y - min_y;

    for (int i = 0; i < n; i++) {
        L->pos[i].x = L->pos[i].x - min_x;
        L->pos[i].y = max_y - L->pos[i].y;   /* 翻转 */
    }
    for (int i = 0; i < ne; i++) {
        EdgeGeom *eg = &L->edges[i];
        for (int k = 0; k < eg->n_pts; k++) {
            eg->pts[2*k]     = eg->pts[2*k]     - min_x;
            eg->pts[2*k + 1] = max_y - eg->pts[2*k + 1];
        }
        eg->ex = eg->ex - min_x;
        eg->ey = max_y - eg->ey;
        eg->label_x = eg->label_x - min_x;
        eg->label_y = max_y - eg->label_y;
    }

    L->width  = (span_x > 0) ? span_x : 1.0;
    L->height = (span_y > 0) ? span_y : 1.0;

    /* ── 6) 释放 graphviz 资源 ──────────────────────────────── */
    gvFreeLayout(gvc, g);
    agclose(g);
    gvFreeContext(gvc);
    g_free(nodes);
    g_array_free(edge_refs, TRUE);

    return L;
}
