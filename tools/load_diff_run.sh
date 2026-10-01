#!/bin/bash
# 负载差分实验：把 IDM_ACTIVATE_PROBE 那条 1 红/7 次的 "<500ms" 读数归到「机器负载」还是「产品真会堵 GUI 线程」。
#
# 三条纪律（都是踩过才写的）：
#  ① 变量一律 `export VAR=1; 程序`。走 `env VAR=1 程序` 在本机静默不跑、还回 0（假绿），四面实测见 ui_probe_run.sh 头。
#  ② 退码在管道之前取（`"$BIN" > log 2>&1 || RC=$?`），不写 `... | tail` 再接 `rc=$?`——那吃的是 tail 的退码。
#  ③ 本脚本**不改判据、不调阈值**：500ms 那条照原样进报告行；这里只加"这一轮的时间是谁花的"的读数。
#     归因走两条独立证人：(a) 机器面标定（同量功的忙等在两种负载下的耗时）；
#     (b) 产品自己的轮内消融 IDM_ROUND_ATTR=1 ⇒ 同一进程里「有定时器→停定时器→再有定时器」三窗口（on/off/on 排单调漂移）。
#  ④ 负载是**我自己起的** python 忙等，收尾只按我记录的 PID 杀；别人会话的进程一个都不碰。
set -u
cd "$(dirname "$0")/.."
BIN=build/ui_snapshot.exe
WANT_SHA=${WANT_SHA:-e9ebbba101e2}
N=${N_RUNS:-5}
LOAD_WORKERS=${LOAD_WORKERS:-16}
LOAD_SECONDS=${LOAD_SECONDS:-110}
D=${1:-$TEMP/idm-probes/loaddiff-$(date +%m%d-%H%M%S)}
mkdir -p "$D" || { echo "RUNDIR_FAIL $D"; exit 5; }
export QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR="C:/Windows/Fonts" QT_LOGGING_TO_CONSOLE=1
[ -x "$BIN" ] || { echo "MISSING $BIN ⇒ 需要重编才能跑，本实验边界禁止重编，停下"; exit 6; }
GOT_SHA=$(sha256sum < "$BIN" | cut -c1-12)
[ "$GOT_SHA" = "$WANT_SHA" ] || { echo "SHA_MISMATCH got=$GOT_SHA want=$WANT_SHA ⇒ 不是同一个二进制，实验作废"; exit 3; }
echo "exe_sha12=$GOT_SHA modes=IDM_ACTIVATE_PROBE(+IDM_ROUND_ATTR) N=$N LOAD_WORKERS=$LOAD_WORKERS LOAD_SECONDS=$LOAD_SECONDS"
echo "cores=$NUMBER_OF_PROCESSORS"

# 预检：IPC 名上有别的实例 ⇒ 整个实验作废（这条探针自带守卫，但要在这里响亮地停）
if netstat -ano 2>/dev/null | grep -q ':15211'; then
  echo "PREFLIGHT_FAIL 15211 已被占用 ⇒ 先关掉在跑的 IDM Next 实例"; exit 4
fi

census() {
  tasklist //FO CSV //NH 2>/dev/null | awk -F'","' '{print $1}' | tr -d '"' | sort | uniq -c | sort -rn | head -9 | tr '\n' ';'
  echo
}
# 机器面标定：固定功量的纯忙等（与产品无关），报毫秒
calib() {
  python -c 'import time
s=time.perf_counter(); x=0
for i in range(3000000): x+=i*i
print(int((time.perf_counter()-s)*1000))' 2>/dev/null || echo -1
}

WPIDS=""
WORKER_SEQ=0
IDLE_MED_CALIB=""                     # 留空：由本轮**空闲面自己**算出来（写死一个"典型 520ms"就是过期读数）
RATIO_MIN=1.5                           # 规则：负载面每轮的标定必须 ≥1.5× 空闲中位，否则这轮不算负载面
rearm_load() {                          # 补满 worker（上一批可能到点退了），负载窗口因此可以拉长
  local dl=$(( $(date +%s) + LOAD_SECONDS )) n=0
  for p in $WPIDS; do kill -0 "$p" 2>/dev/null && n=$((n+1)); done
  while [ "$n" -lt "$LOAD_WORKERS" ]; do
    WORKER_SEQ=$((WORKER_SEQ + 1))
    ef="$D/worker-$WORKER_SEQ.err"
    python tools/load_worker.py "$dl" 2> "$ef" &
    WPIDS="$WPIDS $!"; n=$((n+1))
    sleep 0.2
    [ -s "$ef" ] && echo "  !! worker-$WORKER_SEQ 一起来就报错：$(head -2 "$ef" | tr '\n' ' ')"
  done
}
start_load() {
  local dl=$(( $(date +%s) + LOAD_SECONDS ))
  for ((k=1;k<=LOAD_WORKERS;k++)); do
    python tools/load_worker.py "$dl" 2> "$D/worker-$k.err" &
    WPIDS="$WPIDS $!"
  done
  local alive=$(jobs -p 2>/dev/null | wc -l)
  echo "load_started workers=$LOAD_WORKERS deadline_seconds=$LOAD_SECONDS pids=($(echo $WPIDS | tr ' ' ',')) alive_now=$alive"
  echo "worker_stderr_非空数=$(find "$D" -name 'worker-*.err' -size +0c | wc -l)（>0 就是发生器自己没跑起来，负载面读数全部作废）"
}
stop_load() {
  for p in $WPIDS; do kill "$p" 2>/dev/null; done
  WPIDS=""; echo "load_stopped（只杀自己起的 PID，别人会话的一个都不碰）"
}
trap stop_load EXIT

run_face() {  # $1=面名  $2=第几面（写 calib 用）
  local face=$1
  echo "=== 面 $face 开始 $(date +%H:%M:%S) 进程普查: $(census) ==="
  for ((i=1;i<=N;i++)); do
    local pre post RC=0 t0 t1 dur ratio alive
    [ "$face" = "load" ] && rearm_load
    pre=$(calib); t0=$(date +%s)
    export IDM_ACTIVATE_PROBE=1 IDM_ROUND_ATTR=1
    "$BIN" > "$D/$face-$i.log" 2>&1 || RC=$?
    unset IDM_ACTIVATE_PROBE IDM_ROUND_ATTR
    t1=$(date +%s); post=$(calib)
    # 存活数用 kill -0 真查（上一版用 `jobs -p | wc -l`，那是 job 表条目：worker 早退了计数还挂着，
    # idle_after 面因此印出"worker 存活=16"这种假话。负载的真实证人改由下面的 calib 比值承担。）
    alive=0; for p in $WPIDS; do kill -0 "$p" 2>/dev/null && alive=$((alive+1)); done
    dur=$((t1 - t0))
    # 空闲面时 IDLE_MED_CALIB 还没算出来 ⇒ 负载比写成 -1（"不适用"），不拿 1 冒充"正好等于基线"
    if [ -z "$IDLE_MED_CALIB" ]; then ratio=-1; else ratio=$(python -c "print(round($pre/$IDLE_MED_CALIB,2))"); fi
    echo "$face $i $pre $post $dur $alive $ratio" >> "$D/calib.txt"
    local worst btrip
    worst=$(grep -ao '单轮最长=[0-9]*ms' "$D/$face-$i.log" | head -1 | tr -dc '0-9')
    btrip=$(grep -ao 'B 段放弃后往返=[0-9]*ms' "$D/$face-$i.log" | head -1 | tr -dc '0-9')
    printf '%s-%s rc=%-3s 单轮最长=%-6s B段往返=%-5s calib=%s/%sms 该轮墙钟=%ss worker存活=%s 负载比=%s  %s\n' \
      "$face" "$i" "$RC" "${worst:-?}" "${btrip:-?}" "$pre" "$post" "$dur" "$alive" "$ratio" "$D/$face-$i.log"
    [ "$face" = "load" ] && { python -c "import sys; sys.exit(0 if float('$ratio')>=$RATIO_MIN else 1)" \
        || echo "  !! 该轮负载比=$ratio < $RATIO_MIN ⇒ **不在负载下**，本轮不计入负载面（上一轮就是这么把空载面当成负载面的）"; }
    grep -q '已被别的 IDM Next 实例占用' "$D/$face-$i.log" && echo "  !! 该轮被别的实例挡在门外 ⇒ 这一轮作废"
  done
  echo "=== 面 $face 结束 $(date +%H:%M:%S) ==="
}

: > "$D/calib.txt"
run_face idle
# 空闲基线由**本轮空闲面自己**算，不写死任何实测数（阈值 1.5 是规则，520 那种数不是）
# 中位用 awk 算，别让 python 去 open("/tmp/…")：MSYS 的 /tmp 与 python 的 /tmp 不是同一个地方（踩过）
IDLE_MED_CALIB=$(grep '^idle ' "$D/calib.txt" | awk '{print $3}' | sort -n   | awk '{a[NR]=$1} END{if(NR==0){print 0} else if(NR%2){print a[(NR+1)/2]} else {print int((a[NR/2]+a[NR/2+1])/2)}}')
[ "$IDLE_MED_CALIB" -gt 0 ] || { echo "IDLE_BASELINE_FAIL 空闲面一条标定都没采到 ⇒ 负载比无法定义，整轮作废"; exit 8; }
echo "idle_median_calib=${IDLE_MED_CALIB}ms（本轮实测，负载比以它为分母；规则阈值 RATIO_MIN=$RATIO_MIN）"

# 负载面：先起负载，等标定读数确认机器真的变慢了，再跑
start_load
sleep 3
echo "load_witness calib_under_load=$(calib)ms calib_idle_baseline=$(head -2 "$D/calib.txt" | awk '{s+=$3} END{print int(s/NR)}')ms"
FACE_L_START=$(date +%s)
run_face load
stop_load
FACE_L_SEC=$(( $(date +%s) - FACE_L_START ))

# 等长但无负载的第三面：排除"只是时间过去了"
echo "=== 静置等长 ${FACE_L_SEC}s（无我起的负载）以排除单调漂移 $(date +%H:%M:%S) ==="
sleep "$FACE_L_SEC"
run_face idle_after

echo "=== 分布汇总 ==="
WIND=$(cygpath -w "$D" 2>/dev/null || echo "$D")
python tools/load_diff_stat.py "$WIND" "$N"
RC_STAT=$?

S="$USERPROFILE/idm_shots"
if [ -d "$S" ]; then
  rm -f "$S"/0*.png "$S"/_data/tasks.json "$S"/_data/history.db 2>/dev/null
  echo "idm_shots 已清（截图/tasks.json/history.db）；_data\\aria2|ffmpeg|yt-dlp 保留"
fi
exit $RC_STAT
