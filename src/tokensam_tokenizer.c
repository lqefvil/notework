/**
 * tokensam_tokenizer.c — UTF-8 安全的分词器
 *
 * 行为：
 *   1) 模式 BY_DELIM：sep 中每个 Unicode 字符均视为分隔符候选，任一命中即切分；
 *      sep 为空字符串时退化为「整段输入即单一 token」。
 *   2) 模式 BY_CHAR：忽略 sep，逐 Unicode 字符切分（每个字符即一个 token），
 *      ASCII 空白字符自动跳过。
 *   3) 段内首尾的 ASCII 空白会被 trim（BY_CHAR 模式无 trim 概念）。
 *   4) 段内文本若不在词典中：
 *      - auto_add=TRUE：自动 lexicon_add，新建一个独立等价类，加入 ids 序列；
 *      - auto_add=FALSE：记录 TokenizeError（保留原始字节偏移），跳过该段。
 */
#include "tokensam.h"

#include <string.h>

static int is_ascii_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

/* 返回 [s, e) 子串去掉首尾 ASCII 空白后的新区间；空段返回 e==s。 */
static void trim_range(const char *text, int s, int e, int *out_s, int *out_e) {
    while (s < e && is_ascii_space((unsigned char)text[s])) s++;
    while (e > s && is_ascii_space((unsigned char)text[e-1])) e--;
    *out_s = s; *out_e = e;
}

/* 前向声明：emit_segment 在 fallback 路径上需要调用 try_resplit_with_lex */
static gboolean try_resplit_with_lex(Lexicon *lex, const char *text,
                                     int ts, int te, GArray *ids_buf);

/* 把 ts..te 段尝试入 ids；未识别则视 auto_add 决定是否自动入典或记错。 */
static void emit_segment(Lexicon *lex, const char *text, int ts, int te,
                         GArray *ids_buf, GArray *errors,
                         gboolean auto_add) {
    if (te <= ts) return;
    int seg_byte_len = te - ts;
    char *piece = g_strndup(text + ts, seg_byte_len);

    int cid = lexicon_lookup_class(lex, piece);
    if (cid >= 0) {
        g_array_append_val(ids_buf, cid);
        g_free(piece);
        return;
    }
    /* 整段未命中：先尝试用「词典已有 token」最长前缀贪心拼回，
     * 防止把「abcw我叫小明」这种漏分隔符的输入再次整段入典造成脏数据。 */
    if (try_resplit_with_lex(lex, text, ts, te, ids_buf)) {
        g_free(piece);
        return;
    }
    if (auto_add) {
        lexicon_add(lex, piece);
        cid = lexicon_lookup_class(lex, piece);
        if (cid >= 0) {
            g_array_append_val(ids_buf, cid);
            g_free(piece);
            return;
        }
        /* 入典失败的极端兜底：仍记一次错误 */
    }
    TokenizeError err = {
        .byte_offset = ts,
        .byte_len    = seg_byte_len,
        .piece       = piece,
    };
    g_array_append_val(errors, err);
}

/* 检查 sep 中是否含某 Unicode 字符。 */
static gboolean sep_contains(const char *sep, gunichar c) {
    if (!sep || !*sep) return FALSE;
    const char *p = sep;
    while (*p) {
        gunichar u = g_utf8_get_char(p);
        if (u == c) return TRUE;
        p = g_utf8_next_char(p);
    }
    return FALSE;
}

/* 在 lex 中查找以 (p, max_len) 开头的最长词条；返回 entry index，未匹配返回 -1，
 * out_byte_len 写出匹配的字节长度。 */
static int lex_longest_prefix(Lexicon *lex, const char *p, int max_len,
                              int *out_byte_len) {
    int best_idx = -1, best_len = 0;
    int n = lexicon_size(lex);
    for (int i = 0; i < n; i++) {
        const LexEntry *e = lexicon_at(lex, i);
        if (!e || !e->text) continue;
        int el = (int)strlen(e->text);
        if (el == 0 || el > max_len || el <= best_len) continue;
        if (memcmp(p, e->text, el) == 0) {
            best_idx = i; best_len = el;
        }
    }
    if (out_byte_len) *out_byte_len = best_len;
    return best_idx;
}

/* 尝试用「词典中已有 token」按最长前缀贪心方式完整覆盖 [text+ts, text+te)。
 * 完整覆盖时把切出的 cid 依序追加到 ids_buf 并返回 TRUE；
 * 任意位置无法匹配则什么也不写，返回 FALSE（交给上层 auto_add / error）。 */
static gboolean try_resplit_with_lex(Lexicon *lex, const char *text,
                                     int ts, int te, GArray *ids_buf) {
    int pos = ts;
    GArray *tmp = g_array_new(FALSE, FALSE, sizeof(int));
    while (pos < te) {
        int m_len = 0;
        int idx = lex_longest_prefix(lex, text + pos, te - pos, &m_len);
        if (idx < 0 || m_len <= 0) { g_array_free(tmp, TRUE); return FALSE; }
        const LexEntry *e = lexicon_at(lex, idx);
        int cid = e->class_id;
        g_array_append_val(tmp, cid);
        pos += m_len;
    }
    /* 至少切成 2 段才算「重切」有意义；只切成 1 段说明等于整段查中（理论上前面已命中） */
    if (tmp->len < 2) { g_array_free(tmp, TRUE); return FALSE; }
    for (guint i = 0; i < tmp->len; i++)
        g_array_append_val(ids_buf, g_array_index(tmp, int, i));
    g_array_free(tmp, TRUE);
    return TRUE;
}

TokenizeResult *tokenize(Lexicon *lex, const char *text, const char *sep,
                         TokenizeMode mode, gboolean auto_add) {
    TokenizeResult *r = g_new0(TokenizeResult, 1);
    r->errors = g_array_new(FALSE, FALSE, sizeof(TokenizeError));
    if (!text) {
        r->ids = NULL; r->n = 0; return r;
    }
    GArray *ids_buf = g_array_new(FALSE, FALSE, sizeof(int));

    if (mode == TOKENIZE_BY_CHAR) {
        /* 逐 Unicode 字符切；跳过 ASCII 空白；非 ASCII 空白（含中文标点）保留为独立 token。 */
        const char *p = text;
        while (*p) {
            const char *next = g_utf8_next_char(p);
            int byte_len = (int)(next - p);
            int byte_off = (int)(p - text);
            /* 跳过 ASCII 空白 */
            if (byte_len == 1 && is_ascii_space((unsigned char)*p)) {
                p = next;
                continue;
            }
            emit_segment(lex, text, byte_off, byte_off + byte_len,
                         ids_buf, r->errors, auto_add);
            p = next;
        }
    } else if (mode == TOKENIZE_BY_LEXICON) {
        /* 词典最长前缀贪心切分。
         * 算法：扫描指针 pos 从 0 到 text_len。
         *   1) 跳过 ASCII 空白；
         *   2) 若以 text+pos 开头有词典前缀命中（最长 m_len）：
         *        若此前累积有「未识别段」[unrec_start, pos)，先记入 errors；
         *        emit cid，pos += m_len；
         *   3) 否则：若 unrec_start < 0 则置为 pos；按 UTF-8 字符前进。
         * 末尾若仍有未识别段，整段记入 errors。
         * 注意：BY_LEXICON 下 auto_add=TRUE 仍尊重原语义——把未识别段直接 lexicon_add
         * 成新条目；auto_add=FALSE 则只记 errors，由 UI 决定是否弹窗确认。 */
        int text_len = (int)strlen(text);
        int pos = 0;
        int unrec_start = -1;
        while (pos < text_len) {
            /* 跳过 ASCII 空白：同时也终结当前未识别段 */
            if (is_ascii_space((unsigned char)text[pos])) {
                if (unrec_start >= 0) {
                    int ts, te;
                    trim_range(text, unrec_start, pos, &ts, &te);
                    if (te > ts) emit_segment(lex, text, ts, te,
                                              ids_buf, r->errors, auto_add);
                    unrec_start = -1;
                }
                pos++;
                continue;
            }
            int m_len = 0;
            int idx = lex_longest_prefix(lex, text + pos, text_len - pos, &m_len);
            if (idx >= 0 && m_len > 0) {
                if (unrec_start >= 0) {
                    int ts, te;
                    trim_range(text, unrec_start, pos, &ts, &te);
                    if (te > ts) emit_segment(lex, text, ts, te,
                                              ids_buf, r->errors, auto_add);
                    unrec_start = -1;
                }
                int cid = lexicon_at(lex, idx)->class_id;
                g_array_append_val(ids_buf, cid);
                pos += m_len;
            } else {
                if (unrec_start < 0) unrec_start = pos;
                /* 按 UTF-8 字符前进，避免切到字符中段 */
                const char *p = text + pos;
                const char *nx = g_utf8_next_char(p);
                int adv = (int)(nx - p);
                if (adv <= 0) adv = 1;
                pos += adv;
            }
        }
        if (unrec_start >= 0) {
            int ts, te;
            trim_range(text, unrec_start, pos, &ts, &te);
            if (te > ts) emit_segment(lex, text, ts, te,
                                      ids_buf, r->errors, auto_add);
        }
    } else {
        /* BY_DELIM：sep 为空 → 整段作单 token；否则任一 sep 字符触发切分。 */
        int text_len = (int)strlen(text);
        if (!sep || !*sep) {
            int ts, te;
            trim_range(text, 0, text_len, &ts, &te);
            emit_segment(lex, text, ts, te, ids_buf, r->errors, auto_add);
        } else {
            int seg_start = 0;
            const char *p = text;
            while (*p) {
                gunichar u = g_utf8_get_char(p);
                const char *next = g_utf8_next_char(p);
                int p_off = (int)(p - text);
                if (sep_contains(sep, u)) {
                    int ts, te;
                    trim_range(text, seg_start, p_off, &ts, &te);
                    emit_segment(lex, text, ts, te, ids_buf, r->errors, auto_add);
                    seg_start = (int)(next - text);
                }
                p = next;
            }
            /* 收尾段 */
            int ts, te;
            trim_range(text, seg_start, text_len, &ts, &te);
            emit_segment(lex, text, ts, te, ids_buf, r->errors, auto_add);
        }
    }

    r->n = ids_buf->len;
    r->ids = (int *)g_array_free(ids_buf, FALSE);
    return r;
}

void tokenize_result_free(TokenizeResult *r) {
    if (!r) return;
    g_free(r->ids);
    if (r->errors) {
        for (guint i = 0; i < r->errors->len; i++) {
            g_free(g_array_index(r->errors, TokenizeError, i).piece);
        }
        g_array_free(r->errors, TRUE);
    }
    g_free(r);
}
