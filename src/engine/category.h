#ifndef IDM_CATEGORY_H
#define IDM_CATEGORY_H

#include <wchar.h>

/* 下载分类（对标 IDM 的 压缩/文档/音乐/程序/视频 归类目录）。 */
typedef enum {
    CAT_OTHER = 0, CAT_VIDEO, CAT_MUSIC, CAT_ARCHIVE, CAT_DOC, CAT_PROGRAM
} dl_category;

/* 按文件名/URL 的后缀判类别（大小写不敏感）。 */
dl_category category_for_filename(const wchar_t *name);

/* 类别对应的子目录名（英文，避免中文路径编码麻烦）。 */
const wchar_t *category_dir_name(dl_category c);

/* 从 URL 提取文件名（去 query/fragment，取最后一段，做 %XX 解码）。 */
void category_filename_from_url(const wchar_t *url, wchar_t *out, int n);

/* 默认下载根目录：%USERPROFILE%\Downloads（取不到则用 exe 目录）。 */
void category_default_base(wchar_t *out, int n);

/* 组装保存路径：base[\类别]\文件名。use_cat=0 时不加类别子目录。 */
void category_build_path(const wchar_t *base, const wchar_t *url_or_name,
                         int use_cat, wchar_t *out, int n);

#endif
