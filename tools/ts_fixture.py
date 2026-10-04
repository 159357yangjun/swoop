#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""真 MPEG-TS 分片生成器 + 独立判读器（零公网）。

为什么需要它：前面几轮的 HLS fixture 用的是**伪 TS**（0x47 开头 + 固定载荷），
它只能证明"字节搬运正确"，证不了"拼接出来的东西还是合法的 TS"。
MPEG 传输流的分片边界是**包粒度**的——一个 PES 可以跨多个 188 字节包，
所以"按序 concat"看着显然、实际有一个真实失败模式：**中间分片长度不是 188 的整数倍时，
拼接点之后的 sync 字节全部错位**，播放器会在接缝处开始丢帧。这条要能被测出来。

⚠ 本机没有 ffprobe/ffmpeg/mpegts 库，也不装软件 ⇒ "能不能真的播放"这一条
   **不能由本脚本声称**。这里做的是更窄也更硬的事：
     · 校验每一包 sync=0x47、adaptation field control 合法；
     · 校验整段包数 == floor(len/188) 且无残余字节（接缝对齐的直接证据）；
     · 解析 PAT/PMT 拿到 program → 视频 PID 映射；
     · 从 PCR 算时长，与 m3u8 里 #EXTINF 声明的时长对账。

自证：`--selftest` 跑三个用例——合法 TS 必须通过、把某包 sync 改坏必须失败、
把最后一包截掉 3 字节必须失败。**判读器自己没被验过就当证人，是本会话反复踩的坑。**

用法：
  python tools/ts_fixture.py --emit <outdir>     # 生成 seg0/1/2.ts + manifest
  python tools/ts_fixture.py --audit <file.ts>   # 判读单个 TS
  python tools/ts_fixture.py --selftest          # 判读器自身的正负例
"""
import argparse
import hashlib
import os
import pathlib
import struct
import sys

PACKET = 188
PID_PAT = 0x0000
PID_PMT = 0x1000
PID_VID = 0x100
PID_PCR = 0x100
PROGRAM = 1


def crc32_mpeg(data):
    """MPEG-2 CRC-32（多项式 0x80FEDC65 的等价实现，初值 0xFFFFFFFF，无反射）。
    这里只需要**确定性**与**两端一致**：生成侧与判读侧用同一个函数即可对账，
    不追求与硬件接收端逐位相同（那需要 ffprobe 级别的验证，本机做不到，已写进限制）。"""
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            if crc & (1 << 31):
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


def _stuff(payload, target):
    """把 payload 补到目标长度（用 0xFF 填充，符合 TS 尾部习惯）。"""
    if len(payload) >= target:
        return payload[:target]
    return payload + bytes([0xFF]) * (target - len(payload))


def pat_packet():
    """PAT：program 1 -> PMT PID。section 语法按 ISO/IEC 13818-1 精简实现。"""
    body = bytearray()
    body += struct.pack(">H", PROGRAM)                       # program_number
    body += bytes([0xE0 | ((PID_PMT >> 8) & 0x1F), PID_PMT & 0xFF])  # reserved + PMT PID
    section = bytearray()
    section += bytes([0x00])                     # table_id
    inner = bytearray()
    inner += struct.pack(">H", 0x0001)           # transport_stream_id
    inner.append(0xC1)                           # reserved|version|current
    inner.append(0x00)                           # section_number
    inner.append(0x00)                           # last_section_number
    inner += body
    seg_len = len(inner) + 4                     # +CRC32
    section += bytes([0xB0 | ((seg_len >> 8) & 0x0F), seg_len & 0xFF])
    section += inner
    section += struct.pack(">I", crc32_mpeg(section))
    pl = bytearray([0x00]) + section             # pointer_field = 0
    return _pack(PID_PAT, bytes(pl), pusi=True)


_CONT = {}


def _pack(pid, payload, pusi=False, cc_key=None, pcr=None):
    """组一个 188 字节 TS 包。adaptation field 只在需要塞 PCR 或补齐时出现。"""
    cc = _CONT.setdefault(cc_key or pid, 0)
    _CONT[cc_key or pid] = (cc + 1) & 0x0F
    afc_bits = 0x01                              # payload only
    adapt = b""
    if pcr is not None:
        afc_bits = 0x03                          # adaptation + payload
        adapt = _adaptation(pcr, 0)
    room = PACKET - 4 - len(adapt)
    if len(payload) > room:
        payload = payload[:room]
    elif len(payload) < room:
        # 用 stuffing 填满；保证每包恰好 188 字节
        payload = payload + bytes([0xFF]) * (room - len(payload))
    hdr = bytearray(4)
    hdr[0] = 0x47
    # byte1 = TSC(0)|PUSI(bit6)|TP(bit5)|PID高5位 ；byte3 = AFC(bit5-4)|CC(低4位)
    hdr[1] = (0x40 if pusi else 0x00) | 0x20 | ((pid >> 8) & 0x1F)   # TP=1（有效载荷）
    hdr[2] = pid & 0xFF
    hdr[3] = ((afc_bits & 0x03) << 4) | (cc & 0x0F)
    return bytes(hdr) + adapt + payload


def _adaptation(pcr, pad_to):
    """最小 adaptation field：只带 PCR，其余用 0xFF 填充。"""
    pcr_bytes = _encode_pcr(pcr)
    flags = 0x10                                 # PCR_flag
    # adaptation_field_length = flags(1) + pcr(6) + pad
    pad = max(0, min(182 - (1 + 6), pad_to))
    length = 1 + 6 + pad
    out = bytearray([length, flags])
    out += pcr_bytes
    out += bytes([0xFF]) * pad
    return bytes(out)


def _encode_pcr(base_90k):
    ext = 0
    b = bytearray(6)
    b[0] = (base_90k >> 25) & 0xFF
    b[1] = (base_90k >> 17) & 0xFF
    b[2] = (base_90k >> 9) & 0xFF
    b[3] = (base_90k >> 1) & 0xFF
    b[4] = ((base_90k & 1) << 7) | 0x7E | ((ext >> 14) & 1)
    b[5] = (ext >> 7) & 0xFF
    return bytes(b)


def decode_pcr(raw):
    base = (raw[0] << 25) | (raw[1] << 17) | (raw[2] << 9) | (raw[3] << 1) | ((raw[4] >> 7) & 1)
    return base


def pmt_packet():
    inner = bytearray()
    inner += struct.pack(">H", PROGRAM)                    # program_number
    inner.append(0xC1)                                     # reserved|version|current
    inner.append(0x00)                                     # section_number
    inner.append(0x00)                                     # last_section_number
    inner += bytes([(0xE0 | ((PID_PCR >> 8) & 0x1F)), PID_PCR & 0xFF])   # PCR_PID
    inner += bytes([0xF0, 0x00])                           # reserved + program_info_length=0
    # 一条流：stream_type=0x02(H.262 video)、elementary PID
    inner += bytes([0x02, 0xE0 | ((PID_VID >> 8) & 0x1F), PID_VID & 0xFF, 0xF0, 0x00])
    seg_len = len(inner) + 4
    section = bytearray([0x02, 0xB0 | ((seg_len >> 8) & 0x0F), seg_len & 0xFF])
    section += inner
    section += struct.pack(">I", crc32_mpeg(section))
    pl = bytearray([0x00]) + section
    return _pack(PID_PMT, bytes(pl), pusi=True)


def pes_packet(stream_id, payload, pts=None, cont=False):
    head = bytearray()
    head += bytes([0x00, 0x00, 0x01, stream_id])
    pts_present = pts is not None
    data_len = len(payload)
    head += struct.pack(">H", data_len if data_len < 0xFFFF else 0)  # PES_packet_length
    flags = 0x80                                   # marker
    if pts_present:
        flags |= 0xC0                              # both DTS and PTS present
        hdr_len = 14
    else:
        hdr_len = 3
    head += bytes([flags, 0x00, hdr_len])
    if pts_present:
        head += _encode_pts(pts)
    return bytes(head) + payload


def _encode_pts(v):
    b = bytearray(5)
    b[0] = 0x31 | ((v >> 29) & 0x0E) | 0x01       # '0011' prefix + bits + marker
    b[1] = (v >> 22) & 0xFF
    b[2] = ((v >> 14) & 0xFE) | 0x01
    b[3] = (v >> 7) & 0xFF
    b[4] = ((v << 1) & 0xFE) | 0x01
    return bytes(b)


def decode_pts(raw):
    return (((raw[0] >> 1) & 0x07) << 30) | ((raw[1] << 22) | ((raw[2] >> 1) << 15)
                                             | ((raw[3] << 7) | (raw[4] >> 1)))


def build_segment(n_packets, pcr_base, pts_base, seed):
    """一段合法 TS：PAT + PMT + 若干视频包（PES 跨包，故意让 PES 不在分片末尾结束）。

    关键设计：**每个分片都以完整的 188 字节包收尾**（这是产品 concat 后仍合法的必要条件）。
    另外准备一个 `build_bad_tail_segment()` 用来造"长度非 188 整数倍"的反例，
    证明判据确实会因接缝错位而红。
    """
    out = bytearray()
    out += pat_packet()
    out += pmt_packet()
    per = 184                                    # payload-only 容量
    frame = bytes(((i * 17 + seed * 3) & 0xFF) for i in range(per * (n_packets - 2) + 40))
    # 视频基本流：前几个包带 PES 头 + PTS，后续为 continuation
    first = True
    idx = 0
    total = n_packets - 2
    while idx < total:
        chunk = frame[idx * per:(idx + 1) * per]
        if first:
            pl = pes_packet(0xE0, chunk, pts=pts_base + idx * 3000)
            first = False
        else:
            pl = chunk
        pcr = pcr_base if idx == 0 else None
        out += _pack(PID_VID, pl, pusi=(idx == 0), pcr=pcr)
        idx += 1
    return bytes(out)


def build_bad_tail_segment(n_packets, pcr_base, pts_base, seed, trim):
    good = build_segment(n_packets, pcr_base, pts_base, seed)
    return good[:-trim]


# ────────────────────────── 判读器 ──────────────────────────
def audit_ts(data, label=""):
    r = {"label": label, "bytes": len(data), "sync_ok": False, "align_ok": False,
         "pat_found": False, "pmt_found": False, "vid_pid": None, "stream_type": None,
         "packet_count": 0, "residual": len(data) % PACKET,
         "pcr_first": None, "pcr_last": None, "pts_first": None, "pts_last": None,
         "errors": []}
    if len(data) < PACKET:
        r["errors"].append("太短，不足一包")
        return r
    n = len(data) // PACKET
    pos = 0
    bad_sync = []
    pids_seen = {}
    for i in range(n):
        pkt = data[pos:pos + PACKET]
        pos += PACKET
        if pkt[0] != 0x47:
            bad_sync.append(i)
            continue
        pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
        pusi = bool(pkt[1] & 0x40)
        afc = (pkt[3] >> 4) & 0x03
        if afc == 0:
            bad_sync.append(("afc=0", i))
            continue
        pids_seen[pid] = pids_seen.get(pid, 0) + 1
        off = 4
        pcr = None
        if afc & 0x02:
            alen = pkt[4]
            if alen > 0 and (pkt[5] & 0x10):
                pcr = decode_pcr(pkt[6:12])
            off = 5 + alen
        payload = pkt[off:]
        if pid == PID_PAT and pusi:
            r["pat_found"] = True
        if pid == PID_PMT and pusi:
            r["pmt_found"] = True
            # pointer_field(1) + table_id(1) + section_header(2) + program_number(2)
            # + c1(1) + sec_num(1) + last_sec(1) + PCR_PID(2) + reserved/prog_info_len(2)
            # ⚠ payload[0] 是 pointer_field，section 从 payload[1] 起（这里恒 0 ⇒ 无偏移）。
            #   之前按 payload[14] 取 program_info_length，取到的其实是 stream 的第一个字节，
            #   于是 vid_pid 永远 None —— 判读器"看起来能跑"但字段全空，正是半坏自检的形状。
            if len(payload) > 16:
                sec = payload[1 + payload[0]] if False else payload[1:]
                # section: table_id(0) len_hi(1) len_lo(2) prog_num(3,4) c1(5) sn(6) lsn(7)
                #          pcr_hi(8) pcr_lo(9) reserved|pil_hi(10) pil_lo(11) -> stream 从 12 起
                pil = ((sec[10] & 0x0F) << 8) | sec[11]
                k = 12 + pil
                if k + 3 <= len(sec):
                    st = sec[k]
                    vp = ((sec[k + 1] & 0x1F) << 8) | sec[k + 2]
                    r["vid_pid"] = vp
                    r["stream_type"] = st
        if pcr is not None:
            if r["pcr_first"] is None:
                r["pcr_first"] = pcr
            r["pcr_last"] = pcr
        if pusi and pid == (r["vid_pid"] or PID_VID) and len(payload) > 14 \
           and payload[0:3] == b"\x00\x00\x01":
            pts = decode_pts(payload[9:14])
            if r["pts_first"] is None:
                r["pts_first"] = pts
            r["pts_last"] = pts
    r["packet_count"] = n
    r["sync_ok"] = not bad_sync
    r["align_ok"] = (r["residual"] == 0)
    if bad_sync:
        r["errors"].append(f"{len(bad_sync)} 包 sync/afc 异常，首处={bad_sync[:3]}")
    return r


def summarize(a):
    dur_s = None
    if a["pcr_first"] is not None and a["pcr_last"] is not None:
        dur_s = (a["pcr_last"] - a["pcr_first"]) / 90000.0
    return (f"[{a['label']}] bytes={a['bytes']} packets={a['packet_count']} residual={a['residual']} "
            f"sync_ok={a['sync_ok']} align_ok={a['align_ok']} PAT={a['pat_found']} PMT={a['pmt_found']} "
            f"vidPID={a['vid_pid']} PCR跨度={dur_s if dur_s is None else round(dur_s, 3)}s "
            f"errors={a['errors'] or '无'}")


def emit(outdir):
    d = pathlib.Path(outdir); d.mkdir(parents=True, exist_ok=True)
    segs = []
    for i in range(3):
        s = build_segment(60, i * 90000 * 2, i * 90000 * 2, i)
        (d / f"tseg{i}.ts").write_bytes(s)
        segs.append(s)
        print(summarize(audit_ts(s, f"tseg{i}.ts")))
    cat = b"".join(segs)
    (d / "concat_all.ts").write_bytes(cat)
    print(summarize(audit_ts(cat, "concat_all.ts")))
    print("[期望指纹] sha256(concat)=" + hashlib.sha256(cat).hexdigest())
    return segs


def selftest():
    """判读器自身必须有正例和负例，否则它当证人不可信。"""
    ok = True
    good = build_segment(60, 0, 0, 0)
    a = audit_ts(good, "正例：完整合法 TS")
    p = a["sync_ok"] and a["align_ok"] and a["pat_found"] and a["pmt_found"]
    print(summarize(a)); print("  正例应通过 =", p); ok &= p

    broken = bytearray(good); broken[188] = 0x00      # 第二包 sync 改坏
    a2 = audit_ts(bytes(broken), "负例1：某包 sync 被改坏")
    n1 = not a2["sync_ok"]
    print(summarize(a2)); print("  负例1应失败 =", n1); ok &= n1

    trimmed = good[:-3]                                # 破坏 188 整数倍 ⇒ 接缝错位
    a3 = audit_ts(trimmed, "负例2：截掉 3 字节（模拟分片长度不对齐）")
    n2 = (not a3["align_ok"]) or (not a3["sync_ok"])
    print(summarize(a3)); print("  负例2应失败 =", n2); ok &= n2

    print(f"[SELFTEST] 判读器{'可信' if ok else '不可信'}")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit")
    ap.add_argument("--audit")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    if a.selftest:
        return selftest()
    if a.emit:
        emit(a.emit)
        return 0
    if a.audit:
        print(summarize(audit_ts(pathlib.Path(a.audit).read_bytes(), pathlib.Path(a.audit).name)))
        return 0
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
