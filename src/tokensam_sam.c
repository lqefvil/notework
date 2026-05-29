/**
 * tokensam_sam.c — Token 级后缀自动机
 *
 * 实现要点：
 *   1) 字母表为「等价类 id」（int），转移表用 GHashTable<int,int>；
 *   2) GArray<SamNode> 在 append 时可能 realloc，所以全程用「索引」访问，
 *      绝不缓存 SamNode* 指针；
 *   3) clone 状态时深拷贝 trans 哈希表。
 */
#include "tokensam.h"

#include <string.h>

#define NODE(sam, i) (&g_array_index((sam)->nodes, SamNode, (i)))

static GHashTable *new_trans_table(void) {
    return g_hash_table_new(g_direct_hash, g_direct_equal);
}

static GHashTable *clone_trans_table(GHashTable *src) {
    GHashTable *dst = new_trans_table();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, src);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        g_hash_table_insert(dst, k, v);
    }
    return dst;
}

static int sam_new_node(Sam *sam, int len, int link) {
    SamNode n;
    n.len = len;
    n.link = link;
    n.trans = new_trans_table();
    n.is_clone = FALSE;
    g_array_append_val(sam->nodes, n);
    return (int)sam->nodes->len - 1;
}

Sam *sam_new(void) {
    Sam *sam = g_new0(Sam, 1);
    sam->nodes = g_array_new(FALSE, FALSE, sizeof(SamNode));
    sam_new_node(sam, 0, -1);   /* 初始状态 */
    sam->last = 0;
    return sam;
}

void sam_free(Sam *sam) {
    if (!sam) return;
    for (guint i = 0; i < sam->nodes->len; i++) {
        GHashTable *t = g_array_index(sam->nodes, SamNode, i).trans;
        if (t) g_hash_table_destroy(t);
    }
    g_array_free(sam->nodes, TRUE);
    g_free(sam);
}

static int trans_get(const Sam *sam, int u, int c) {
    GHashTable *t = NODE(sam, u)->trans;
    gpointer hit;
    if (g_hash_table_lookup_extended(t, GINT_TO_POINTER(c), NULL, &hit))
        return GPOINTER_TO_INT(hit);
    return -1;
}

static void trans_set(Sam *sam, int u, int c, int v) {
    GHashTable *t = NODE(sam, u)->trans;
    g_hash_table_insert(t, GINT_TO_POINTER(c), GINT_TO_POINTER(v));
}

void sam_extend(Sam *sam, int c) {
    int last = sam->last;
    int cur  = sam_new_node(sam, NODE(sam, last)->len + 1, -1);

    int p = last;
    while (p != -1 && trans_get(sam, p, c) < 0) {
        trans_set(sam, p, c, cur);
        p = NODE(sam, p)->link;
    }
    if (p == -1) {
        NODE(sam, cur)->link = 0;
    } else {
        int q = trans_get(sam, p, c);
        if (NODE(sam, p)->len + 1 == NODE(sam, q)->len) {
            NODE(sam, cur)->link = q;
        } else {
            /* clone q */
            int clone = sam_new_node(sam, NODE(sam, p)->len + 1,
                                     NODE(sam, q)->link);
            NODE(sam, clone)->is_clone = TRUE;
            /* 复制 q 的转移表到 clone（注意 q 在 clone 之后仍然有效，但 NODE 指针不能跨 append 缓存） */
            GHashTable *qtrans = NODE(sam, q)->trans;
            GHashTable *ctrans = clone_trans_table(qtrans);
            g_hash_table_destroy(NODE(sam, clone)->trans);
            NODE(sam, clone)->trans = ctrans;

            /* 把所有从 p 沿 suffix link 上行、且经 c 指向 q 的状态改指 clone */
            while (p != -1 && trans_get(sam, p, c) == q) {
                trans_set(sam, p, c, clone);
                p = NODE(sam, p)->link;
            }
            NODE(sam, q)->link   = clone;
            NODE(sam, cur)->link = clone;
        }
    }
    sam->last = cur;
}

Sam *sam_build(const int *ids, gsize n) {
    Sam *sam = sam_new();
    for (gsize i = 0; i < n; i++) sam_extend(sam, ids[i]);
    return sam;
}

/* ─── GSA（广义后缀自动机）增量构造 ──────────────────────────────
 * Blumer 等人的标准在线 GSA 算法。与单串 sam_extend 的关键差别在入口：
 * 当 last 已有 c-trans 到 q 时，不创建新 cur 节点，而是按 q.len 与
 * len(last)+1 是否相等来决定「直接复用 q」或「克隆 q」。
 * 这样多次 sam_append_string 调用会得到与「全部串拼接后一次性构造的
 * SAM」结构等价的图。 */
void sam_extend_gsa(Sam *sam, int c) {
    int last = sam->last;

    /* —— 情形 1：last 已经有 c-trans —— */
    int q0 = trans_get(sam, last, c);
    if (q0 >= 0) {
        if (NODE(sam, last)->len + 1 == NODE(sam, q0)->len) {
            /* q0 完整对应「last 后接 c」，直接跳过去 */
            sam->last = q0;
            return;
        }
        /* 否则克隆 q0 出来作为新 last */
        int clone = sam_new_node(sam, NODE(sam, last)->len + 1,
                                 NODE(sam, q0)->link);
        NODE(sam, clone)->is_clone = TRUE;
        GHashTable *qtrans = NODE(sam, q0)->trans;
        GHashTable *ctrans = clone_trans_table(qtrans);
        g_hash_table_destroy(NODE(sam, clone)->trans);
        NODE(sam, clone)->trans = ctrans;

        /* 重定向所有沿 last 的 suffix link 链上、c-trans 还指向 q0 的状态 */
        int p = last;
        while (p != -1 && trans_get(sam, p, c) == q0) {
            trans_set(sam, p, c, clone);
            p = NODE(sam, p)->link;
        }
        NODE(sam, q0)->link = clone;
        sam->last = clone;
        return;
    }

    /* —— 情形 2：last 没有 c-trans —— 与 sam_extend 一致 */
    int cur = sam_new_node(sam, NODE(sam, last)->len + 1, -1);

    int p = last;
    while (p != -1 && trans_get(sam, p, c) < 0) {
        trans_set(sam, p, c, cur);
        p = NODE(sam, p)->link;
    }
    if (p == -1) {
        NODE(sam, cur)->link = 0;
    } else {
        int q = trans_get(sam, p, c);
        if (NODE(sam, p)->len + 1 == NODE(sam, q)->len) {
            NODE(sam, cur)->link = q;
        } else {
            int clone = sam_new_node(sam, NODE(sam, p)->len + 1,
                                     NODE(sam, q)->link);
            NODE(sam, clone)->is_clone = TRUE;
            GHashTable *qtrans = NODE(sam, q)->trans;
            GHashTable *ctrans = clone_trans_table(qtrans);
            g_hash_table_destroy(NODE(sam, clone)->trans);
            NODE(sam, clone)->trans = ctrans;

            while (p != -1 && trans_get(sam, p, c) == q) {
                trans_set(sam, p, c, clone);
                p = NODE(sam, p)->link;
            }
            NODE(sam, q)->link   = clone;
            NODE(sam, cur)->link = clone;
        }
    }
    sam->last = cur;
}

void sam_append_string(Sam *sam, const int *ids, gsize n) {
    if (!sam) return;
    sam->last = 0;   /* GSA 的关键：每条新串都从根开始读 */
    for (gsize i = 0; i < n; i++) sam_extend_gsa(sam, ids[i]);
}

Sam *sam_copy(const Sam *src) {
    if (!src) return NULL;
    Sam *dst = g_new0(Sam, 1);
    dst->nodes = g_array_sized_new(FALSE, FALSE, sizeof(SamNode), src->nodes->len);
    for (guint i = 0; i < src->nodes->len; i++) {
        const SamNode *s = &g_array_index(src->nodes, SamNode, i);
        SamNode d;
        d.len      = s->len;
        d.link     = s->link;
        d.is_clone = s->is_clone;
        d.trans    = clone_trans_table(s->trans);
        g_array_append_val(dst->nodes, d);
    }
    dst->last = src->last;
    return dst;
}

int sam_node_count(const Sam *sam) {
    return sam ? (int)sam->nodes->len : 0;
}

int sam_edge_count(const Sam *sam) {
    if (!sam) return 0;
    int total = 0;
    for (guint i = 0; i < sam->nodes->len; i++) {
        total += g_hash_table_size(
            g_array_index(sam->nodes, SamNode, i).trans);
    }
    return total;
}
