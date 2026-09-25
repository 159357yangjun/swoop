#!/usr/bin/env python3
"""发行打包：把「下载即用」的文件集打成 zip 并算 SHA256。

产物（dist/）:
  swoop-<版本>-win64.zip   主程序 + nmhost + 浏览器扩展 + 文档
  SHA256SUMS.txt              校验和（给 GitHub Release 附件用）

前提：先 make all。两个 exe 均为 -static 链接，只依赖 Windows 系统 DLL，
      因此包内不需要任何运行时。
"""
import hashlib
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")


def configure_output_encoding():
    """让 Windows 上的中文状态/错误信息不受系统代码页影响。"""
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8")


def read_version():
    """从 src/common/version.h 解出 (语义版本, tag)。"""
    path = os.path.join(ROOT, "src", "common", "version.h")
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    m = re.search(r'IDM_VERSION_STR\s+"([^"]+)"', text)
    if not m:
        sys.exit("无法从 src/common/version.h 解析 IDM_VERSION_STR")
    return m.group(1)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ver = read_version()
    app = "swoop"
    bundle = f"{app}-{ver}-win64"
    os.makedirs(DIST, exist_ok=True)

    # (源路径, zip 内相对路径) —— 按包内目录结构排好
    required = [
        (os.path.join(ROOT, "swoop.exe"), f"{bundle}/swoop.exe"),
        (os.path.join(ROOT, "swoop_nmhost.exe"), f"{bundle}/swoop_nmhost.exe"),
    ]
    optional = [
        (os.path.join(ROOT, "README.md"), f"{bundle}/README.md"),
        (os.path.join(ROOT, "LICENSE"), f"{bundle}/LICENSE"),
        (os.path.join(ROOT, "extension", "manifest.json"),
         f"{bundle}/extension/manifest.json"),
        (os.path.join(ROOT, "extension", "background.js"),
         f"{bundle}/extension/background.js"),
        (os.path.join(ROOT, "extension", "content.js"),
         f"{bundle}/extension/content.js"),
        (os.path.join(ROOT, "extension", "popup.html"),
         f"{bundle}/extension/popup.html"),
        (os.path.join(ROOT, "extension", "popup.css"),
         f"{bundle}/extension/popup.css"),
        (os.path.join(ROOT, "extension", "popup.js"),
         f"{bundle}/extension/popup.js"),
        (os.path.join(ROOT, "extension", "icons", "swoop.svg"),
         f"{bundle}/extension/icons/swoop.svg"),
        (os.path.join(ROOT, "packaging", "register-nmhost.cmd"),
         f"{bundle}/register-nmhost.cmd"),
    ]

    missing = [src for src, _ in required if not os.path.isfile(src)]
    if missing:
        sys.exit("缺少产物（先跑 make all）：\n  " + "\n  ".join(missing))

    zip_path = os.path.join(DIST, bundle + ".zip")
    entries = list(required) + [(s, d) for s, d in optional if os.path.isfile(s)]

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for src, arc in entries:
            z.write(src, arc)

    digest = sha256(zip_path)
    size_kb = os.path.getsize(zip_path) / 1024.0
    with open(os.path.join(DIST, "SHA256SUMS.txt"), "w", encoding="utf-8") as f:
        f.write(f"{digest}  {os.path.basename(zip_path)}\n")

    skipped = [s for s, _ in optional if not os.path.isfile(s)]
    print(f"打包完成: dist/{os.path.basename(zip_path)}  ({size_kb:.1f} KB)")
    print(f"  SHA256  {digest}")
    print(f"  收录 {len(entries)} 个文件" + (f"；跳过缺失 {skipped}" if skipped else ""))
    return 0


if __name__ == "__main__":
    configure_output_encoding()
    sys.exit(main())
