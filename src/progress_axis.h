/**
 * progress_axis.h — 进度轴模块
 *
 * 在主窗口"进度轴"页中负责把整本相册的所有 LAYER_DOODLE 图形按弧长
 * 等比拼接为一条横轴；并在轴上展示：
 *   · 主轴线
 *   · 分页竖线 + 页码 + 段长标签
 *   · 高亮热力图叠加（每条 HighlightRecord 一段半透明色带）
 *
 * 不在本模块的范围（属于轨道行）：
 *   · 把手（A/B 端点对）与拖动交互；
 *   · 由把手对划定的"激活区域"语义。
 *
 * 设计要点：
 *   · 进度轴拥有 GtkDrawingArea 控件的 draw_func；生命周期与 ProgressAxis
 *     实例同步，由调用方负责 progress_axis_free。
 *   · Album 由调用方拥有，仅借用指针；progress_axis 不释放它。
 *   · 数据变动后调用方需主动 progress_axis_refresh（通过 album 的 changed
 *     回调即可串起）。
 */
#pragma once

#include <adwaita.h>
#include "album.h"

G_BEGIN_DECLS

typedef struct ProgressAxis ProgressAxis;

/* 创建进度轴：附加 draw_func 到 canvas。
 * 不接管 canvas 与 album 的所有权。 */
ProgressAxis *progress_axis_new(GtkDrawingArea *canvas, Album *album);

/* 释放：解除 draw_func。 */
void          progress_axis_free(ProgressAxis *pa);

/* 重算几何（弧长、分页偏移、轴宽）并 queue_draw。
 * 调用方在以下时机调用：
 *   · 首次构造完成后；
 *   · Album 内容变动（页增删、涂鸦修改、高亮变动）。 */
void          progress_axis_refresh(ProgressAxis *pa);

/* 当前进度轴像素宽度（已 set_content_width 至 canvas）。
 * 用于轨道行 bar.width-request 与之保持一致。 */
int           progress_axis_get_content_width(ProgressAxis *pa);

/* 全局弧长总长度。用于轨道行拖动 clamp 边界。 */
double        progress_axis_get_total_arc(ProgressAxis *pa);

/* 拖动可達的弧长上界：total_arc>0 时等于 total_arc；否则以
 * width_px 为兑底（在空相册、未涂鸦场景下让轨道行仍可交互，
 * 此时 arc 域 = px 域 1:1）。 */
double        progress_axis_get_drag_arc_max(ProgressAxis *pa);

/* 弧长 ↔ 像素双向映射，与进度轴上用的一致。
 * total_arc <= 0 时采用 1:1 像素兑底（供轨道行交互使用）。 */
double        progress_axis_arc_to_px(ProgressAxis *pa, double arc);
double        progress_axis_px_to_arc(ProgressAxis *pa, double px);

/* 收集吸附点的全局弧长坐标（由小到大排序，含重复则去重）：
 *   · 各分页边界（页起点） + 结束点 total_arc
 *   · 各页 LAYER_DOODLE 中每个 LINE/PATH 的首末端点在全局弧长上的位置
 * 调用方 g_array_unref 释放。 */
GArray       *progress_axis_collect_snap_arcs(ProgressAxis *pa);

/* 播放头（playhead）—— 贯穿进度轴和所有轨道的垂直指针线。
 * 轨道 bar 绘制时通过 progress_axis_get_playhead_px 获取当前 x 位置。 */
double        progress_axis_get_playhead_px(ProgressAxis *pa);
void          progress_axis_set_playhead_arc(ProgressAxis *pa, double arc);

/* 将播放头拖动手势绑定到进度轴画布，并注册 body_container 供联动刷新。
 * 在 progress_axis_new 之后、populate_tracks 之前调用。 */
void          progress_axis_setup_playhead(ProgressAxis *pa,
                                            GtkWidget *body_container);

/* ─── 高亮突出 / 聚焦 / 拾取（供激活区域联动使用） ─────────────
 * 本模块只提供“渲染参数与拾取查询”，不实现联动交互（交互由 main.c
 * 挑起 GtkGestureClick 完成）。 */
typedef struct {
    int     page_idx;
    guint64 hl_id;
} ProgressAxisHLKey;

/* 接管 keys 内容拷贝，不接管 ownership；keys 可传 NULL/空表等同于清空。 */
void          progress_axis_set_emphasized      (ProgressAxis *pa, GArray *keys);
void          progress_axis_clear_emphasized    (ProgressAxis *pa);

/* 单点聚焦（跳转后的可视反馈）；id==0 等同于 clear。 */
void          progress_axis_set_focused_highlight  (ProgressAxis *pa,
                                                     int page, guint64 hl_id);
void          progress_axis_clear_focused_highlight(ProgressAxis *pa);

/* 拾取：在热力图绘制区域内命中某条高亮记录。命中返回 TRUE
 * 并写出 (page, hl_id)；未命中返回 FALSE，*out_* 不修改。 */
gboolean      progress_axis_pick_highlight_at_px(ProgressAxis *pa,
                                                  double x, double y,
                                                  int *out_page,
                                                  guint64 *out_hl_id);

G_END_DECLS
