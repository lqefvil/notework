/**
 * track_row.h — 轨道 bar（条带）构造
 *
 * 一条轨道在视觉上由两部分组成：
 *   · 左侧固定 sidebar 内的「名称 + × 删除按钮」（由 main.c inline 构造）
 *   · 右侧水平滚动区内的「条带」（本接口构造，含拖动/绘制全部交互）
 *
 * 条带 = 一个 GtkDrawingArea，其 width 与进度轴 content_width 一致：
 *   · 每对 HandlePair 渲染为「半透明蓝色色块 + 两侧把手三角」；
 *   · GtkGestureDrag 提供两种交互：
 *       - 把柄上拖动：移动 A 或 B 端点（A≤B 约束 + 8px 吸附到分页线/笔画端点）；
 *       - 空白处拖动：从落点开始创建新的把手对，B 跟随光标；
 *   · 拖动结束若产生零宽对（纯单击），则把该把手对清除。
 */
#pragma once

#include <adwaita.h>
#include "album.h"
#include "progress_axis.h"

G_BEGIN_DECLS

/* 构造一条轨道 bar。返回的 GtkDrawingArea 直接 gtk_box_append 到容器即可。
 *   · album/axis 仅借用，调用方需保证生命周期不短于该 bar；
 *   · track_idx 为 album->tracks 中的索引；删除/重排后调用方需重新装填；
 *   · bar_width_px 由 progress_axis_get_content_width 提供；
 *
 * 引用：transfer-full —— 调用方 gtk_box_append 后须 g_object_unref 平衡。
 * TrackRow 控制器以 qdata("track-row") 挂在 bar 上，随 bar 销毁。 */
GtkWidget *track_row_new(Album *album, ProgressAxis *axis,
                          int track_idx, int bar_width_px);

G_END_DECLS
