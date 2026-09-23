#ifndef IDM_JSONLITE_H
#define IDM_JSONLITE_H
#include <stddef.h>

/* 极简 JSON 取值：从 json 文本里取出字符串字段 key 的值，写入 out（UTF-8，已解转义）。
   处理 \\ \" \/ \b \f \n \r \t 与 \uXXXX（含代理对）。
   成功返回 1；未找到该键或值不是字符串返回 0。 */
int json_get_str(const char *json, const char *key, char *out, size_t n);

#endif
