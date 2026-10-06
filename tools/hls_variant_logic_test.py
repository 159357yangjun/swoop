#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""hls_variant_logic_test.py — ⑥ 画质下拉的**引擎/数据层**判据（非 E2E）。

与 tools/hls_variant_probe.cpp 的分工：那枚探针证的是"产物字节归属"（端到端下载结果
== 所选变体拼接）；本脚本证的是它测不到的四件事——
  L1 列表生成：master 多档 BANDWIDTH ⇒ host listHlsVariants 逐档产出，id/fmt==BANDWIDTH 串
  L2 排序规则：**声明序透传、不重排**（本轮实测定的语义；默认项是 UI 占位、不参与）
  L3 默认选择：forcedVariant 空 ⇒ 引擎选 BANDWIDTH 最大者；未知值 ⇒ 回退最高、不失败
  L4 值传递：传给下载引擎的是 **BANDWIDTH 十进制串**（不是索引）⇒ 用"同带宽不同 URI"夹具钉死

被测对象都是**真制品**：L1/L2 跑 build/idm-next-host.exe（IDM_HOST_LIST_JSON 测具面），
L3/L4 跑 build/hls_variant_probe.exe（生产链 TaskController⇒HlsDownloader，产物字节证人）。
夹具是本文件内的简化 HTTP server（只服务 master/media/分片；probe 从 media playlist 现场
拼期望值 ⇒ 内容互斥即可，不必是真 TS）。

用法：python tools/hls_variant_logic_test.py            （全六段）
      python tools/hls_variant_logic_test.py --face B   （只跑变异判别式所需面）
退码：红条数。读数写 stdout，同时落 %TEMP%/idm-hls-logic-<ts>/readings.txt。
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST_EXE = ROOT / "build" / "idm-next-host.exe"
PROBE_EXE = ROOT / "build" / "hls_variant_probe.exe"

FAILS = []
READINGS = []


def say(line):
    READINGS.append(line)
    try:
        print(line)
    except UnicodeEncodeError:
        print(line.encode("gbk", errors="replace").decode("gbk"))


def check(cond, name, detail):
    if cond:
        say(f"[PASS] {name} — {detail}")
    else:
        say(f"[FAIL] {name} — {detail}")
        FAILS.append(name)


def sha12(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:12]


def seg_blob(seed, n=4096):
    return bytes((seed * 7 + i) & 0xFF for i in range(n))


class Fixture:
    """routes: path -> (bytes, content_type)。GET only，线程化。"""

    def __init__(self, routes):
        self.routes = routes
        outer = self

        class H(BaseHTTPRequestHandler):
            def do_GET(self):
                r = outer.routes.get(self.path)
                if r is None:
                    self.send_response(404)
                    self.end_headers()
                    return
                body, ctype = r
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass

        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.srv.server_address[1]
        self.th = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.th.start()

    @property
    def base(self):
        return f"http://127.0.0.1:{self.port}"

    def stop(self):
        self.srv.shutdown()
        self.srv.server_close()


M3U8 = "application/vnd.apple.mpegurl"


def media_body(names, seed0):
    lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2"]
    acc = b""
    for i, n in enumerate(names):
        lines += ["#EXTINF:2.0,", n]
        acc += seg_blob(seed0 + i)
    lines += ["#EXT-X-ENDLIST", ""]
    return "\n".join(lines).encode(), acc


def variant_master_routes(prefix="", hi_bw=6239287, lo_bw=246440, order=("hi", "lo"),
                          hi_uri=None, lo_uri=None):
    """两档 master + 各自 media + 分片。hi/lo 内容 seed 族互斥（100+ vs 900+）。"""
    hi_names = [f"{prefix}hseg{i}.ts" for i in range(3)]
    lo_names = [f"{prefix}lseg{i}.ts" for i in range(3)]
    hi_media, hi_expect = media_body(hi_names, 100)
    lo_media, lo_expect = media_body(lo_names, 900)
    hu = hi_uri or f"{prefix}media_hi.m3u8"
    lu = lo_uri or f"{prefix}media_lo.m3u8"
    hi_tag = f"#EXT-X-STREAM-INF:BANDWIDTH={hi_bw},RESOLUTION=1080x1920,CODECS=\"avc1.640028\""
    lo_tag = f"#EXT-X-STREAM-INF:BANDWIDTH={lo_bw},RESOLUTION=96x540,CODECS=\"avc1.4d001f\""
    blocks = {"hi": [hi_tag, hu], "lo": [lo_tag, lu]}
    lines = ["#EXTM3U"]
    for k in order:
        lines += blocks[k]
    master = ("\n".join(lines) + "\n").encode()
    routes = {
        "/master.m3u8": (master, M3U8),
        "/" + hu: (hi_media, M3U8),
        "/" + lu: (lo_media, M3U8),
    }
    for i, n in enumerate(hi_names):
        routes["/" + n] = (seg_blob(100 + i), "video/mp2t")
    for i, n in enumerate(lo_names):
        routes["/" + n] = (seg_blob(900 + i), "video/mp2t")
    return routes, hi_expect, lo_expect


def run_host_list(url):
    env = dict(os.environ, IDM_HOST_LIST_JSON=url)
    p = subprocess.run([str(HOST_EXE)], capture_output=True, timeout=30, env=env)
    out = p.stdout.decode("utf-8", errors="replace").strip()
    try:
        return json.loads(out.splitlines()[-1]), p.returncode
    except Exception:
        return {"_raw": out}, p.returncode


def run_probe(base, outdir, variant):
    env = dict(os.environ, IDM_HLS_VARIANT_PROBE="1", IDM_HLS_BASE_URL=base,
               IDM_HLS_OUT_DIR=str(outdir), QT_QPA_PLATFORM="offscreen",
               # probe 是 GUI 子系统（QApplication）：Windows 无控制台时 qInfo/qWarning
               # 走 Debug 通道、capture pipe 拿不到 ⇒ 必须显式强制 stderr（本轮实测）。
               QT_FORCE_STDERR_LOGGING="1", QT_ASSUME_STDERR_HAS_CONSOLE="1")
    p = subprocess.run([str(PROBE_EXE)], capture_output=True, timeout=240, env=env)
    txt = p.stdout.decode("utf-8", errors="replace") + p.stderr.decode("utf-8", errors="replace")
    return txt, p.returncode


def run_probe_case(base, outdir, variant, expect_bytes):
    """单例判别式：自己起任务（生产链同 hls_variant_probe），比对**产物字节归属**。
    与整跑 probe 的区别：能指定 variant 与期望内容 ⇒ 支持 URI 交换夹具。"""
    env = dict(os.environ, IDM_PROBE_CASE_VARIANT=variant, IDM_PROBE_EXPECT_LEN=str(len(expect_bytes)),
               IDM_HLS_BASE_URL=base, IDM_HLS_OUT_DIR=str(outdir), QT_QPA_PLATFORM="offscreen",
               QT_FORCE_STDERR_LOGGING="1", QT_ASSUME_STDERR_HAS_CONSOLE="1")
    p = subprocess.run([str(PROBE_EXE)], capture_output=True, timeout=240, env=env)
    txt = p.stdout.decode("utf-8", errors="replace") + p.stderr.decode("utf-8", errors="replace")
    got = b""
    f = Path(outdir) / "master.ts"
    if f.exists():
        got = f.read_bytes()
    return got == expect_bytes, p.returncode, txt


def main():
    face = sys.argv[2] if len(sys.argv) > 2 and sys.argv[1] == "--face" else "ALL"
    ts = time.strftime("%m%d-%H%M%S", time.localtime())
    logdir = Path(os.environ.get("TEMP", "/tmp")) / f"idm-hls-logic-{ts}"
    logdir.mkdir(parents=True, exist_ok=True)
    say(f"[身份] 仓根={ROOT} HOST={sha12(HOST_EXE)} PROBE={sha12(PROBE_EXE)}")
    git = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                         capture_output=True, text=True).stdout.strip()
    say(f"[身份] HEAD={git} face={face}")

    # ── L1/L2：host listHlsVariants（真制品，经 IDM_HOST_LIST_JSON 面）──
    fx = Fixture(variant_master_routes()[0])
    try:
        j, rc = run_host_list(fx.base + "/master.m3u8")
        fmts = j.get("formats", [])
        ids = [f.get("id") for f in fmts]
        bw_decl = ["6239287", "246440"]
        check(rc == 0 and j.get("success") is True,
              "L1a 两档 master ⇒ success", f"rc={rc} success={j.get('success')}")
        check(ids == bw_decl, "L1b id 序列==声明序透传", f"ids={ids}")
        check(all(f.get("fmt") == f.get("id") for f in fmts),
              "L1c fmt==id（content.js 用它作 option.value）",
              f"fmt={[f.get('fmt') for f in fmts]}")
        check(all(re.fullmatch(r"\d+", str(i)) for i in ids),
              "L1d 值是 BANDWIDTH 十进制串（不是索引/label）", f"ids={ids}")
        labels = [f.get("label") for f in fmts]
        check(labels == ["1080x1920 · 6.2 Mbps", "96x540 · 0.2 Mbps"],
              "L2a label 含 RESOLUTION 与换算后的 Mbps（同长自检）", f"labels={labels}")
        heights = [f.get("height") for f in fmts]
        check(heights == [1920, 540], "L2b height 取 RESOLUTION 高（第二证人）", f"h={heights}")
        # 乱序声明 ⇒ 仍按声明序输出（证明"没重排"而不是"碰巧输入有序"）
        fx.stop()
        fx = Fixture(variant_master_routes(order=("lo", "hi"))[0])
        j2, _ = run_host_list(fx.base + "/master.m3u8")
        ids2 = [f.get("id") for f in j2.get("formats", [])]
        check(ids2 == ["246440", "6239287"],
              "L2c 乱序声明⇒输出保持声明序（**不重排**是本轮定的语义；若改成降序门必须红）",
              f"ids={ids2}")
        # 无 BANDWIDTH 的 STREAM-INF ⇒ 跳过（无法与引擎匹配键对齐）
        fx.stop()
        routes, _, _ = variant_master_routes()
        bad = ("\n".join(["#EXTM3U",
                          '#EXT-X-STREAM-INF:RESOLUTION=640x360',  # 无 BANDWIDTH
                          "media_hi.m3u8",
                          '#EXT-X-STREAM-INF:BANDWIDTH=246440',
                          "media_lo.m3u8", ""]).encode())
        routes["/master.m3u8"] = (bad, M3U8)
        fx = Fixture(routes)
        j3, _ = run_host_list(fx.base + "/master.m3u8")
        ids3 = [f.get("id") for f in j3.get("formats", [])]
        check(ids3 == ["246440"], "L1e 无 BANDWIDTH 的档位被跳过", f"ids={ids3}")
        # 单档 media playlist（无 STREAM-INF）⇒ success=false，UI 只留默认项
        fx.stop()
        one, _ = media_body(["s0.ts"], 500)
        fx = Fixture({"/master.m3u8": (one, M3U8), "/s0.ts": (seg_blob(500), "video/mp2t")})
        j4, _ = run_host_list(fx.base + "/master.m3u8")
        check(j4.get("success") is False, "L1f 无可选变体 ⇒ success=false（UI 降级路径有数据形状）",
              f"msg={j4.get('message','')!r}")
    finally:
        fx.stop()

    # ── L3/L4：引擎默认/传值（真 hls_variant_probe 走生产链，产物字节证人）──
    routes, hi_exp, lo_exp = variant_master_routes()
    fx = Fixture(routes)
    try:
        txt, prc = run_probe(fx.base, logdir / "dl", None)
        (logdir / "probe.log").write_text(txt, encoding="utf-8")
        v1 = "V1 no-variant downloads highest" in txt and "[PASS] V1" in txt
        v2 = "[PASS] V2" in txt
        v3 = "[PASS] V3" in txt
        check(v1, "L3a 默认（forcedVariant 空）⇒ 产物==最高 BANDWIDTH 拼接",
              f"probe rc={prc}")
        check(v2, "L3b 指定低档 '246440' ⇒ 产物==lo 拼接（传值=BANDWIDTH 串被引擎认）", "")
        check(v3, "L3c 未知值 '999999999' ⇒ 回退最高、任务成功", "")
        # L4：同带宽、不同 URI ⇒ 只有"按 BANDWIDTH 匹配再取其 URI"才会拿到对应字节。
        # 整跑 V1–V3 的期望值钉在固定 URI 上，这种夹具会让三条全红（无诊断力）⇒ 走单例面。
        fx.stop()
        routes2, hi_exp2, lo_exp2 = variant_master_routes(hi_uri="media_lo.m3u8",
                                                           lo_uri="media_hi.m3u8")
        fx = Fixture(routes2)
        ok_v1, rc1, _ = run_probe_case(fx.base, logdir / "dl2a", "__none__", hi_exp2)
        ok_v2, rc2, _ = run_probe_case(fx.base, logdir / "dl2b", "246440", lo_exp2)
        check(ok_v1 and ok_v2,
              "L4 URI 交换夹具 ⇒ 默认拿 hi 字节(rc=%d)、选 246440 拿 lo 字节(rc=%d)"
              "（传值绝不是 URI/索引）" % (rc1, rc2),
              f"hi=={hi_exp2[:4].hex()} lo=={lo_exp2[:4].hex()}")
    finally:
        fx.stop()

    say(f"[汇总] 判据 {len([l for l in READINGS if l.startswith('[PASS]') or l.startswith('[FAIL]')])} 条，"
        f"红 {len(FAILS)} 条" + (f"：{FAILS}" if FAILS else ""))
    (logdir / "readings.txt").write_text("\n".join(READINGS), encoding="utf-8")
    say(f"[读数档] {logdir}")
    return len(FAILS)


if __name__ == "__main__":
    sys.exit(main())
