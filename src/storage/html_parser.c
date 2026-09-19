/*
 * html_parser.c
 * 自研 C99 有限状态机 HTML 解析器 — 完整实现
 *
 * 状态机覆盖：
 *   TEXT  →  TAG_OPEN  →  TAG_NAME  →  ATTR_NAME  →  ATTR_EQ
 *   →  ATTR_VAL_SQ / ATTR_VAL_DQ / ATTR_VAL_UNQUOTED
 *   →  SELF_CLOSE  →  COMMENT_START  →  COMMENT  →  COMMENT_END
 *   →  DOCTYPE  →  SCRIPT/STYLE_CONTENT（跳过内部）
 *
 * 内存策略：每个节点/属性/字符串独立 malloc，html_doc_free 递归释放。
 */

#include "html_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ─────────────────────────────────────────────
 * 内部辅助：可增长字符串缓冲
 * ───────────────────────────────────────────── */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} StrBuf;

static void strbuf_init(StrBuf *s) {
    s->cap = 64;
    s->buf = (char*)malloc(s->cap);
    s->buf[0] = '\0';
    s->len = 0;
}

static void strbuf_push(StrBuf *s, char c) {
    if (s->len + 1 >= s->cap) {
        s->cap *= 2;
        s->buf = (char*)realloc(s->buf, s->cap);
    }
    s->buf[s->len++] = c;
    s->buf[s->len]   = '\0';
}

static char *strbuf_take(StrBuf *s) {
    /* 返回当前内容的独立 malloc 副本，并重置缓冲 */
    char *r = (char*)malloc(s->len + 1);
    memcpy(r, s->buf, s->len + 1);
    s->len     = 0;
    s->buf[0]  = '\0';
    return r;
}

static void strbuf_free(StrBuf *s) { free(s->buf); s->buf = NULL; }

/* 原地转小写 */
static void str_tolower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

/* ─────────────────────────────────────────────
 * 节点/属性分配
 * ───────────────────────────────────────────── */
static HtmlNode *node_alloc(NodeType type) {
    HtmlNode *n = (HtmlNode*)calloc(1, sizeof(HtmlNode));
    n->type = type;
    return n;
}

static HtmlAttr *attr_alloc(const char *name, const char *value) {
    HtmlAttr *a = (HtmlAttr*)calloc(1, sizeof(HtmlAttr));
    a->name  = name  ? _strdup(name)  : _strdup("");
    a->value = value ? _strdup(value) : _strdup("");
    return a;
}

/* 将子节点 child 追加到 parent */
static void node_append_child(HtmlNode *parent, HtmlNode *child) {
    child->parent = parent;
    if (!parent->first_child) {
        parent->first_child = parent->last_child = child;
    } else {
        child->prev_sibling = parent->last_child;
        parent->last_child->next_sibling = child;
        parent->last_child = child;
    }
}

/* ─────────────────────────────────────────────
 * 状态枚举
 * ───────────────────────────────────────────── */
typedef enum {
    S_TEXT,
    S_TAG_OPEN,
    S_TAG_NAME,
    S_END_TAG_NAME,     /* </xxx> */
    S_SELF_CLOSE,
    S_ATTR_BEFORE,      /* 标签名之后、属性之前的空白 */
    S_ATTR_NAME,
    S_ATTR_EQ,
    S_ATTR_VAL_DQ,
    S_ATTR_VAL_SQ,
    S_ATTR_VAL_UNQUOTED,
    S_COMMENT_START1,   /* < ! */
    S_COMMENT_START2,   /* < ! - */
    S_COMMENT,
    S_COMMENT_END1,     /* - */
    S_COMMENT_END2,     /* - - */
    S_DOCTYPE,
    S_SCRIPT_STYLE      /* 跳过 script/style 内部 */
} ParseState;

/* 无需解析内部内容的标签 */
static int is_raw_tag(const char *tag) {
    return strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0;
}

/* 空元素（不能有子节点） */
static int is_void_tag(const char *tag) {
    static const char *voids[] = {
        "area","base","br","col","embed","hr","img","input",
        "link","meta","param","source","track","wbr", NULL
    };
    for (int i = 0; voids[i]; i++)
        if (strcmp(tag, voids[i]) == 0) return 1;
    return 0;
}

/* ─────────────────────────────────────────────
 * 主解析函数
 * ───────────────────────────────────────────── */
HtmlDoc *html_parse(const char *html, size_t len) {
    if (!html || len == 0) return NULL;

    HtmlDoc  *doc  = (HtmlDoc*)calloc(1, sizeof(HtmlDoc));
    HtmlNode *root = node_alloc(NODE_ELEMENT);
    root->tag      = _strdup("_root_");
    doc->root      = root;

    /* 解析栈（最深支持 256 层嵌套） */
    HtmlNode *stack[256];
    int top = 0;
    stack[top] = root;

    ParseState state = S_TEXT;
    StrBuf     tbuf, nbuf, kbuf, vbuf; /* text / tagname / attrkey / attrval */
    strbuf_init(&tbuf);
    strbuf_init(&nbuf);
    strbuf_init(&kbuf);
    strbuf_init(&vbuf);

    /* 当前正在构建的节点 */
    HtmlNode *cur_node = NULL;
    int       is_end_tag = 0;

    /* script/style 跳过时记住标签名 */
    char raw_close[64] = {0};
    size_t raw_close_match = 0;

    for (size_t i = 0; i < len; i++) {
        char c = html[i];

        /* ── script/style 特殊跳过模式 ── */
        if (state == S_SCRIPT_STYLE) {
            /* 等待匹配 </tagname> */
            if (c == raw_close[raw_close_match]) {
                raw_close_match++;
                if (raw_close[raw_close_match] == '\0') {
                    state = S_TEXT;
                    raw_close_match = 0;
                }
            } else {
                raw_close_match = 0;
                if (c == raw_close[0]) raw_close_match = 1;
            }
            continue;
        }

        switch (state) {

        /* ── 文本收集 ── */
        case S_TEXT:
            if (c == '<') {
                /* 提交文本节点 */
                if (tbuf.len > 0) {
                    /* 去掉纯空白文本节点 */
                    int all_ws = 1;
                    for (size_t k = 0; k < tbuf.len; k++) {
                        if (!isspace((unsigned char)tbuf.buf[k])) { all_ws = 0; break; }
                    }
                    if (!all_ws) {
                        HtmlNode *tn = node_alloc(NODE_TEXT);
                        tn->text = strbuf_take(&tbuf);
                        node_append_child(stack[top], tn);
                        doc->node_count++;
                    } else {
                        tbuf.len = 0; tbuf.buf[0] = '\0';
                    }
                }
                state = S_TAG_OPEN;
                is_end_tag = 0;
            } else {
                strbuf_push(&tbuf, c);
            }
            break;

        /* ── < 之后 ── */
        case S_TAG_OPEN:
            if (c == '!') {
                state = S_COMMENT_START1;
            } else if (c == '/') {
                state = S_END_TAG_NAME;
                is_end_tag = 1;
            } else if (isalpha((unsigned char)c) || c == '_') {
                strbuf_push(&nbuf, c);
                state = S_TAG_NAME;
                is_end_tag = 0;
            } else {
                /* 不是合法标签，当文本处理 */
                strbuf_push(&tbuf, '<');
                strbuf_push(&tbuf, c);
                state = S_TEXT;
            }
            break;

        /* ── 结束标签名 </xxx> ── */
        case S_END_TAG_NAME:
            if (isalnum((unsigned char)c) || c == '-' || c == '_') {
                strbuf_push(&nbuf, c);
            } else if (c == '>') {
                char *name = strbuf_take(&nbuf);
                str_tolower(name);
                /* 弹栈，找到匹配的开始标签 */
                for (int t = top; t > 0; t--) {
                    if (stack[t]->tag && strcmp(stack[t]->tag, name) == 0) {
                        top = t - 1;
                        break;
                    }
                }
                free(name);
                state = S_TEXT;
            }
            /* 忽略结束标签内属性（容错） */
            break;

        /* ── 标签名收集 ── */
        case S_TAG_NAME:
            if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == ':') {
                strbuf_push(&nbuf, c);
            } else {
                /* 标签名结束，创建节点 */
                char *name = strbuf_take(&nbuf);
                str_tolower(name);
                cur_node = node_alloc(NODE_ELEMENT);
                cur_node->tag = name;
                doc->node_count++;

                if (isspace((unsigned char)c)) {
                    state = S_ATTR_BEFORE;
                } else if (c == '>') {
                    /* 立即闭合 */
                    node_append_child(stack[top], cur_node);
                    if (!is_void_tag(cur_node->tag)) {
                        if (top < 255) stack[++top] = cur_node;
                        if (is_raw_tag(cur_node->tag)) {
                            snprintf(raw_close, sizeof(raw_close),
                                "</%s>", cur_node->tag);
                            raw_close_match = 0;
                            state = S_SCRIPT_STYLE;
                        } else {
                            state = S_TEXT;
                        }
                    } else {
                        state = S_TEXT;
                    }
                    cur_node = NULL;
                } else if (c == '/') {
                    state = S_SELF_CLOSE;
                } else {
                    state = S_ATTR_BEFORE;
                }
            }
            break;

        /* ── 属性前空白 ── */
        case S_ATTR_BEFORE:
            if (isspace((unsigned char)c)) { /* skip */ }
            else if (c == '>') {
                if (cur_node) {
                    node_append_child(stack[top], cur_node);
                    if (!is_void_tag(cur_node->tag)) {
                        if (top < 255) stack[++top] = cur_node;
                        if (is_raw_tag(cur_node->tag)) {
                            snprintf(raw_close, sizeof(raw_close),
                                "</%s>", cur_node->tag);
                            raw_close_match = 0;
                            state = S_SCRIPT_STYLE;
                        } else {
                            state = S_TEXT;
                        }
                    } else {
                        state = S_TEXT;
                    }
                    cur_node = NULL;
                } else {
                    state = S_TEXT;
                }
            } else if (c == '/') {
                state = S_SELF_CLOSE;
            } else {
                strbuf_push(&kbuf, c);
                state = S_ATTR_NAME;
            }
            break;

        /* ── 属性名 ── */
        case S_ATTR_NAME:
            if (c == '=') {
                state = S_ATTR_EQ;
            } else if (isspace((unsigned char)c) || c == '/' || c == '>') {
                /* 布尔属性（无值） */
                char *k = strbuf_take(&kbuf);
                str_tolower(k);
                if (cur_node) {
                    HtmlAttr *a = attr_alloc(k, "");
                    a->next = cur_node->attrs;
                    cur_node->attrs = a;
                }
                free(k);
                if (c == '/')       state = S_SELF_CLOSE;
                else if (c == '>')  goto emit_tag;
                else                state = S_ATTR_BEFORE;
            } else {
                strbuf_push(&kbuf, c);
            }
            break;

        /* ── = 之后决定引号类型 ── */
        case S_ATTR_EQ:
            if (c == '"')       state = S_ATTR_VAL_DQ;
            else if (c == '\'') state = S_ATTR_VAL_SQ;
            else if (!isspace((unsigned char)c)) {
                strbuf_push(&vbuf, c);
                state = S_ATTR_VAL_UNQUOTED;
            }
            break;

        /* ── 双引号属性值 ── */
        case S_ATTR_VAL_DQ:
            if (c == '"') {
                char *k = strbuf_take(&kbuf);
                char *v = strbuf_take(&vbuf);
                str_tolower(k);
                if (cur_node) {
                    HtmlAttr *a = attr_alloc(k, v);
                    a->next = cur_node->attrs;
                    cur_node->attrs = a;
                }
                free(k); free(v);
                state = S_ATTR_BEFORE;
            } else {
                strbuf_push(&vbuf, c);
            }
            break;

        /* ── 单引号属性值 ── */
        case S_ATTR_VAL_SQ:
            if (c == '\'') {
                char *k = strbuf_take(&kbuf);
                char *v = strbuf_take(&vbuf);
                str_tolower(k);
                if (cur_node) {
                    HtmlAttr *a = attr_alloc(k, v);
                    a->next = cur_node->attrs;
                    cur_node->attrs = a;
                }
                free(k); free(v);
                state = S_ATTR_BEFORE;
            } else {
                strbuf_push(&vbuf, c);
            }
            break;

        /* ── 无引号属性值 ── */
        case S_ATTR_VAL_UNQUOTED:
            if (isspace((unsigned char)c) || c == '>' || c == '/') {
                char *k = strbuf_take(&kbuf);
                char *v = strbuf_take(&vbuf);
                str_tolower(k);
                if (cur_node) {
                    HtmlAttr *a = attr_alloc(k, v);
                    a->next = cur_node->attrs;
                    cur_node->attrs = a;
                }
                free(k); free(v);
                if (c == '/')      state = S_SELF_CLOSE;
                else if (c == '>') goto emit_tag;
                else               state = S_ATTR_BEFORE;
            } else {
                strbuf_push(&vbuf, c);
            }
            break;

        /* ── 自闭合 /> ── */
        case S_SELF_CLOSE:
            if (c == '>') {
                emit_tag:
                if (cur_node) {
                    node_append_child(stack[top], cur_node);
                    /* 自闭合 / 空元素不压栈 */
                    if (state != S_SELF_CLOSE && !is_void_tag(cur_node->tag)) {
                        if (top < 255) stack[++top] = cur_node;
                        if (is_raw_tag(cur_node->tag)) {
                            snprintf(raw_close, sizeof(raw_close),
                                "</%s>", cur_node->tag);
                            raw_close_match = 0;
                            state = S_SCRIPT_STYLE;
                        } else {
                            state = S_TEXT;
                        }
                    } else {
                        state = S_TEXT;
                    }
                    cur_node = NULL;
                } else {
                    state = S_TEXT;
                }
            }
            break;

        /* ── 注释 <!-- ── */
        case S_COMMENT_START1:
            if (c == '-') state = S_COMMENT_START2;
            else if (c == 'D' || c == 'd') state = S_DOCTYPE;
            else state = S_TEXT;
            break;
        case S_COMMENT_START2:
            if (c == '-') state = S_COMMENT;
            else state = S_TEXT;
            break;
        case S_COMMENT:
            if (c == '-') state = S_COMMENT_END1;
            break;
        case S_COMMENT_END1:
            state = (c == '-') ? S_COMMENT_END2 : S_COMMENT;
            break;
        case S_COMMENT_END2:
            if (c == '>') state = S_TEXT;
            else if (c != '-') state = S_COMMENT;
            break;

        /* ── DOCTYPE（直接跳过到 >） ── */
        case S_DOCTYPE:
            if (c == '>') state = S_TEXT;
            break;

        default:
            break;
        }
    }

    strbuf_free(&tbuf);
    strbuf_free(&nbuf);
    strbuf_free(&kbuf);
    strbuf_free(&vbuf);
    return doc;
}

/* ─────────────────────────────────────────────
 * 内存释放
 * ───────────────────────────────────────────── */
static void node_free_recursive(HtmlNode *n) {
    if (!n) return;
    /* 先释放属性链表 */
    HtmlAttr *a = n->attrs;
    while (a) {
        HtmlAttr *next = a->next;
        free(a->name); free(a->value); free(a);
        a = next;
    }
    /* 递归释放子节点 */
    HtmlNode *child = n->first_child;
    while (child) {
        HtmlNode *next = child->next_sibling;
        node_free_recursive(child);
        child = next;
    }
    free(n->tag);
    free(n->text);
    free(n);
}

void html_doc_free(HtmlDoc *doc) {
    if (!doc) return;
    node_free_recursive(doc->root);
    free(doc);
}

/* ─────────────────────────────────────────────
 * 查询实现
 * ───────────────────────────────────────────── */

/* 动态数组辅助（仅内部使用） */
typedef struct { HtmlNode **arr; int len; int cap; } NodeArr;
static void narr_push(NodeArr *a, HtmlNode *n) {
    if (a->len >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->arr = (HtmlNode**)realloc(a->arr, a->cap * sizeof(HtmlNode*));
    }
    a->arr[a->len++] = n;
}

static void find_by_tag_rec(HtmlNode *node, const char *tag, NodeArr *result) {
    if (!node) return;
    if (node->type == NODE_ELEMENT && node->tag && strcmp(node->tag, tag) == 0)
        narr_push(result, node);
    HtmlNode *c = node->first_child;
    while (c) { find_by_tag_rec(c, tag, result); c = c->next_sibling; }
}

HtmlNode **html_find_by_tag(HtmlNode *root, const char *tag) {
    char lower_tag[64];
    strncpy(lower_tag, tag, sizeof(lower_tag) - 1);
    lower_tag[sizeof(lower_tag)-1] = '\0';
    str_tolower(lower_tag);

    NodeArr result = {NULL, 0, 0};
    find_by_tag_rec(root, lower_tag, &result);
    narr_push(&result, NULL); /* NULL 终止 */
    return result.arr;
}

static HtmlNode *find_by_id_rec(HtmlNode *node, const char *id) {
    if (!node) return NULL;
    if (node->type == NODE_ELEMENT) {
        const char *v = html_get_attr(node, "id");
        if (v && strcmp(v, id) == 0) return node;
    }
    HtmlNode *c = node->first_child;
    while (c) {
        HtmlNode *r = find_by_id_rec(c, id);
        if (r) return r;
        c = c->next_sibling;
    }
    return NULL;
}

HtmlNode *html_find_by_id(HtmlNode *root, const char *id) {
    return find_by_id_rec(root, id);
}

static int has_class(HtmlNode *node, const char *classname) {
    const char *cls = html_get_attr(node, "class");
    if (!cls) return 0;
    /* class 属性可能包含多个类名，用空格分隔 */
    const char *p = cls;
    size_t clen = strlen(classname);
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        const char *end = p;
        while (*end && !isspace((unsigned char)*end)) end++;
        if ((size_t)(end - p) == clen && strncmp(p, classname, clen) == 0)
            return 1;
        p = end;
    }
    return 0;
}

static void find_by_class_rec(HtmlNode *node, const char *cls, NodeArr *result) {
    if (!node) return;
    if (node->type == NODE_ELEMENT && has_class(node, cls))
        narr_push(result, node);
    HtmlNode *c = node->first_child;
    while (c) { find_by_class_rec(c, cls, result); c = c->next_sibling; }
}

HtmlNode **html_find_by_class(HtmlNode *root, const char *classname) {
    NodeArr result = {NULL, 0, 0};
    find_by_class_rec(root, classname, &result);
    narr_push(&result, NULL);
    return result.arr;
}

const char *html_get_attr(HtmlNode *node, const char *name) {
    if (!node) return NULL;
    char lower[64];
    strncpy(lower, name, sizeof(lower) - 1);
    lower[sizeof(lower)-1] = '\0';
    str_tolower(lower);
    for (HtmlAttr *a = node->attrs; a; a = a->next)
        if (strcmp(a->name, lower) == 0) return a->value;
    return NULL;
}

/* ─────────────────────────────────────────────
 * URL 提取
 * ───────────────────────────────────────────── */
typedef struct { UrlItem *head; UrlItem *tail; } UrlList;

static void url_push(UrlList *lst, const char *url, const char *type) {
    if (!url || url[0] == '\0') return;
    /* 跳过相对路径锚点和 js: 伪协议 */
    if (url[0] == '#') return;
    if (strncmp(url, "javascript:", 11) == 0) return;
    UrlItem *it = (UrlItem*)calloc(1, sizeof(UrlItem));
    it->url  = _strdup(url);
    it->type = _strdup(type);
    if (!lst->head) lst->head = lst->tail = it;
    else { lst->tail->next = it; lst->tail = it; }
}

/* 从 style 属性值里抓 url(...) */
static void extract_style_urls(UrlList *lst, const char *style) {
    if (!style) return;
    const char *p = style;
    while ((p = strstr(p, "url(")) != NULL) {
        p += 4;
        while (isspace((unsigned char)*p)) p++;
        char q = 0;
        if (*p == '"' || *p == '\'') q = *p++;
        const char *start = p;
        while (*p && (q ? *p != q : *p != ')')) p++;
        if (p > start) {
            char *u = (char*)malloc(p - start + 1);
            memcpy(u, start, p - start);
            u[p - start] = '\0';
            url_push(lst, u, "style-url");
            free(u);
        }
    }
}

/* 媒体相关标签和属性白名单 */
static const struct { const char *tag; const char *attr; int media_only; } URL_MAP[] = {
    {"a",       "href",    0},
    {"img",     "src",     0},
    {"img",     "data-src",0},
    {"video",   "src",     1},
    {"video",   "poster",  1},
    {"audio",   "src",     1},
    {"source",  "src",     1},
    {"source",  "srcset",  1},
    {"track",   "src",     1},
    {"link",    "href",    0},
    {"script",  "src",     0},
    {"iframe",  "src",     0},
    {"embed",   "src",     0},
    {"object",  "data",    0},
    {"form",    "action",  0},
    {NULL, NULL, 0}
};

static void extract_urls_rec(HtmlNode *node, UrlList *lst, int media_only) {
    if (!node) return;
    if (node->type == NODE_ELEMENT && node->tag) {
        for (int i = 0; URL_MAP[i].tag; i++) {
            if (strcmp(node->tag, URL_MAP[i].tag) != 0) continue;
            if (media_only && !URL_MAP[i].media_only) continue;
            const char *v = html_get_attr(node, URL_MAP[i].attr);
            if (v) url_push(lst, v, URL_MAP[i].attr);
        }
        /* style 属性内的 url() */
        if (!media_only) {
            const char *st = html_get_attr(node, "style");
            if (st) extract_style_urls(lst, st);
        }
    }
    HtmlNode *c = node->first_child;
    while (c) { extract_urls_rec(c, lst, media_only); c = c->next_sibling; }
}

UrlItem *html_extract_urls(HtmlDoc *doc) {
    if (!doc) return NULL;
    UrlList lst = {NULL, NULL};
    extract_urls_rec(doc->root, &lst, 0);
    return lst.head;
}

UrlItem *html_extract_media_urls(HtmlDoc *doc) {
    if (!doc) return NULL;
    UrlList lst = {NULL, NULL};
    extract_urls_rec(doc->root, &lst, 1);
    return lst.head;
}

void html_url_list_free(UrlItem *head) {
    while (head) {
        UrlItem *next = head->next;
        free(head->url); free(head->type); free(head);
        head = next;
    }
}

/* ─────────────────────────────────────────────
 * 调试打印
 * ───────────────────────────────────────────── */
void html_dump(HtmlNode *node, int depth) {
    if (!node) return;
    for (int i = 0; i < depth * 2; i++) putchar(' ');
    if (node->type == NODE_TEXT) {
        printf("[TEXT] \"%s\"\n", node->text ? node->text : "");
    } else {
        printf("<%s", node->tag ? node->tag : "?");
        for (HtmlAttr *a = node->attrs; a; a = a->next)
            printf(" %s=\"%s\"", a->name, a->value);
        printf(">\n");
    }
    HtmlNode *c = node->first_child;
    while (c) { html_dump(c, depth + 1); c = c->next_sibling; }
}

/* ─────────────────────────────────────────────
 * HTML 实体解码
 * ───────────────────────────────────────────── */
char *html_decode_entities(const char *str) {
    if (!str) return _strdup("");
    /* 最坏情况：每个字符都展开 */
    char *out = (char*)malloc(strlen(str) * 6 + 1);
    int oi = 0;
    for (size_t i = 0; str[i]; i++) {
        if (str[i] == '&') {
            if (strncmp(str+i, "&amp;", 5)==0)       { out[oi++] = '&'; i += 4; }
            else if (strncmp(str+i, "&lt;", 4)==0)   { out[oi++] = '<'; i += 3; }
            else if (strncmp(str+i, "&gt;", 4)==0)   { out[oi++] = '>'; i += 3; }
            else if (strncmp(str+i, "&quot;",6)==0)  { out[oi++] = '"'; i += 5; }
            else if (strncmp(str+i, "&apos;",6)==0)  { out[oi++] = '\'';i += 5; }
            else if (strncmp(str+i, "&nbsp;",6)==0)  { out[oi++] = ' '; i += 5; }
            else if (str[i+1] == '#') {
                /* 数字实体 &#NNN; 或 &#xHH; */
                int base = 10, val = 0;
                size_t start = i + 2;
                if (str[start] == 'x' || str[start] == 'X') { base = 16; start++; }
                for (size_t j = start; str[j]; j++) {
                    if (str[j] == ';') { i = j; break; }
                    int d = 0;
                    if (str[j] >= '0' && str[j] <= '9') d = str[j]-'0';
                    else if (base==16 && str[j]>='a'&&str[j]<='f') d=str[j]-'a'+10;
                    else if (base==16 && str[j]>='A'&&str[j]<='F') d= str[j]-'A'+10;
                    else break;
                    val = val*base + d;
                }
                out[oi++] = (char)(val > 0 && val < 256 ? val : '?');
            } else {
                out[oi++] = str[i]; /* 不认识的实体原样保留 */
            }
        } else {
            out[oi++] = str[i];
        }
    }
    out[oi] = '\0';
    return out;
}

/* ─────────────────────────────────────────────
 * 相对 URL → 绝对 URL 转换
 * ───────────────────────────────────────────── */

/* 从完整URL中提取协议+主机部分（如 https://example.com） */
static void extract_base_origin(const char *base, char *out, size_t outlen) {
    const char *proto_end = strstr(base, "://");
    if (!proto_end) { out[0]='\0'; return; }
    const char *host_start = proto_end + 3;
    const char *host_end = strchr(host_start, '/');
    size_t len = host_end ? (size_t)(host_end - host_start) : strlen(host_start);
    snprintf(out, outlen, "%.*s", (int)(proto_end - base), base);
    size_t plen = proto_end - base;
    if ((int)outlen > (int)plen + (int)len)
        snprintf(out+plen, outlen-plen, "%.*s", (int)len, host_start);
}

char *html_resolve_url(const char *base, const char *rel) {
    if (!rel || rel[0]=='\0') return _strdup(base ? base : "");
    if (!base || base[0]=='\0') return _strdup(rel);

    /* 已经是绝对URL */
    if (strncmp(rel,"http://",7)==0||strncmp(rel,"https://",8)==0||
        strncmp(rel,"ftp://",6)==0  || strncmp(rel,"//",2)==0) {
        char *r = _strdup(rel);
        if (strncmp(r,"//",2)==0) {
            char tmp[16];
            strncpy(tmp, base, 7); tmp[7]='\0'; /* 取 "https:" 或 "http:" */
            char *full = (char*)malloc(strlen(r)+8);
            sprintf(full, "%s%s", tmp, r);
            free(r); r = full;
        }
        return r;
    }

    /* 锚点和空路径 */
    if (rel[0]=='#') return _strdup(base);

    char origin[512] = "";
    extract_base_origin(base, origin, sizeof(origin));

    /* 提取基础目录路径 */
    const char *last_slash = strrchr(base, '/');
    size_t base_dir_len = last_slash ? (size_t)(last_slash - base + 1) : strlen(base);

    char *result = NULL;

    if (rel[0] == '/') {
        /* 根相对路径：/path/to/file */
        result = (char*)malloc(strlen(origin) + strlen(rel) + 1);
        sprintf(result, "%s%s", origin, rel);
    } else if (rel[0] == '?' || rel[0] == '#') {
        /* 查询/片段替换 */
        const char *qmark = strchr(base, '?');
        const char *hash  = strchr(base, '#');
        size_t cut = qmark ? (size_t)(qmark-base) : (hash?(size_t)(hash-base):strlen(base));
        result = (char*)malloc(cut + strlen(rel) + 1);
        memcpy(result, base, cut);
        strcpy(result+cut, rel);
    } else {
        /* 相对路径：处理 ../ 和 ./ */
        result = (char*)malloc(base_dir_len + strlen(rel) + 4);
        memcpy(result, base, base_dir_len);
        strcpy(result + base_dir_len, rel);

        /* 简化路径：解析 .. 和 . */
        /* 分割为段，然后逐段处理 */
        char parts[128][260];
        int np = 0;
        memset(parts, 0, sizeof(parts));
        char *saveptr;
        char *tok = strtok_s(result, "/", &saveptr);
        while (tok && np < 127) {
            if (strcmp(tok, "..") == 0) {
                if (np > 0) np--;
            } else if (strcmp(tok, ".") != 0) {
                strncpy(parts[np], tok, 259);
                np++;
            }
            tok = strtok_s(NULL, "/", &saveptr);
        }

        /* 重构结果 */
        strcpy(result, origin);
        for (int i = 0; i < np; i++) {
            strcat(result, "/");
            strcat(result, parts[i]);
        }
        if (np == 0) strcat(result, "/");
    }

    /* URL解码 %XX */
    char *decoded = (char*)malloc(strlen(result)+1);
    int di=0;
    for (size_t i=0; result[i]; i++) {
        if (result[i]=='%' && isxdigit((unsigned char)result[i+1])&&isxdigit((unsigned char)result[i+2])) {
            char hex[3]={result[i+1],result[i+2],'\0'};
            decoded[di++]=(char)(int)strtol(hex,NULL,16);
            i+=2;
        } else {
            decoded[di++]=result[i];
        }
    }
    decoded[di]='\0';
    free(result);
    return decoded;
}

/* ─────────────────────────────────────────────
 * m3u8 流媒体链接检测
 * ───────────────────────────────────────────── */
UrlItem *html_detect_m3u8_urls(HtmlDoc *doc) {
    if (!doc) return NULL;
    UrlList lst = {NULL, NULL};

    /* 1. 先用通用媒体提取找所有媒体URL，再过滤 .m3u8/.mpd */
    UrlItem *media = html_extract_media_urls(doc);
    UrlItem *m = media;
    while (m) {
        const char *url = m->url;
        if (url &&
            (strstr(url,".m3u8") || strstr(url,".M3U8") ||
             strstr(url,".mpd")  || strstr(url,".MPD")  ||
             strstr(url,"m3u8")  || strstr(url,"playlist"))) {
            url_push(&lst, url, "m3u8-stream");
        }
        m = m->next;
    }

    /* 2. 遍历所有 <a> 标签查找 m3u8 链接 */
    HtmlNode **links = html_find_by_tag(doc->root, "a");
    for (int i = 0; links && links[i]; i++) {
        const char *href = html_get_attr(links[i], "href");
        if (href && (
            strstr(href, ".m3u8") || strstr(href, ".M3U8") ||
            strstr(href, ".mpd")  || strstr(href, ".MPD"))) {
            url_push(&lst, href, "m3u8-link");
        }
        const char *text = links[i]->first_child ? links[i]->first_child->text : NULL;
        if (text && (strstr(text, ".m3u8") || strstr(text, ".M3U8"))) {
            url_push(&lst, text, "m3u8-text");
        }
    }
    free(links);

    return lst.head;
}
