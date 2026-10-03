#!/usr/bin/env bash
# FORWARD 根因复现：两个候选扰动，各自单独切、各留原始输出。
# 用法：bash tools/fwd_repro.sh <n|block> <每面轮数>
set -u
cd "$(dirname "$0")/.."
FACE="${1:-}"
N="${2:-6}"
BIN=build/ui_snapshot.exe
[ -f "$BIN" ] || { echo "MISSING $BIN"; exit 6; }
export QT_QPA_PLATFORM=offscreen

case "$FACE" in
  n)
    # 扰动定义：假服务 handler **不再同步 flush**（只 write + deleteLater），
    # 让字节停在客户端缓冲里 ⇒ 产品侧 waitForBytesWritten(2000) 该超时、走写失败分支。
    # ⚠ 这是被测代码的**对端**行为改变，不是把判据改松；判据与阈值一字未动。
    python - <<'PY'
import io, subprocess, os, re
p = "tools/ui_snapshot.cpp"
s = io.open(p, encoding="utf-8", newline="\n").read()
old = """                c->write(QJsonDocument(resp).toJson(QJsonDocument::Compact));
                c->flush();"""
new = """                c->write(QJsonDocument(resp).toJson(QJsonDocument::Compact));
                // REPRO-N: flush 摘掉（只为复现"写完不落"，跑完逐字还原）"""
assert s.count(old) == 1, s.count(old)
io.open(p, "w", encoding="utf-8", newline="\n").write(s.replace(old, new))
print("[repro] 已摘掉假服务的 flush")
PY
    ;;
  block)
    # 扰动定义：worker 线程在 connectToServer 之后、write 之前被按住 2.5s
    #           ⇒ 客户端事件循环没人驱动，waitForBytesWritten 必然超时。
    python - <<'PY'
import io
p = "src/app/cli_forward.cpp"
s = io.open(p, encoding="utf-8", newline="\n").read()
old = "    socket.write(data);\n    socket.flush();"
new = ("    if (qEnvironmentVariableIsSet(\"IDM_FWD_BLOCK_MS\")) {\n"
       "        QThread::msleep(qgetenv(\"IDM_FWD_BLOCK_MS\").toLongLong());\n"
       "    }\n" + old)
assert s.count(old) == 1, s.count(old)
io.open(p, "w", encoding="utf-8", newline="\n").write(s.replace(old, new))
print("[repro] 已在 write 前加阻塞钩子（默认不生效）")
PY
    ;;
  *) echo "用法: $0 <n|block> <轮数>"; exit 2;;
esac

python tools/build_gate.py --targets ui_snapshot >/tmp/repro_build.log 2>&1
BRC=$?
echo "# gate_rc=$BRC （非 0 ⇒ 产物不合格，下面读数全部不作数）"
[ "$BRC" -ne 0 ] && { tail -4 /tmp/repro_build.log; exit 7; }
SHA=$(sha256sum < "$BIN" | cut -c1-12)
TS=$(date +%m%d-%H%M%S)
OUT="docs/forward-repro-$FACE-$TS.txt"
{
  echo "# repo=D:/visual studio/lianxi/MFC/idm-next @ $(git rev-parse --short HEAD) exe_sha12=$SHA"
  echo "# 扰动 face=$FACE ｜判据表达式与阈值未动｜gate_rc=$BRC"
  for i in $(seq 1 "$N"); do
    if [ "$FACE" = block ]; then
      IDM_FWD_BLOCK_MS=2500 IDM_FORWARD_PROBE=1 "$BIN" >/tmp/rp_$i.log 2>&1
    else
      IDM_FORWARD_PROBE=1 "$BIN" >/tmp/rp_$i.log 2>&1
    fi
    RC=$?
    V=$(grep -o '管道状态=[0-9]*，错误码=[0-9-]*' /tmp/rp_$i.log | head -1)
    echo "[face=$FACE run$i] rc=$RC FAIL=$(grep -ac '\[FAIL\]' /tmp/rp_$i.log) 命中写失败分支=$(grep -ac '没写进去' /tmp/rp_$i.log) ${V:-无状态码读数}"
  done
} > "$OUT" 2>&1
echo "档=$OUT"
cp /tmp/rp_1.log "$(dirname "$OUT")/../forward-repro-$FACE-sample-$TS.log" 2>/dev/null || cp /tmp/rp_1.log "/tmp/forward-repro-$FACE-sample-$TS.log"
echo "样本=/tmp/forward-repro-$FACE-sample-$TS.log"
