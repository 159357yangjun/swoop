#!/usr/bin/env python3
"""发行打包：生成可直接解压使用的 Windows x64 便携包并计算 SHA256。

产物（dist/）:
  swoop-<版本>-win64.zip   主程序 + nmhost + 浏览器扩展生产文件 + 文档
  SHA256SUMS.txt          校验和（给 GitHub Release 附件用）

前提：先 make all。两个 exe 均为 -static 链接，只依赖 Windows 系统 DLL，
因此包内不需要 MinGW 运行时。
"""
from __future__ import annotations

import hashlib
import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / "dist"
FIXED_ZIP_TIME = (1980, 1, 1, 0, 0, 0)
EXTENSION_EXCLUDED_DIRS = {"__pycache__", ".pytest_cache", ".git", "node_modules"}
EXTENSION_EXCLUDED_SUFFIXES = {".py", ".pyc", ".pyo"}


def configure_output_encoding() -> None:
    """让 Windows 上的中文状态/错误信息不受系统代码页影响。"""
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8")


def read_version() -> str:
    """从 src/common/version.h 读取语义版本。"""
    path = ROOT / "src" / "common" / "version.h"
    text = path.read_text(encoding="utf-8")
    match = re.search(r'IDM_VERSION_STR\s+"([^"]+)"', text)
    if not match:
        sys.exit("无法从 src/common/version.h 解析 IDM_VERSION_STR")
    return match.group(1)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_release_extension_file(path: Path, extension_dir: Path) -> bool:
    """只收录浏览器实际运行需要的扩展文件，排除测试/缓存/开发文件。"""
    rel = path.relative_to(extension_dir)
    if any(part in EXTENSION_EXCLUDED_DIRS for part in rel.parts):
        return False
    if any(part.startswith(".") for part in rel.parts):
        return False
    if path.name.lower().startswith("test_"):
        return False
    if path.suffix.lower() in EXTENSION_EXCLUDED_SUFFIXES:
        return False
    return True


def collect_release_files(bundle: str) -> list[tuple[Path, str]]:
    """收集发布文件；扩展目录递归收录生产资源，避免新增资源后漏包。"""
    required = [
        (ROOT / "swoop.exe", f"{bundle}/swoop.exe"),
        (ROOT / "swoop_nmhost.exe", f"{bundle}/swoop_nmhost.exe"),
        (ROOT / "README.md", f"{bundle}/README.md"),
        (ROOT / "LICENSE", f"{bundle}/LICENSE"),
        (ROOT / "packaging" / "register-nmhost.cmd", f"{bundle}/register-nmhost.cmd"),
        (ROOT / "extension" / "manifest.json", f"{bundle}/extension/manifest.json"),
    ]

    missing = [str(src) for src, _ in required if not src.is_file()]
    if missing:
        sys.exit("缺少发行文件：\n  " + "\n  ".join(missing))

    entries: list[tuple[Path, str]] = list(required[:-1])
    extension_dir = ROOT / "extension"
    extension_files = sorted(
        path
        for path in extension_dir.rglob("*")
        if path.is_file() and is_release_extension_file(path, extension_dir)
    )
    if not extension_files:
        sys.exit("extension/ 没有可发行文件，无法生成浏览器扩展包")

    manifest = extension_dir / "manifest.json"
    if manifest not in extension_files:
        sys.exit("extension/manifest.json 被意外排除，无法生成浏览器扩展包")

    for src in extension_files:
        rel = src.relative_to(extension_dir).as_posix()
        entries.append((src, f"{bundle}/extension/{rel}"))

    # 去重并保持稳定排序，让相同输入尽量得到相同 ZIP。
    deduped = {arc: src for src, arc in entries}
    return [(deduped[arc], arc) for arc in sorted(deduped)]


def write_reproducible_zip(zip_path: Path, entries: list[tuple[Path, str]]) -> None:
    """写固定时间戳的 ZIP，减少 CI/本地打包产生的无意义差异。"""
    with zipfile.ZipFile(
        zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for src, arc in entries:
            info = zipfile.ZipInfo(arc, date_time=FIXED_ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(
                info,
                src.read_bytes(),
                compress_type=zipfile.ZIP_DEFLATED,
                compresslevel=9,
            )


def main() -> int:
    version = read_version()
    bundle = f"swoop-{version}-win64"
    DIST.mkdir(parents=True, exist_ok=True)

    zip_path = DIST / f"{bundle}.zip"
    entries = collect_release_files(bundle)
    write_reproducible_zip(zip_path, entries)

    digest = sha256(zip_path)
    size_kb = zip_path.stat().st_size / 1024.0
    (DIST / "SHA256SUMS.txt").write_text(
        f"{digest}  {zip_path.name}\n",
        encoding="utf-8",
        newline="\n",
    )

    print(f"打包完成: dist/{zip_path.name}  ({size_kb:.1f} KB)")
    print(f"  SHA256  {digest}")
    print(f"  收录 {len(entries)} 个生产文件（扩展测试/缓存文件已排除）")
    return 0


if __name__ == "__main__":
    configure_output_encoding()
    sys.exit(main())
