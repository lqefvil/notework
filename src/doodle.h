/**
 * doodle.h — 涂鸦/绘图模块对外接口
 *
 * 本模块独立于主程序（src/main.c + src/window.ui），
 * 用于在 notework-doodle 这个独立可执行文件中演示
 * 直线 / 手绘路径 / 擦除 / 选择拖动 / 阵列预览 / 图层骨架 /
 * 自动编号与缺号检测等能力。后续可通过 doodle_view_new()
 * 嵌入到主窗口。
 */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

/* ─── 数据模型 ─────────────────────────────────────────────────── */

typedef enum {
    SHAPE_LINE,    /* 直线 */
    SHAPE_PATH,    /* 手绘路径 */
    SHAPE_ARRAY,   /* 阵列组（整体选/拖/删，子项各占编号） */
    SHAPE_RECT,    /* 矩形：a/b 为对角线两点；仅描边，不参与编号体系（仅出现在 paint 层） */
    SHAPE_ELLIPSE  /* 椭圆：a/b 为外接矩形对角；仅描边，不参与编号体系（仅出现在 paint 层） */
} ShapeKind;

typedef struct { double x, y; } DPoint;
typedef struct { double r, g, b, a; } DRGBA;

typedef struct Shape Shape;
struct Shape {
    ShapeKind kind;
    int       number;   /* 全局编号（1..N）。SHAPE_ARRAY 顶层未使用 */
    double    dx, dy;   /* 累积平移；阵列组用同一对偏移整组移动 */
    DRGBA     color;    /* 笔色（仅 LINE/PATH/RECT/ELLIPSE 使用；ARRAY 顶层未使用） */
    union {
        struct { DPoint a, b; } line;
        struct { DPoint *pt; gsize n, cap; } path;
        struct { DPoint a, b; } box;   /* RECT 与 ELLIPSE 共用：a/b 为对角线两点 */
        struct {
            Shape **bases;           /* n_bases 个基准（克隆），保留各自 dx/dy 维持相对位置 */
            int     n_bases;
            int     rows, cols;
            double  gap_x, gap_y;
            int    *child_numbers;   /* 长度 rows*cols*n_bases；
                                      * 索引 = (r*cols+c)*n_bases + b */
        } arr;
    } u;
};

typedef struct {
    Shape **items;
    gsize   n, cap;
} ShapeStore;

typedef enum {
    LAYER_DOODLE,
    LAYER_IMAGE_STUB,
    LAYER_HIGHLIGHT             /* 高亮层；仅吸附在 LINE/PATH 上，置顶渲染 */
} LayerKind;

/* 单条高亮记录：沿宿主路径吸附后采样得到的连续点列。
 * host_layer_idx 指向同一 doc 中的某个 LAYER_DOODLE 层；
 * host_shape_number 是该层中宿主图形（LINE 或 PATH）的稳定全局编号。
 * 整条记录共用一个 width。 */
typedef struct {
    guint64 id;                 /* 全局单调递增唯一标识，用于跨会话稳定绑定 */
    int     host_layer_idx;
    int     host_shape_number;
    double  width;
    DPoint *pt;
    gsize   n, cap;
} HighlightRecord;

typedef struct {
    LayerKind  kind;
    gboolean   visible;
    char      *name;
    ShapeStore store;            /* 仅 LAYER_DOODLE 有效 */

    /* 仅 LAYER_DOODLE 有效；TRUE 表示这是「绘画层」（paint），
     * 其内的 shapes 不参与路径编号体系，也不计入进度轴弧长聚合。
     * 视觉上居于 doodle 路径线条之下，作为图像内容的延伸。 */
    gboolean   is_paint;

    /* 仅 LAYER_IMAGE_STUB 有效；surface 为 NULL 时退化为占位 */
    cairo_surface_t *surface;
    double           img_w;      /* 原生像素宽  */
    double           img_h;      /* 原生像素高  */
    double           x, y;       /* 在画布上的偏移 */
    double           scale;      /* 统一缩放；1.0 为原始尺寸 */

    /* 仅 LAYER_HIGHLIGHT 有效；元素为 HighlightRecord*（堆分配） */
    GPtrArray *highlights;
} Layer;

typedef struct {
    GArray *layers;       /* Layer（按值存储） */
    int     active_layer; /* 当前编辑的涂鸦层下标 */
} DoodleDoc;

/* ─── Shape 构造/释放 ─────────────────────────────────────────── */
Shape *shape_new_line(DPoint a, DPoint b);
Shape *shape_new_path(void);
void   shape_path_add_point(Shape *s, DPoint p);
Shape *shape_new_rect(DPoint a, DPoint b);     /* a/b 为对角线两点 */
Shape *shape_new_ellipse(DPoint a, DPoint b);  /* a/b 为外接矩形对角 */
Shape *shape_clone(const Shape *s);
void   shape_free(Shape *s);

/* ─── 几何弧长（进度轴使用） ──────────────────────────────────────
 * SHAPE_LINE: 两点欧氏距离
 * SHAPE_PATH: 相邻点距离累加
 * SHAPE_ARRAY: 0（本期不参与进度轴聚合，避免与组内副本编号体系混淆）
 * SHAPE_RECT/SHAPE_ELLIPSE: 0（仅出现在 paint 层，视为图像延伸不计入路径弧长）
 * doodle_doc_total_arc: 遍历 doc 内所有 LAYER_DOODLE 层，对其每个 shape
 *   调用 shape_arc_length 求和；不计入隐藏图层；自动跳过 is_paint 层。 */
double shape_arc_length(const Shape *s);
double doodle_doc_total_arc(const DoodleDoc *doc);

/* ─── 文档/编号 API ───────────────────────────────────────────── */
DoodleDoc  *doodle_doc_new(void);
/* 创建不包含任何图层的空文档，调用者需自行插入首层
 * 并保证后续访问前 active_layer 进入合法范围。 */
DoodleDoc  *doodle_doc_new_empty(void);
void        doodle_doc_free(DoodleDoc *doc);
Layer      *doc_active_layer(DoodleDoc *doc);
ShapeStore *doc_active_store(DoodleDoc *doc);
int         doc_layer_count(const DoodleDoc *doc);
int         doc_max_number(const DoodleDoc *doc);

/* 添加新 shape：自动分配 number = max+1 */
void   doc_add_shape(DoodleDoc *doc, Shape *s);

/* 删除：number 之后的所有编号自动 -1（阵列组按其子项段长度补位） */
void   doc_remove_shape_at(DoodleDoc *doc, gsize idx);

/* 用 n_new 条新图形替换原图形（擦除分裂用，适用于手绘路径与直线）；
 * 第一条沿用原编号，其余按原编号紧后插入并把后续编号整体顺移。
 * n_new == 0 等同于 doc_remove_shape_at。 */
void   doc_replace_shape_with_shapes(DoodleDoc *doc, gsize idx,
                                     Shape **new_shapes, gsize n_new);

/* 把 idx 处的 SHAPE_ARRAY 展开为 rows*cols*n_bases 个独立图形，
 * 各自沿用原子项编号与画布位置；总编号空间不变。 */
void   doc_explode_array_at(DoodleDoc *doc, gsize idx);

/* 把若干图形（不能是 SHAPE_ARRAY）包装为 rows*cols 的阵列组。
 * base_indices 中的图形会被从 store 移除，组追加到 store 末尾。
 * 编号按 (r,c) 行优先、副本内按 base 在画布上的位置（锚点 y 后 x 升序）。 */
void   doc_apply_array(DoodleDoc *doc,
                       const int *base_indices, int n_idx,
                       int rows, int cols, double gap_x, double gap_y,
                       double pre_dx, double pre_dy);

/* 一键整理：按当前 store 顺序把所有非阵列图形与阵列子项的编号重排为 1..N 紧凑序列。 */
void   doc_compact_numbers(DoodleDoc *doc);

/* 在当前 doodle 层下方插入图片占位层（仅骨架，不加载图片）*/
void doc_insert_image_stub_below_active(DoodleDoc *doc);

/* 在当前 doodle 层下方插入携带 cairo_surface 的真实图片层；
 * doc 接管 surface 引用 (cairo_surface_reference)。 */
void doc_insert_image_layer_below_active(DoodleDoc *doc,
                                          cairo_surface_t *surface,
                                          const char *name);

/* 图层辅助 API（按值在 GArray<Layer> 中维护） */
Layer  layer_new_doodle_value(const char *name);
Layer  layer_new_image_value (cairo_surface_t *surface, const char *name);
Layer  layer_new_highlight_value(const char *name);
Layer  layer_new_paint_value(const char *name);  /* 绘画层：is_paint=TRUE，置于 doodle 之下、image 之上 */
Layer  layer_clone_value     (const Layer *src);
void   layer_free_contents   (Layer *l);

/* HighlightRecord 构造/释放/采样 */
HighlightRecord *highlight_record_new(int host_layer_idx,
                                       int host_shape_number,
                                       double width);
void             highlight_record_add_point(HighlightRecord *r, DPoint p);
void             highlight_record_free(HighlightRecord *r);
HighlightRecord *highlight_record_clone(const HighlightRecord *src);

/* 在 doc 中查找/确保高亮层存在；返回其在 doc->layers 中的下标。
 * doc_ensure_top_highlight_layer 若不存在会在最顶端追加一个新的高亮层。 */
int  doc_find_top_highlight_layer (const DoodleDoc *doc);
int  doc_ensure_top_highlight_layer(DoodleDoc *doc);
int  doc_find_top_doodle_layer    (const DoodleDoc *doc);

/* paint 层：is_paint=TRUE 的 LAYER_DOODLE。
 * doc_find_top_paint_layer：若不存在返回 -1。
 * doc_ensure_top_paint_layer：若不存在则在最顶部 doodle 层下方插入一个新的；
 *   返回该 paint 层在 doc->layers 中的下标。 */
int  doc_find_top_paint_layer    (const DoodleDoc *doc);
int  doc_ensure_top_paint_layer  (DoodleDoc *doc);

/* 把 shape 加入 paint 层（不分配编号；其 number 字段保持 0）。
 * 调用方需保证 layer_idx 指向 is_paint 层。 */
void doc_add_shape_to_paint_layer(DoodleDoc *doc, int layer_idx, Shape *s);

/* 通用：删除指定图层 store 中的第 idx 个 shape（适用于 paint 层与 doodle 层）。
 * 如果是 doodle 路径层（!is_paint），会按现有规则 shift 编号；
 * 如果是 paint 层（is_paint），仅释放并移除，不动编号。 */
void doc_remove_shape_at_layer(DoodleDoc *doc, int layer_idx, gsize idx);

/* 文档：图层增删改查（以 Layer 值拷贝交付所有权） */
void   doc_insert_layer_at(DoodleDoc *doc, int pos, Layer src_value);
void   doc_remove_layer    (DoodleDoc *doc, int idx);
void   doc_move_layer      (DoodleDoc *doc, int from, int to);
void   doc_set_active_layer(DoodleDoc *doc, int idx);
Layer  doc_clone_layer_value(const DoodleDoc *doc, int idx);

/* 设置任意编号（允许产生空缺 / 重复，不自动整理）；<1 视为非法 */
gboolean shape_set_number(Shape *s, int new_n);

/* 计算 1..max 中缺失的编号；返回 GArray<int>（升序），调用者负责释放 */
GArray *doc_missing_numbers(const DoodleDoc *doc);

/* 是否存在重复编号（用户改号可能造成） */
gboolean doc_has_duplicate_numbers(const DoodleDoc *doc);

/* ─── 画布控件 ────────────────────────────────────────────────── */
typedef enum {
    TOOL_LINE,
    TOOL_PATH,
    TOOL_RECT,        /* 矩形（仅作用于 paint 层） */
    TOOL_ELLIPSE,     /* 椭圆（仅作用于 paint 层） */
    TOOL_ERASE,
    TOOL_SELECT,
    TOOL_HIGHLIGHT,
    TOOL_HL_ERASE
} Tool;

typedef void (*DoodleChangedFn)(GtkWidget *canvas, gpointer user_data);

GtkWidget *doodle_canvas_new(DoodleDoc *doc);
DoodleDoc *doodle_canvas_get_doc(GtkWidget *w);
void       doodle_canvas_set_tool(GtkWidget *w, Tool t);
Tool       doodle_canvas_get_tool(GtkWidget *w);

/* 绘画目标开关：TRUE 时 LINE/PATH/RECT/ELLIPSE 工具新建的形状落到 paint 层
 *（不存在则自动创建）；FALSE 时 LINE/PATH 落到 doodle 路径层。
 * RECT/ELLIPSE 工具被选中时会强制 paint 目标为 TRUE。 */
void       doodle_canvas_set_paint_target(GtkWidget *w, gboolean to_paint);
gboolean   doodle_canvas_get_paint_target(GtkWidget *w);

/* 全局笔色：仅作用于此后新绘制的图形（已存在图形保持原色）。
 * 默认 {0,0,0,1}（黑色不透明）。 */
void       doodle_canvas_set_global_pen_color(GtkWidget *w, DRGBA c);
DRGBA      doodle_canvas_get_global_pen_color(GtkWidget *w);

int        doodle_canvas_get_selected_index(GtkWidget *w); /* -1: none */
void       doodle_canvas_set_selected_index(GtkWidget *w, int idx);

/* 多选与阵列使能检查：至少 1 个选中且都不是 SHAPE_ARRAY */
gboolean   doodle_canvas_selection_array_eligible(GtkWidget *w);
int        doodle_canvas_selection_count(GtkWidget *w);

void       doodle_canvas_request_redraw(GtkWidget *w);
void       doodle_canvas_set_changed_cb(GtkWidget *w,
                                        DoodleChangedFn cb,
                                        gpointer        user_data);

/* 阵列预览：调用前需先选中一个非阵列图形；返回 FALSE 表示前置条件不满足 */
gboolean   doodle_canvas_begin_array_preview(GtkWidget *w,
                                             int rows, int cols,
                                             double gap_x, double gap_y);
void       doodle_canvas_set_array_params(GtkWidget *w,
                                          int rows, int cols,
                                          double gap_x, double gap_y);
gboolean   doodle_canvas_is_array_active(GtkWidget *w);
void       doodle_canvas_apply_array(GtkWidget *w);
void       doodle_canvas_cancel_array(GtkWidget *w);

/* ─── 窗口/视图 ──────────────────────────────────────────────── */
GtkWidget *doodle_window_new(void);  /* AdwApplicationWindow，未关联 GApplication */
GtkWidget *doodle_view_new(void);    /* 仅 paned 内容区，便于嵌入主窗口 */
DoodleDoc *doodle_view_get_doc(GtkWidget *view);

/* 用外部提供的 DoodleDoc 构造涂鸦窗口；own_doc=FALSE 时调用方负责释放 doc。 */
GtkWidget *doodle_window_new_for_doc(DoodleDoc *doc, gboolean own_doc);

/* 画布：渲染选项与只读模式 */
void       doodle_canvas_set_doodle_alpha(GtkWidget *w, double alpha);
void       doodle_canvas_set_view_only   (GtkWidget *w, gboolean view_only);

/* 画布：视图缩放（仅影响画布渲染，不修改文档数据）。
 * 范围 0.25 – 4.0，默认 1.0；set 会自动 clamp。 */
#define DOODLE_VIEW_SCALE_MIN 0.25
#define DOODLE_VIEW_SCALE_MAX 4.00
#define DOODLE_VIEW_SCALE_DEF 1.00
void       doodle_canvas_set_view_scale(GtkWidget *w, double scale);
double     doodle_canvas_get_view_scale(GtkWidget *w);

/* ─── 高亮：全局/局部 width 与选中状态 ───────────────────────────── */
/* 默认 12px，范围 4–25px。设置时会自动 clamp。 */
void       doodle_canvas_set_global_highlight_width(GtkWidget *w, double width);
double     doodle_canvas_get_global_highlight_width(GtkWidget *w);

/* 选中高亮记录：layer_idx 应为 LAYER_HIGHLIGHT 层下标；rec_idx 为其内部下标。
 * 任一参数 <0 表示清除选中。 */
void       doodle_canvas_set_selected_highlight(GtkWidget *w,
                                                int layer_idx, int rec_idx);
void       doodle_canvas_get_selected_highlight(GtkWidget *w,
                                                int *layer_idx, int *rec_idx);
/* 调整当前选中高亮记录的 width；未选中时无效 */
void       doodle_canvas_set_selected_highlight_width(GtkWidget *w, double width);

/* 删除当前选中的高亮记录；未选中时无效。
 * 删除后会自动清除高亮选中状态并触发 changed_cb。 */
void       doodle_canvas_delete_selected_highlight(GtkWidget *w);

/* 设置画布初始工具（doodle_window 创建时由调用方触发，例如 album → 高亮） */
void       doodle_window_set_initial_tool(GtkWidget *win, Tool t);

/* 通用纯渲染入口：在任意 cairo_t 上画出 doc 的图层叠加结果（不含交互覆层） */
void       doodle_render_doc(cairo_t *cr, DoodleDoc *doc,
                              double doodle_alpha, int width, int height);

G_END_DECLS
