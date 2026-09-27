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

/* Windows 文件名不能包含这些字符；控制字符 0..31 同样禁止。
   URL 的 %2F/%5C 必须在解码后再过滤，否则会变成路径分隔符。 */
static int bad_filename_char(wchar_t c)
{
    if (c < 32) return 1;
    return wcschr(L"<>:\"/\\|?*", c) != NULL;
}

/* Windows 设备名即使带扩展名仍然保留，例如 CON.txt / LPT1.log。 */
static int reserved_windows_name(const wchar_t *name)
{
    wchar_t stem[16];
    int n = 0;
    if (!name) return 0;
    while (*name && *name != L'.' && n < (int)(sizeof stem / sizeof stem[0]) - 1)
        stem[n++] = *name++;
    while (n > 0 && stem[n - 1] == L' ') n--;
    stem[n] = 0;

    if (_wcsicmp(stem, L"CON") == 0 || _wcsicmp(stem, L"PRN") == 0 ||
        _wcsicmp(stem, L"AUX") == 0 || _wcsicmp(stem, L"NUL") == 0)
        return 1;
    if (n == 4 &&
        ((_wcsnicmp(stem, L"COM", 3) == 0) || (_wcsnicmp(stem, L"LPT", 3) == 0)) &&
        stem[3] >= L'1' && stem[3] <= L'9')
        return 1;
    return 0;
}

/* 把不可信的 URL/站点建议名收敛成“单个 Windows 文件名”。
   目标：不能产生路径穿越、不能因为非法字符直接 CreateFile 失败，也不能落到设备名。 */
static void sanitize_filename(wchar_t *name, int n)
{
    if (!name || n <= 0) return;

    int w = 0;
    for (int r = 0; name[r] && w + 1 < n; r++) {
        wchar_t c = name[r];
        name[w++] = bad_filename_char(c) ? L'_' : c;
    }

    /* Win32 会忽略/拒绝文件名末尾的空格和点；主动去掉，避免“看着存在实际打不开”。 */
    while (w > 0 && (name[w - 1] == L' ' || name[w - 1] == L'.')) w--;
    name[w] = 0;

    if (!name[0]) {
        wcsncpy(name, L"download", n - 1);
        name[n - 1] = 0;
        return;
    }

    if (reserved_windows_name(name)) {
        size_t len = wcslen(name);
        if ((int)len + 1 < n) {
            for (size_t i = len + 1; i > 0; i--) name[i] = name[i - 1];
            name[0] = L'_';
        } else {
            /* 极小缓冲区没有前插空间时，至少破坏设备名本身。 */
            name[0] = L'_';
        }
    }
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
   解码完成后还必须做 Windows 文件名清洗：%2F/%5C 等不能重新变成路径。 */
void category_filename_from_url(const wchar_t *url, wchar_t *out, int n)
{
    if (!out || n <= 0) return;
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

    char bytes[4096];
    int j = 0;
    for (const wchar_t *p = base; *p && j < (int)sizeof bytes - 8; ) {
        if (*p == L'%' && p[1] && p[2]) {
            int hi = hexv(p[1]), lo = hexv(p[2]);
            if (hi >= 0 && lo >= 0) {
                int v = (hi << 4) | lo;
                bytes[j++] = (char)(v == 0 ? '_' : v);   /* NUL 不能进入 Windows 文件名 */
                p += 3;
                continue;
            }
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
    sanitize_filename(out, n);
}

void category_default_base(wchar_t *out, int n)
{
    out[0] = 0;
    wchar_t up[MAX_PATH];
    DWORD r = GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
    if (r > 0 && r < MAX_PATH) {
        _snwprintf(out, n, L"%s\\Downloads", up);
        out[n - 1] = 0;
        return;
    }
    /* 退化：exe 所在目录 */
    GetModuleFileNameW(NULL, out, n);
    out[n - 1] = 0;
    wchar_t *sl = wcsrchr(out, L'\\');
    if (sl) *(sl + 1) = 0;
}

void category_build_path(const wchar_t *base, const wchar_t *url_or_name,
                         int use_cat, wchar_t *out, int n)
{
    if (!out || n <= 0) return;
    wchar_t fname[512];
    category_filename_from_url(url_or_name, fname, 512);
    if (use_cat) {
        dl_category c = category_for_filename(fname);
        _snwprintf(out, n, L"%s\\%s\\%s", base, category_dir_name(c), fname);
    } else {
        _snwprintf(out, n, L"%s\\%s", base, fname);
    }
    out[n - 1] = 0;
}