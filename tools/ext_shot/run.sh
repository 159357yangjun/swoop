#!/usr/bin/env bash
# 起 headless Edge → 跑 popup 截图/断言 → 无论成败都关掉浏览器。
#
#   bash tools/ext_shot/run.sh
#
# 为什么要写成"一个脚本"：WorkBuddy 的 Bash 调用结束后，用 `&` 起的后台进程会被回收
# （浏览器会"凭空消失"），跨调用跑只会得到一片连接失败。
# 浏览器用独立 user-data-dir，不碰用户日常在用的 Edge 配置；退出时按 PID 连子进程一起杀。
set -u

PORT=9333
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PROFILE="$ROOT/build/ext-edge-profile"
OUT="$ROOT/build/ext-shots"

EDGE="/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"
[ -f "$EDGE" ] || EDGE="/c/Program Files/Microsoft/Edge/Application/msedge.exe"
[ -f "$EDGE" ] || EDGE="/c/Program Files/Google/Chrome/Application/chrome.exe"
[ -f "$EDGE" ] || { echo "找不到 Edge/Chrome"; exit 2; }

NODE="C:/Users/yyyy/.workbuddy/binaries/node/versions/22.22.2-3/node.exe"

mkdir -p "$OUT"
rm -rf "$PROFILE"

"$EDGE" --headless=new --remote-debugging-port=$PORT \
        --user-data-dir="$(cygpath -w "$PROFILE")" \
        --no-first-run --no-default-browser-check --disable-gpu \
        --allow-file-access-from-files \
        about:blank > "$ROOT/build/ext-edge.log" 2>&1 &
EDGE_PID=$!

cleanup() {
    # ① 按 PID 连子进程树一起杀。⚠️ 前面**不能**加 MSYS_NO_PATHCONV=1：
    #    那个变量只是关掉路径转换，于是 //F 原样传给 taskkill → 「无效参数」，
    #    而错误又被 >/dev/null 吞掉 —— 这条清理从来没真的生效过，孤儿留了一地。
    taskkill //F //T //PID "$EDGE_PID" >/dev/null 2>&1
    # ② 再按「我们自己的 profile 目录」精确补杀孤儿。
    #    绝不按进程名一刀切：用户自己开着的浏览器同样是 msedge.exe。
    powershell.exe -NoProfile -Command \
"Get-CimInstance Win32_Process -Filter \"Name='msedge.exe'\" | Where-Object { \$_.CommandLine -match 'ext-edge-profile' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force -ErrorAction SilentlyContinue }" \
        >/dev/null 2>&1
}

# 数一遍还剩几个我们的 headless Edge。
# ⚠️ 不要用 wmic ... | grep：wmic 的输出是 UTF-16，管道进 grep 只能数出 0，
#    之前这条自检就是靠它"永久绿"的 —— 实际系统里还挂着 15 个孤儿进程。
count_orphans() {
    local tmp="$ROOT/build/ext-orphan-count.txt" n
    powershell.exe -NoProfile -Command \
"Get-CimInstance Win32_Process -Filter \"Name='msedge.exe'\" | Where-Object { \$_.CommandLine -match 'ext-edge-profile' } | Measure-Object | Select-Object -ExpandProperty Count | Out-File -Encoding ascii '$(cygpath -w "$tmp")'" \
        >/dev/null 2>&1
    n=$(tr -d '\r\n ' < "$tmp" 2>/dev/null)
    rm -f "$tmp"
    [ -n "$n" ] || n=0
    echo "$n"
}
trap cleanup EXIT

for i in $(seq 1 40); do
    if curl -s --noproxy '*' -o /dev/null "http://127.0.0.1:$PORT/json/version"; then break; fi
    sleep 0.25
done

# ⚠️ 必须给 Node 传 Windows 风格路径：Git Bash 的 /d/xxx 会被 Node 解析成 D:\d\xxx
# 第 1 个参数可选：all（默认）/ popup / content —— 只跑其中一套，省时间
WHICH="${1:-all}"
RC=0
if [ "$WHICH" = "all" ] || [ "$WHICH" = "popup" ]; then
    "$NODE" "$(cygpath -w "$ROOT/tools/ext_shot/shot.mjs")" "$PORT" \
            "$(cygpath -w "$ROOT/browser-extension/popup.html")" "$(cygpath -w "$OUT")"
    RC=$?
fi
if [ "$WHICH" = "all" ] || [ "$WHICH" = "content" ]; then
    "$NODE" "$(cygpath -w "$ROOT/tools/ext_shot/content-shot.mjs")" "$PORT" \
            "$(cygpath -w "$ROOT/tools/ext_shot/content-harness.html")" "$(cygpath -w "$OUT")"
    RC=$?
fi

cleanup
trap - EXIT
sleep 1
RES=$(count_orphans)
PORT_OPEN=0
if netstat -ano | grep LISTENING | grep -q ":$PORT[^0-9]"; then PORT_OPEN=1; fi
echo "--- 残留 Edge（应为 0）: $RES ｜ 调试端口 $PORT 仍监听: $PORT_OPEN（应为 0）---"
if [ "$RES" != "0" ] || [ "$PORT_OPEN" != "0" ]; then
    # 自检不通过就不许报绿：断掉后台常驻是这条基线的硬要求，不是"最好有"。
    echo "★ 清理没做干净，本次基线不算通过（截图与断言结果仍见上行输出）"
    [ "$RC" = "0" ] && RC=1
fi
exit $RC
