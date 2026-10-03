#!/bin/bash
# 本地跑全部 UI 探针模式（CI 里那 17 个模式的同一批入口，这里是**离屏**本机版）。
# 三条规矩是踩过才写进来的：
#  ① 变量传法本轮实测过四面（同一个 build/ui_snapshot.exe、同一个 IDM_ZOOM_PROBE）：
#       `VAR=1 exe`（真前缀赋值）      → 546 字节 / PASS=4  跑到了
#       `export VAR=1; exe`            → 546 字节 / PASS=4  跑到了（本脚本走这条）
#       `env VAR=1 exe`                → 0 字节   / rc=0    **静默没跑＝假绿**
#       tools/quiet_run.py 那条通道同上（上一轮踩的，本轮没重测）
#     走 `export` 而不是前缀赋值，是因为模式名在变量里、只有 `eval` 才能拼出前缀赋值形式，
#     而 eval 的输入是从二进制 `strings` 扫出来的名字——虽然 `grep -oE 'IDM_[A-Z_]+_PROBE'`
#     是白名单，也不值得为省一行开这个口。
#     （另注：第一次复算 `VAR=1 exe` 时得到 rc=127，那是我把整段加引号写成了 `"$M=1" "$BIN"`
#      ⇒ bash 不再把它当赋值、而当命令名去找，**这是我测法的破，不是机制的破**，别据此写"前缀赋值不行"。）
#     所以这里数 **[PASS] 行数**，光看退出码分不出"没跑"。
#  ② 每个模式的完整 stdout+stderr 落盘成**带模式名与时刻戳**的文件（被引用过的读数不得用固定名重写）。
#  ③ IDM_IPC_PROBE 会因"已有别的实例在监听"直接返回 1 ⇒ 起前先查；探针会往 ~/idm_shots 写
#     截图与 _data\tasks.json / history.db，收尾要清这三样，**但 _data\aria2|ffmpeg|yt-dlp 是用户自己的
#     工具缓存，绝对不能删**。
set -u
cd "$(dirname "$0")/.."
D="${1:-$TEMP/idm-probes/ui-$(date +%m%d-%H%M%S)}"
mkdir -p "$D" || { echo "RUNDIR_FAIL $D"; exit 5; }
export QT_QPA_PLATFORM=offscreen
export QT_QPA_FONTDIR="C:/Windows/Fonts"
export QT_LOGGING_TO_CONSOLE=1
BIN=build/ui_snapshot.exe
[ -x "$BIN" ] || { echo "MISSING $BIN — 先 ninja -C build ui_snapshot（要 -DIDM_BUILD_UI_SNAPSHOT=ON）"; exit 6; }

# ── 新鲜度门（会拦退码，不是日志）──
# 踩过的坑：编译 FAILED 的那一轮里我照样看到六个 rc=0 —— 它们吃的是**旧二进制**。
# 红会被看到，假绿直接进结论，所以断言之前必须先证明"产物含本轮改动"。
# J1 BUILD_RC!=0 ⇒ 退 3；J2 exe 比它的输入旧 ⇒ 退 4。两面各有反面夹具：python tools/build_gate.py --selftest
echo "--- 新鲜度门 ---"
PYTHONIOENCODING=utf-8 python tools/build_gate.py --targets ui_snapshot
GATE_RC=$?
if [ "$GATE_RC" -ne 0 ]; then
  echo "BUILD_GATE rc=$GATE_RC ⇒ 整轮作废，不跑任何探针"
  exit $((8 + GATE_RC))     # 11=编译失败 12=产物过期
fi

MODES=$(strings -n 6 "$BIN" 2>/dev/null | grep -aoE 'IDM_[A-Z_]+_PROBE' | sort -u)
[ -n "$MODES" ] || { echo "NOMODES 从二进制里一个模式名都没扫到 ⇒ 整轮作废"; exit 7; }
echo "exe_sha12=$(sha256sum < "$BIN" | cut -c1-12) modes=$(echo "$MODES" | wc -l)"
NOSHO=0; TOTAL_PASS=0; TOTAL_FAIL=0; FAILED=""
for m in $MODES; do
  log="$D/$m.log"
  : > "$log"
  case "$m" in
    IDM_ICON_PROBE|IDM_ARROW_PROBE) kind="人眼对照表（无断言，刻意排除在闸门名单外）" ;;
    *) kind="断言型" ;;
  esac
  # ACTIVATE 额外带 IDM_ROUND_ATTR=1：12:05 那次红（单轮最长=3263ms ≥500）**本该自动跑消融**，
  # 但消融由 env 闸门控制、sweep 没开 ⇒ 唯一一次最想归因的复现，日志里连一行 [attr] 都没有。
  # 不开销别的轮：不复现时它只打印"本轮未复现⇒跳过消融"，复现那轮才有 on/off/on 三个窗口。
  RC=0; export "$m=1"; [ "$m" = "IDM_ACTIVATE_PROBE" ] && export IDM_ROUND_ATTR=1
  "$BIN" > "$log" 2>&1 || RC=$?; unset "$m"; unset IDM_ROUND_ATTR
  P=$(grep -ac '\[PASS\]' "$log" || true); F=$(grep -ac '\[FAIL\]' "$log" || true)
  if [ "$m" != "IDM_ICON_PROBE" ] && [ "$m" != "IDM_ARROW_PROBE" ]; then
    TOTAL_PASS=$((TOTAL_PASS + P)); TOTAL_FAIL=$((TOTAL_FAIL + F))
    [ "$F" -gt 0 ] && FAILED="$FAILED $m(F=$F,rc=$RC)"
    if [ "$((P+F))" -eq 0 ]; then NOSHO=$((NOSHO+1)); FAILED="$FAILED $m(一条断言都没有⇒不算通过)"; fi
  fi
  printf '%-24s rc=%-3s PASS=%-3s FAIL=%-3s %s  %s\n' "$m" "$RC" "$P" "$F" "$kind" "$log"
done
echo "--- 汇总 ---"
echo "PASS=$TOTAL_PASS FAIL=$TOTAL_FAIL 零断言模式=$NOSHO"
echo "问题模式:${FAILED:- 无}"
echo "LOGDIR=$D"
# 收尾：清探针产物，**不动**用户自己的工具缓存
S="$USERPROFILE/idm_shots"
if [ -d "$S" ]; then
  rm -f "$S"/0*.png "$S"/_data/tasks.json "$S"/_data/history.db 2>/dev/null
  echo "idm_shots 已清（截图/tasks.json/history.db）；_data\\aria2|ffmpeg|yt-dlp 保留"
fi
[ "$TOTAL_FAIL" -eq 0 ] && [ "$NOSHO" -eq 0 ] || exit 1
exit 0
