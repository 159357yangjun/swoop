#include "jsonlite.h"
#include <string.h>
#include <stdio.h>

/* 把一个 Unicode 码点写成 UTF-8 到 out（容量 n，i 为当前写入位置）。 */
static void put_utf8(char *out, size_t n, size_t *i, unsigned int cp)
{
    if (cp <= 0x7F) {
        if (*i + 1 < n) out[(*i)++] = (char)cp;
    } else if (cp <= 0x7FF) {
        if (*i + 2 < n) {
            out[(*i)++] = (char)(0xC0 | (cp >> 6));
            out[(*i)++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp <= 0xFFFF) {
        if (*i + 3 < n) {
            out[(*i)++] = (char)(0xE0 | (cp >> 12));
            out[(*i)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[(*i)++] = (char)(0x80 | (cp & 0x3F));
        }
    } else {
        if (*i + 4 < n) {
            out[(*i)++] = (char)(0xF0 | (cp >> 18));
            out[(*i)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[(*i)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[(*i)++] = (char)(0x80 | (cp & 0x3F));
        }
    }
}

static int hex4(const char *p, unsigned int *out)
{
    unsigned int v = 0;
    for (int k = 0; k < 4; k++) {
        char c = p[k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

int json_get_str(const char *json, const char *key, char *out, size_t n)
{
    if (!json || !key || !out || n == 0) return 0;
    out[0] = 0;

    char pat[160];
    _snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = json;
    for (;;) {
        p = strstr(p, pat);
        if (!p) return 0;
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == ':') { p = q + 1; break; }   /* 确认是键而非值里的同名子串 */
        p += 1;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;

    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            char c = *p;
            if (c == 'u') {
                unsigned int cp;
                if (!hex4(p + 1, &cp)) { p++; continue; }
                p += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    unsigned int lo;
                    if (hex4(p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                put_utf8(out, n, &i, cp);
            } else {
                switch (c) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    default: break;   /* \\ \" \/ 原样 */
                }
                if (i + 1 < n) out[i++] = c;
                p++;
            }
        } else {
            if (i + 1 < n) out[i++] = *p;
            p++;
        }
    }
    out[i] = 0;
    return 1;
}
