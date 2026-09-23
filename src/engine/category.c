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

/* 取最后一个 '.' 之后的后缀（不含点）；没有则返回空。 */
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

void category_filename_from_url(const wchar_t *url, wchar_t *out, int n)
{
    out[0] = 0;
    if (!url) return;

    /* 去掉 query / fragment */
    wchar_t tmp[2048];
    int i = 0;
    for (; url[i] && url[i] != L'?' && url[i] != L'#' && i < 2047; i++) tmp[i] = url[i];
    tmp[i] = 0;

    /* 取最后一个 '/' 之后 */
    const wchar_t *base = tmp;
    for (const wchar_t *p = tmp; *p; p++)
        if (*p == L'/' || *p == L'\\') base = p + 1;

    /* 基本 %XX 解码 */
    int j = 0;
    for (const wchar_t *p = base; *p && j < n - 1; p++) {
        if (*p == L'%' && p[1] && p[2]) {
            int hi = 0, lo = 0, k;
            for (k = 0; k < 2; k++) {
                wchar_t ch = p[1 + k];
                int v = -1;
                if (ch >= L'0' && ch <= L'9') v = ch - L'0';
                else if (ch >= L'a' && ch <= L'f') v = ch - L'a' + 10;
                else if (ch >= L'A' && ch <= L'F') v = ch - L'A' + 10;
                if (v < 0) break;
                if (k == 0) hi = v; else lo = v;
            }
            if (k == 2) { out[j++] = (wchar_t)((hi << 4) | lo); p += 2; continue; }
        }
        out[j++] = *p;
    }
    out[j] = 0;
    if (!out[0]) wcscpy(out, L"download");
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
