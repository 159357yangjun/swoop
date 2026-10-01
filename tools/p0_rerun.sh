#!/bin/bash
# P0/MC-3 的红基线重跑器。与旧的一次性 s0_run.sh 同逻辑，两处不同是有原因的：
#   ① 输出名带时刻戳（§7 第 8.1 条：被制品引用过的读数文件不得用固定名重写——
#      旧脚本写死 s0b-selftest.log，把 01:41 那轮原地覆盖了，sha12=ce59a2f0362b 不可恢复）。
#   ② 起服务前强制"端口上零个监听"、起完强制"恰好 1 个"：Windows 的 SO_REUSEADDR 允许多个
#      进程 bind 同一端口，读到的 conns/reqs 可能是别人家的计数器 ⇒ 假绿与假红同源。
# 用法：bash tools/p0_rerun.sh [日志目录]      （默认 %TEMP%/idm-probes/p0-runs）
set -u
cd "$(dirname "$0")/.."
D="${1:-$TEMP/idm-probes/p0-runs}"
mkdir -p "$D" || { echo "RUNDIR_FAIL cannot create $D"; exit 5; }
STAMP=$(date +%m%d-%H%M%S)
SLOG="$D/selftest-$STAMP.log"
VLOG="$D/server-$STAMP.log"
[ -e "$SLOG" ] && { echo "PREEXISTS refuses to overwrite $SLOG"; exit 6; }
PORT=18080; SUSER=idmuser; SPASS=idmpass
LISTENERS() { netstat -ano | tr -d '\r' | iconv -f gbk -t utf-8 2>/dev/null | grep -i "127.0.0.1:$PORT" | grep -ci LISTENING; }
N0=$(LISTENERS); echo "preflight_listening=$N0"
[ "$N0" = "0" ] || { echo "PREFLIGHT_FAIL port_busy pid=$(netstat -ano | tr -d '\r' | grep -i "127.0.0.1:$PORT" | grep -i LISTENING | head -1 | awk '{print $NF}')"; exit 3; }
python tools/engine_selftest_server.py $PORT $SUSER $SPASS > "$VLOG" 2>&1 &
SRV=$!; echo "server_msys_pid=$SRV"
READY=0
for i in $(seq 1 60); do
  CODE=$(curl -s --noproxy '*' --max-time 3 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/_stats") || CODE=000
  [ "$CODE" = "200" ] && { READY=1; break; }
  kill -0 $SRV 2>/dev/null || { echo "SERVER_DIED_EARLY"; tail -20 "$VLOG"; exit 4; }
  sleep 1
done
echo "ready=$READY"
N1=$(LISTENERS); WPID=$(netstat -ano | tr -d '\r' | grep -i "127.0.0.1:$PORT" | grep -i LISTENING | head -1 | awk '{print $NF}')
echo "after_start_listening=$N1 windows_pid=$WPID"
[ "$N1" = "1" ] || { echo "PREFLIGHT_FAIL listeners_not_1"; exit 3; }
echo "stats_before=$(curl -s --noproxy '*' "http://127.0.0.1:$PORT/_stats")"
date +"T3_start=%Y-%m-%d %H:%M:%S %z"
export IDM_SELFTEST_MAX_LIFE=300
RC=0; ./build/engine_selftest.exe $PORT $SUSER $SPASS > "$SLOG" 2>&1 || RC=$?
echo "SELFTEST_RC=$RC"
for i in $(seq 1 40); do kill -0 $SRV 2>/dev/null || break; sleep 0.5; done
kill -0 $SRV 2>/dev/null && echo "server_still_alive=yes(/_shutdown 没生效)" || echo "server_still_alive=no"
date +"T3_end=%Y-%m-%d %H:%M:%S %z"
echo "exe_mtime=$(stat -c '%y' build/engine_selftest.exe)  exe_sha12=$(sha256sum < build/engine_selftest.exe | cut -c1-12)"
echo "SLOG=$SLOG"
grep -a "结果：" "$SLOG" | tail -1 | tr -d '\r'
echo "FAIL_rows=$(grep -ac '^  \[FAIL\]' "$SLOG" || true)"
grep -a '^  \[FAIL\]' "$SLOG" | tr -d '\r'
# 无条件印：所有带 error_msg 的读数行，**绿侧也印**。
# 这一行存在的理由：旧版只在失败行里印 error_msg ⇒ 通过时盘上一行证据都没有，
# 于是 "STATE_CLAMP_rows=0" 会被读成"引擎没写自我报告"，而真相可能只是测具没印。
echo "--- error_msg 读数（无条件）---"
grep -a "error_msg=" "$SLOG" | tr -d '\r'
echo "error_msg_rows=$(grep -ac 'error_msg=' "$SLOG" || true) STATE_CLAMP_rows=$(grep -ac 'STATE-CLAMP' "$SLOG" || true)"
exit $RC
