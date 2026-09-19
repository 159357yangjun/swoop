/*
 * html_parser.h
 * HTML 解析器公共头文件
 */

#ifndef HTML_PARSER_H
#define HTML_PARSER_H

#include <stddef.h>

/* 类型定义 */

/* 节点类型 */
typedef enum {
    NODE_ELEMENT,
    NODE_TEXT
} NodeType;

/* 属性链表 */
typedef struct HtmlAttr {
    char *name;
    char *value;
    struct HtmlAttr *next;
} HtmlAttr;

/* HTML 节点 */
typedef struct HtmlNode {
    NodeType type;
    char *tag;           /* 仅 ELEMENT 节点有效 */
    char *text;          /* 仅 TEXT 节点有效 */
    HtmlAttr *attrs;     /* 属性链表头 */
    struct HtmlNode *parent;
    struct HtmlNode *first_child;
    struct HtmlNode *last_child;
    struct HtmlNode *next_sibling;
    struct HtmlNode *prev_sibling;
} HtmlNode;

/* HTML 文档 */
typedef struct {
    HtmlNode *root;
    int node_count;
} HtmlDoc;

/* URL 提取结果链表 */
typedef struct UrlItem {
    char *url;
    char *type;
    struct UrlItem *next;
} UrlItem;

/* 解析接口 */

/* 解析 HTML 字符串，返回 HtmlDoc*（调用者负责 free）*/
HtmlDoc *html_parse(const char *html, size_t len);

/* 释放 HtmlDoc 及其所有子节点 */
void html_doc_free(HtmlDoc *doc);

/* 查询接口 */

/* 按标签名查找，返回 NULL 终止的数组（调用者负责 free）*/
HtmlNode **html_find_by_tag(HtmlNode *root, const char *tag);

/* 按 id 属性查找，返回第一个匹配节点或 NULL */
HtmlNode *html_find_by_id(HtmlNode *root, const char *id);

/* 按 class 属性查找，返回 NULL 终止的数组（调用者负责 free）*/
HtmlNode **html_find_by_class(HtmlNode *root, const char *classname);

/* 获取节点指定属性的值，不存在返回 NULL */
const char *html_get_attr(HtmlNode *node, const char *name);

/* URL 提取接口 */

/* 提取所有 URL（含 a.href、img.src 等）*/
UrlItem *html_extract_urls(HtmlDoc *doc);

/* 仅提取媒体 URL（video/audio/source 等）*/
UrlItem *html_extract_media_urls(HtmlDoc *doc);

/* 释放 UrlItem 链表 */
void html_url_list_free(UrlItem *head);

/* 工具接口 */

/* 调试打印 HTML 树 */
void html_dump(HtmlNode *node, int depth);

/* HTML 实体解码（&amp; → & 等），返回 malloc 字符串 */
char *html_decode_entities(const char *str);

/* 相对 URL → 绝对 URL 转换，返回 malloc 字符串 */
char *html_resolve_url(const char *base, const char *rel);

/* 检测 m3u8/mpd 流媒体链接 */
UrlItem *html_detect_m3u8_urls(HtmlDoc *doc);

#endif /* HTML_PARSER_H */
