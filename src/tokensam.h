/**
 * tokensam.h — Token 级后缀自动机（含同义词等价类）独立模块对外接口
 *
 * 模块分层（与 GTK 解耦）：
 *   - Lexicon  : 词典 + 同义词等价类（Union-Find）
 *   - Tokenize : 严格分隔符切分 + 词典查找
 *   - SAM      : token 级后缀自动机（字母表 = 等价类 id）
 *   - Layout   : 按 len 分层的 DAG 布局
 * 以上四层不引用任何 GTK 头文件，方便后续被主程序集成。
 *
 * UI 层：
 *   - tokensam_window/canvas : GTK4 + Cairo 自绘 DAG
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* ─── Lexicon ─────────────────────────────────────────────────── */

typedef struct {
    char *text;        /* 用户定义的字符串（深拷贝持有） */
    int   class_id;    /* 等价类 id ∈ [0, class_count)；同义词共享 */
} LexEntry;

typedef struct Lexicon Lexicon;

Lexicon *lexicon_new(void);
void     lexicon_free(Lexicon *lex);

/* 添加 token；若 text 已存在则原样返回已有索引、不做新增。
 * 新 entry 默认独立成一个新等价类。返回 entry 索引；text 为空/NULL 返回 -1。 */
int      lexicon_add(Lexicon *lex, const char *text);

/* 删除指定索引的 entry；删除后类号会自动 compact。返回 TRUE 表示成功。 */
gboolean lexicon_remove_at(Lexicon *lex, int idx);

/* 把若干 entry 合并到同一等价类（保留所有成员中最小的 class_id 作为代表）。 */
void     lexicon_merge(Lexicon *lex, const int *entry_indices, int n);

/* 把指定 entry 拆出独立等价类（即使该类只剩它一个，也会触发一次 compact）。 */
void     lexicon_split(Lexicon *lex, int idx);

/* 查询 */
int             lexicon_size       (const Lexicon *lex);
const LexEntry *lexicon_at         (const Lexicon *lex, int idx);
int             lexicon_class_count(const Lexicon *lex);

/* 文本 -> class_id；未找到返回 -1。 */
int      lexicon_lookup_class(const Lexicon *lex, const char *text);

/* 取等价类内的全部代表文本（用 " | " 拼接），用于 UI/调试显示；free by caller。 */
char    *lexicon_class_label(const Lexicon *lex, int class_id);

/* ─── Tokenize ────────────────────────────────────────────────── */

typedef struct {
    int   byte_offset;   /* 在原始输入字节流中的偏移（UI 错误高亮用） */
    int   byte_len;
    char *piece;         /* 切出的子串内容（深拷贝） */
} TokenizeError;

typedef struct {
    int   *ids;          /* 等价类 id 序列；长度 n */
    gsize  n;
    GArray *errors;      /* TokenizeError 按值存储 */
} TokenizeResult;

/* 切分模式 */
typedef enum {
    TOKENIZE_BY_DELIM   = 0, /* 按分隔符切：sep 中每个 Unicode 字符均视为分隔符候选 */
    TOKENIZE_BY_CHAR    = 1, /* 按 Unicode 字符切：每个字符就是一个 token，忽略 sep */
    TOKENIZE_BY_LEXICON = 2, /* 按词典最长前缀切：从左到右贪心匹配词典词；
                              * 任一段落无前缀命中时累积为「未识别段」记入 errors，
                              * 由 UI 弹窗让用户确认是否入典（auto_add=TRUE 时仍走自动入典）。 */
} TokenizeMode;

/* tokenize：
 * - mode=BY_DELIM：sep 中的每个 Unicode 字符都是分隔符候选（任意命中即切分）；
 *   sep 为空时退化为整段输入作为单一 token。
 * - mode=BY_CHAR：忽略 sep，按 Unicode 字符逐个切。
 * - auto_add=TRUE：未识别 token 自动加入词典（lex 必须非 const，因此本函数取 Lexicon*）；
 *   FALSE 时仍记录 TokenizeError。
 */
TokenizeResult *tokenize(Lexicon *lex, const char *text, const char *sep,
                         TokenizeMode mode, gboolean auto_add);
void            tokenize_result_free(TokenizeResult *r);

/* ─── SAM ─────────────────────────────────────────────────────── */

typedef struct {
    int         len;
    int         link;     /* -1 表示无 suffix link（仅初始状态） */
    GHashTable *trans;    /* int(class_id) -> int(node_idx)，皆用 GINT_TO_POINTER 编码 */
    gboolean    is_clone; /* 该状态由 clone 创建（用于可视化区分） */
} SamNode;

typedef struct {
    GArray *nodes;        /* SamNode 按值；index 0 为初始状态 */
    int     last;
} Sam;

Sam *sam_new(void);
void sam_free(Sam *sam);
void sam_extend(Sam *sam, int class_id);
Sam *sam_build(const int *ids, gsize n);

/* GSA（广义后缀自动机）增量构造。
 * 与 sam_extend 不同：进入前若 trans[last][c] 已存在，则按 q.len 是否
 * 等于 last.len+1 决定「直接跳转」或「克隆 q」，避免重复创建 cur 节点。
 * 用于多次输入串拼接成同一张 SAM 的场景。 */
void sam_extend_gsa(Sam *sam, int class_id);

/* 把一段 token 流追加到现有 SAM 上：自动 last=0 重置，再逐个 gsa_extend。
 * 等价于 GSA 的「新增一条串」操作。 */
void sam_append_string(Sam *sam, const int *ids, gsize n);

/* 深拷贝 SAM（含每个节点的 trans 哈希表）。
 * 用于异步布局：把快照交给 worker 线程，避免主线程后续 append 改动数据。 */
Sam *sam_copy(const Sam *src);

int  sam_node_count(const Sam *sam);
int  sam_edge_count(const Sam *sam);   /* 仅统计 trans 边 */

/* ─── Layout ──────────────────────────────────────────────────── */

typedef struct { double x, y; } SamPos;

/* 一条带样条几何信息的边（trans 或 suffix link）。
 * Graphviz dot 输出的边由若干段三阶贝塞尔拼接，控制点总数 = 3k+1。
 * 当 n_pts < 2 时退化为直线（极少出现，仅作兜底）。 */
typedef struct {
    int      u, v;          /* 端点节点 index */
    int      class_id;      /* trans 边带 class_id；suffix link 为 -1 */
    gboolean is_suffix;
    int      n_pts;
    double  *pts;           /* 长度 = 2 * n_pts，顺序 x0,y0,x1,y1,... */
    double   ex, ey;        /* 箭头尖端坐标 */
    double   label_x, label_y;  /* 标签锚点（取曲线中点） */
} EdgeGeom;

typedef struct {
    SamPos   *pos;          /* 长度 = sam_node_count(sam) */
    int       n;
    double    width, height;
    int      *layer_of;     /* 每个节点所属层（= node.len） */
    int       max_layer;
    EdgeGeom *edges;
    int       n_edges;
} SamLayout;

/* col_w/row_h 对 Graphviz 后端仅作为偏好参考，
 * 真实坐标由 dot 引擎根据节点尺寸与 nodesep/ranksep 计算。 */
SamLayout *sam_layout_compute(const Sam *sam, double col_w, double row_h);
void       sam_layout_free(SamLayout *layout);

/* ─── UI 层 ───────────────────────────────────────────────────── */
#ifdef GTK_TYPE_WIDGET   /* 仅当包含 <gtk/gtk.h>/adwaita 后才可用 */

/* DAG 画布：自绘 GtkDrawingArea 的封装。
 * 调用 set_data 后会触发布局重算与重绘；sam/lex 仅作只读引用，
 * 调用方必须保证调用 set_data(NULL,NULL) 或释放 canvas 之前不释放它们。 */
GtkWidget *tokensam_canvas_new(void);
void       tokensam_canvas_set_data(GtkWidget *w,
                                    const Sam *sam,
                                    const Lexicon *lex);
void       tokensam_canvas_set_show_suffix_link(GtkWidget *w, gboolean on);

/* 主窗口（AdwApplicationWindow，不主动关联 GApplication，由 main 注册） */
GtkWidget *tokensam_window_new(void);

#endif

G_END_DECLS
