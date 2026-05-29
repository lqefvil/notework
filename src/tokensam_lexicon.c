/**
 * tokensam_lexicon.c — 词典与同义词等价类
 *
 * 设计：
 *   - entries: GArray<LexEntry>，按值存储；text 深拷贝持有。
 *   - text2idx: GHashTable<char*, int>（值用 GINT_TO_POINTER 编码 entry 索引），
 *               key 共享 entries[i].text 指针，不再二次释放。
 *   - uf_parent: GArray<int>，与 entries 等长，标准 Union-Find。
 *   - class_id 在每次 add/remove/merge/split 后通过一次 O(N) 重算保持紧凑：
 *     class_id[i] = 该 entry 所在并查集根在所有根中的有序排名。
 */
#include "tokensam.h"

#include <string.h>

struct Lexicon {
    GArray     *entries;     /* LexEntry */
    GHashTable *text2idx;    /* text -> GINT_TO_POINTER(entry index) */
    GArray     *uf_parent;   /* int parent；与 entries 等长 */
    int         class_count;
};

/* ─── 内部：Union-Find ────────────────────────────────────────── */

static int uf_find(GArray *p, int x) {
    while (g_array_index(p, int, x) != x) {
        int parent = g_array_index(p, int, x);
        int gp = g_array_index(p, int, parent);
        g_array_index(p, int, x) = gp;     /* 路径压缩到祖父 */
        x = gp;
    }
    return x;
}

static void uf_union(GArray *p, int a, int b) {
    int ra = uf_find(p, a);
    int rb = uf_find(p, b);
    if (ra == rb) return;
    /* 把较大根挂到较小根；保证「最小 id 为代表」 */
    if (ra < rb) g_array_index(p, int, rb) = ra;
    else         g_array_index(p, int, ra) = rb;
}

/* ─── 内部：重算 class_id 使其紧凑 [0, class_count) ────────────── */

static void recompact_classes(Lexicon *lex) {
    int n = lex->entries->len;
    /* 收集所有根并排序 */
    GHashTable *root2cid = g_hash_table_new(g_direct_hash, g_direct_equal);
    GArray *roots = g_array_new(FALSE, FALSE, sizeof(int));
    for (int i = 0; i < n; i++) {
        int r = uf_find(lex->uf_parent, i);
        if (!g_hash_table_contains(root2cid, GINT_TO_POINTER(r))) {
            g_hash_table_insert(root2cid, GINT_TO_POINTER(r),
                                GINT_TO_POINTER(0));
            g_array_append_val(roots, r);
        }
    }
    /* 用插入排序（roots 数量很小） */
    int *raw = (int *)roots->data;
    for (int i = 1; i < (int)roots->len; i++) {
        int v = raw[i], j = i - 1;
        while (j >= 0 && raw[j] > v) { raw[j+1] = raw[j]; j--; }
        raw[j+1] = v;
    }
    /* 给每个根分配 class id */
    for (int i = 0; i < (int)roots->len; i++) {
        g_hash_table_insert(root2cid, GINT_TO_POINTER(raw[i]),
                            GINT_TO_POINTER(i));
    }
    /* 写回每个 entry */
    for (int i = 0; i < n; i++) {
        int r = uf_find(lex->uf_parent, i);
        int cid = GPOINTER_TO_INT(g_hash_table_lookup(root2cid,
                                                     GINT_TO_POINTER(r)));
        g_array_index(lex->entries, LexEntry, i).class_id = cid;
    }
    lex->class_count = roots->len;
    g_array_free(roots, TRUE);
    g_hash_table_destroy(root2cid);
}

/* ─── API ─────────────────────────────────────────────────────── */

Lexicon *lexicon_new(void) {
    Lexicon *lex = g_new0(Lexicon, 1);
    lex->entries  = g_array_new(FALSE, FALSE, sizeof(LexEntry));
    lex->text2idx = g_hash_table_new(g_str_hash, g_str_equal);
    lex->uf_parent= g_array_new(FALSE, FALSE, sizeof(int));
    lex->class_count = 0;
    return lex;
}

void lexicon_free(Lexicon *lex) {
    if (!lex) return;
    for (guint i = 0; i < lex->entries->len; i++) {
        g_free(g_array_index(lex->entries, LexEntry, i).text);
    }
    g_array_free(lex->entries, TRUE);
    g_array_free(lex->uf_parent, TRUE);
    g_hash_table_destroy(lex->text2idx);
    g_free(lex);
}

int lexicon_add(Lexicon *lex, const char *text) {
    if (!lex || !text || !*text) return -1;
    gpointer hit;
    if (g_hash_table_lookup_extended(lex->text2idx, text, NULL, &hit))
        return GPOINTER_TO_INT(hit);
    LexEntry e = { .text = g_strdup(text), .class_id = 0 };
    g_array_append_val(lex->entries, e);
    int idx = (int)lex->entries->len - 1;
    g_array_append_val(lex->uf_parent, idx);     /* 自己作为根 */
    g_hash_table_insert(lex->text2idx,
                        g_array_index(lex->entries, LexEntry, idx).text,
                        GINT_TO_POINTER(idx));
    recompact_classes(lex);
    return idx;
}

gboolean lexicon_remove_at(Lexicon *lex, int idx) {
    if (!lex || idx < 0 || idx >= (int)lex->entries->len) return FALSE;
    LexEntry *e = &g_array_index(lex->entries, LexEntry, idx);
    g_hash_table_remove(lex->text2idx, e->text);
    g_free(e->text);
    g_array_remove_index(lex->entries, idx);
    g_array_remove_index(lex->uf_parent, idx);
    /* parent 数组里凡是 >idx 的项需要 -1；指向 idx 的需要重指：
     * 简单做法：所有节点重新 find 后回写新 id。先把 >idx 的 parent 减 1，
     * 同时凡是 == idx 的根直接改成自己（脱链），再重算。 */
    int *p = (int *)lex->uf_parent->data;
    int  n = (int)lex->uf_parent->len;
    for (int i = 0; i < n; i++) {
        if (p[i] == idx) p[i] = i;        /* 该子树孤立 */
        else if (p[i] > idx) p[i] -= 1;
    }
    /* 同步刷新哈希表里 idx 之后的偏移 */
    g_hash_table_remove_all(lex->text2idx);
    for (int i = 0; i < n; i++) {
        g_hash_table_insert(lex->text2idx,
            g_array_index(lex->entries, LexEntry, i).text,
            GINT_TO_POINTER(i));
    }
    recompact_classes(lex);
    return TRUE;
}

void lexicon_merge(Lexicon *lex, const int *entry_indices, int n) {
    if (!lex || !entry_indices || n < 2) return;
    int total = (int)lex->entries->len;
    int base = -1;
    for (int i = 0; i < n; i++) {
        int v = entry_indices[i];
        if (v < 0 || v >= total) continue;
        if (base < 0) { base = v; continue; }
        uf_union(lex->uf_parent, base, v);
    }
    recompact_classes(lex);
}

void lexicon_split(Lexicon *lex, int idx) {
    if (!lex || idx < 0 || idx >= (int)lex->entries->len) return;
    /* 把 idx 自己作为新根脱出，且其他指向它的子节点要重新挂回当前类的另一成员。 */
    int *p = (int *)lex->uf_parent->data;
    int  n = (int)lex->uf_parent->len;
    int  old_root = uf_find(lex->uf_parent, idx);
    /* 找到同类中除 idx 外的任一成员作为新根候选 */
    int  alt = -1;
    for (int i = 0; i < n; i++) {
        if (i == idx) continue;
        if (uf_find(lex->uf_parent, i) == old_root) { alt = i; break; }
    }
    if (alt < 0) {           /* idx 已经独占一类，直接重算即可 */
        recompact_classes(lex);
        return;
    }
    /* 让 alt 成为新根 */
    p[alt] = alt;
    for (int i = 0; i < n; i++) {
        if (i == idx) continue;
        if (p[i] == old_root && i != alt) p[i] = alt;
    }
    if (old_root != idx && old_root != alt) p[old_root] = alt;
    p[idx] = idx;            /* idx 独立成根 */
    recompact_classes(lex);
}

int lexicon_size(const Lexicon *lex) {
    return lex ? (int)lex->entries->len : 0;
}

const LexEntry *lexicon_at(const Lexicon *lex, int idx) {
    if (!lex || idx < 0 || idx >= (int)lex->entries->len) return NULL;
    return &g_array_index(lex->entries, LexEntry, idx);
}

int lexicon_class_count(const Lexicon *lex) {
    return lex ? lex->class_count : 0;
}

int lexicon_lookup_class(const Lexicon *lex, const char *text) {
    if (!lex || !text) return -1;
    gpointer hit;
    if (!g_hash_table_lookup_extended(lex->text2idx, text, NULL, &hit))
        return -1;
    int idx = GPOINTER_TO_INT(hit);
    return g_array_index(lex->entries, LexEntry, idx).class_id;
}

char *lexicon_class_label(const Lexicon *lex, int class_id) {
    if (!lex || class_id < 0 || class_id >= lex->class_count) return NULL;
    GString *s = g_string_new(NULL);
    int first = 1;
    for (guint i = 0; i < lex->entries->len; i++) {
        const LexEntry *e = &g_array_index(lex->entries, LexEntry, i);
        if (e->class_id == class_id) {
            if (!first) g_string_append(s, " | ");
            g_string_append(s, e->text);
            first = 0;
        }
    }
    return g_string_free(s, FALSE);
}
