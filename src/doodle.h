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
    SHAPE_LINE,   /* 直线 */
    SHAPE_PATH,   /* 手绘路径 */
    SHAPE_ARRAY   /* 阵列组（整体选/拖/删，子项各占编号） */
} ShapeKind;

typedef struct { double x, y; } DPoint;

typedef struct Shape Shape;
struct Shape {
    ShapeKind kind;
    int       number;   /* 全局编号（1..N）。SHAPE_ARRAY 顶层未使用 */
    double    dx, dy;   /* 累积平移；阵列组用同一对偏移整组移动 */
    union {
        struct { DPoint a, b; } line;
        struct { DPoint *pt; gsize n, cap; } path;
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
    LAYER_IMAGE_STUB
} LayerKind;

typedef struct {
    LayerKind  kind;
    gboolean   visible;
    char      *name;
    ShapeStore store;   /* 仅 LAYER_DOODLE 有效 */
} Layer;

typedef struct {
    GArray *layers;       /* Layer（按值存储） */
    int     active_layer; /* 当前编辑的涂鸦层下标 */
} DoodleDoc;

/* ─── Shape 构造/释放 ─────────────────────────────────────────── */
Shape *shape_new_line(DPoint a, DPoint b);
Shape *shape_new_path(void);
void   shape_path_add_point(Shape *s, DPoint p);
Shape *shape_clone(const Shape *s);
void   shape_free(Shape *s);

/* ─── 文档/编号 API ───────────────────────────────────────────── */
DoodleDoc  *doodle_doc_new(void);
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

/* 在当前 doodle 层下方插入图片占位层（仅骨架，不加载图片） */
void   doc_insert_image_stub_below_active(DoodleDoc *doc);

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
    TOOL_ERASE,
    TOOL_SELECT
} Tool;

typedef void (*DoodleChangedFn)(GtkWidget *canvas, gpointer user_data);

GtkWidget *doodle_canvas_new(DoodleDoc *doc);
DoodleDoc *doodle_canvas_get_doc(GtkWidget *w);
void       doodle_canvas_set_tool(GtkWidget *w, Tool t);
Tool       doodle_canvas_get_tool(GtkWidget *w);

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

G_END_DECLS
