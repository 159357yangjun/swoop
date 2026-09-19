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
    # /T 连子进程一起杀：Edge 会 fork 一堆 renderer/gpu 进程，只杀父进程会留一地孤儿
    MSYS_NO_PATHCONV=1 taskkill //F //T //PID $EDGE_PID >/dev/null 2>&1
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
echo "--- 残留 Edge（应为 0）: $(MSYS_NO_PATHCONV=1 wmic process where "name='msedge.exe'" get commandline 2>/dev/null | grep -c 'ext-edge-profile') ---"
exit $RC
