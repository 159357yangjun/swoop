#include "category.h"
#include <windows.h>
#include <wchar.h>

/* 一张后缀表，全项目唯一来源。 */
static const wchar_t *k_video[] = { L"mp4", L"mkv", L"avi", L"mov", L"flv", L"wmv",
                                    L"webm", L"m4v", L"mpg", L"mpeg", L"ts", L"3gp", NULL };
static const wchar_t *k_music[] = { L"mp3", L"flac", L"wav", L"aac", L"m4a", L"ogg",
                                    L"wma", L"opus", L"mid", L"ape", NULL };
static const wchar_t *k_archive[] = { L"zip", L"rar", L"7z", L"tar", L"gz", L"bz2",
                                      L"xz", L"iso", L"cab", L"zst", L"tgz", NULL };
static const wchar_t *k_doc[] = { L"pdf", L"doc", L"docx", L"xls", L"xlsx", L"ppt",
                                  L"pptx", L"txt", L"rtf", L"epub", L"csv", L"md", L"odt", NULL };
static const wchar_t *k_prog[] = { L"exe", L"msi", L"apk", L"deb", L"rpm", L"dmg",
                                   L"appimage", L"bat", L"com", L"jar", L"7z.001", NULL };

static int in_list(const wchar_t *ext, const wchar_t **list)
{
    for (int i = 0; list[i]; i++)
        if (_wcsicmp(ext, list[i]) == 0) return 1;
    return 0;
}

static int hexv(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

/* 取最后一个 '/' 之后的后缀（不含点）；没有则返回空。 */
static void ext_of(const wchar_t *name, wchar_t *out, int n)
{
    out[0] = 0;
    const wchar_t *dot = NULL;
    for (const wchar_t *p = name; *p; p++)
        if (*p == L'.') dot = p;
    if (!dot || !dot[1]) return;
    wcsncpy(out, dot + 1, n - 1);
    out[n - 1] = 0;
}

dl_category category_for_filename(const wchar_t *name)
{
    if (!name) return CAT_OTHER;
    wchar_t ext[32];
    ext_of(name, ext, 32);
    if (!ext[0]) return CAT_OTHER;
    if (in_list(ext, k_video))   return CAT_VIDEO;
    if (in_list(ext, k_music))   return CAT_MUSIC;
    if (in_list(ext, k_archive)) return CAT_ARCHIVE;
    if (in_list(ext, k_doc))     return CAT_DOC;
    if (in_list(ext, k_prog))    return CAT_PROGRAM;
    return CAT_OTHER;
}

const wchar_t *category_dir_name(dl_category c)
{
    switch (c) {
        case CAT_VIDEO:   return L"Videos";
        case CAT_MUSIC:   return L"Music";
        case CAT_ARCHIVE: return L"Compressed";
        case CAT_DOC:     return L"Documents";
        case CAT_PROGRAM: return L"Programs";
        default:          return L"Other";
    }
}

/* 从 p 起算的「宽字符单元」长度：1 = 普通 BMP 字符，2 = 代理项对（非 BMP）。 */
static int wide_unit_len(const wchar_t *p)
{
    if (p[0] >= (wchar_t)0xD800 && p[0] <= (wchar_t)0xDBFF &&
        p[1] >= (wchar_t)0xDC00 && p[1] <= (wchar_t)0xDFFF) return 2;
    return 1;
}

/* 取 URL 末段作文件名，并做 %XX 解码。
   关键：%XX 是**字节**，UTF-8 的多字节序列必须整段解回宽字符。
   以前是「一个 %XX 直接塞进一个 wchar_t」，于是
   %E4%B8%AD 变成 "ä¸" 三个乱码字符 —— 中文/日文文件名全乱。 */
void category_filename_from_url(const wchar_t *url, wchar_t *out, int n)
{
    out[0] = 0;
    if (!url || n <= 0) return;

    /* 去掉 query / fragment */
    wchar_t tmp[2048];
    int i = 0;
    for (; url[i] && url[i] != L'?' && url[i] != L'#' && i < 2047; i++) tmp[i] = url[i];
    tmp[i] = 0;

    /* 取最后一个 '/' 之后 */
    const wchar_t *base = tmp;
    for (const wchar_t *p = tmp; *p; p++)
        if (*p == L'/' || *p == L'\\') base = p + 1;

    char bytes[4096];
    int j = 0;
    for (const wchar_t *p = base; *p && j < (int)sizeof bytes - 8; ) {
        if (*p == L'%' && p[1] && p[2]) {
            int hi = hexv(p[1]), lo = hexv(p[2]);
            if (hi >= 0 && lo >= 0) { bytes[j++] = (char)((hi << 4) | lo); p += 3; continue; }
        }
        char u8[8];
        int wl = wide_unit_len(p);
        int k = WideCharToMultiByte(CP_UTF8, 0, p, wl, u8, 8, NULL, NULL);
        if (k <= 0) { bytes[j++] = '?'; p++; continue; }   /* 非法代理项：占位，别死循环 */
        for (int m = 0; m < k && j < (int)sizeof bytes - 1; m++) bytes[j++] = u8[m];
        p += wl;
    }
    bytes[j] = 0;

    if (!MultiByteToWideChar(CP_UTF8, 0, bytes, -1, out, n)) out[0] = 0;
    out[n - 1] = 0;
    if (!out[0]) wcsncpy(out, L"download", n - 1);
    out[n - 1] = 0;
}

void category_default_base(wchar_t *out, int n)
{
    out[0] = 0;
    wchar_t up[MAX_PATH];
    DWORD r = GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
    if (r > 0 && r < MAX_PATH) {
        _snwprintf(out, n, L"%s\\Downloads", up);
        return;
    }
    /* 退化：exe 所在目录 */
    GetModuleFileNameW(NULL, out, n);
    wchar_t *sl = wcsrchr(out, L'\\');
    if (sl) *(sl + 1) = 0;
}

void category_build_path(const wchar_t *base, const wchar_t *url_or_name,
                         int use_cat, wchar_t *out, int n)
{
    wchar_t fname[512];
    category_filename_from_url(url_or_name, fname, 512);
    if (use_cat) {
        dl_category c = category_for_filename(fname);
        _snwprintf(out, n, L"%s\\%s\\%s", base, category_dir_name(c), fname);
    } else {
        _snwprintf(out, n, L"%s\\%s", base, fname);
    }
}
