#!/usr/bin/env bash
# 「必变」夹具跑法：注掉 app_icons.cpp 里一个 switch case → IDM_FILEICON_PROBE 必须红 → 拔了就绿。
# 存在的理由见 tools/README-assertion-fixtures.md：对照式断言（改前==改后）对「两侧同时变坏」是瞎的，
# 所以每条这样的断言都要配一个已知会红的夹具。任何一步不满足就非 0 退出。
set -u
cd "$(dirname "$0")/.." || exit 2
SRC=src/gui/app_icons.cpp
CASE='        case FileType::Compressed: return cachedIcon(Glyph::Compressed, Tint::Amber);'
LOGDIR="${TMPDIR:-/tmp}/idm-probes/mutation-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$LOGDIR"
export QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR="C:/Windows/Fonts"

before=$(git diff --numstat -- "$SRC"); echo "[fixture] 改前 numstat = [${before:-（无改动）}]"

printf '%s\n' "$CASE" | grep -q . || { echo "[fixture] 夹具锚点为空"; exit 2; }
grep -qF "$CASE" "$SRC" || { echo "[fixture] 锚点在 $SRC 里找不到（实现改过？夹具要同步更新）"; exit 2; }
cp "$SRC" "$LOGDIR/app_icons.orig"

python - "$SRC" "$CASE" <<'PY' || { echo "[fixture] 注入失败或空操作"; exit 2; }
import io, sys
p, case = sys.argv[1], sys.argv[2]
s = io.open(p, encoding="utf-8", newline="").read()
assert case in s, "anchor missing"
s2 = s.replace(case + "\n", "", 1)
assert s2 != s, "NO-OP"
io.open(p, "w", encoding="utf-8", newline="").write(s2)
print("[fixture] 已注掉 1 个 case（自证：文件比原来短 %d 字节）" % (len(s) - len(s2)))
PY

echo "[fixture] 构建中…"
if ! cmake --build build > "$LOGDIR/build.log" 2>&1; then
  echo "[fixture] 构建失败（不拿陈旧二进制下结论），日志：$LOGDIR/build.log"; tail -5 "$LOGDIR/build.log"
  cp "$LOGDIR/app_icons.orig" "$SRC"; exit 1
fi
grep -qE "Building CXX object .*app_icons\.cpp\.obj" "$LOGDIR/build.log" || {
  echo "[fixture] 构建输出里没有 app_icons.cpp.obj 的重编行 ⇒ 没真重编，结果不记账。日志：$LOGDIR/build.log"
  cp "$LOGDIR/app_icons.orig" "$SRC"; exit 1
}
IDM_FILEICON_PROBE=1 ./build/ui_snapshot.exe > "$LOGDIR/probe_mutated.log" 2>&1; RC=$?
grep -q "\[FAIL\] 六个取值的图标两两不同" "$LOGDIR/probe_mutated.log" && SAWFAIL=1 || SAWFAIL=0
echo "[fixture] 变异态：探针 rc=$RC，第 [7] 条报红=$SAWFAIL（期望 rc!=0 且 1）"
grep -E "PASS|FAIL" "$LOGDIR/probe_mutated.log" | sed 's/^/    /'

cp "$LOGDIR/app_icons.orig" "$SRC"
after=$(git diff --numstat -- "$SRC"); echo "[fixture] 还原后 numstat = [${after:-（无改动）}]"
[ "${before:-}" = "${after:-}" ] || { echo "[fixture] 还原与改前不一致，不许收工"; exit 1; }

cmake --build build > "$LOGDIR/build_restored.log" 2>&1 || { echo "[fixture] 还原后构建失败"; exit 1; }
IDM_FILEICON_PROBE=1 ./build/ui_snapshot.exe > "$LOGDIR/probe_restored.log" 2>&1; RC2=$?
echo "[fixture] 还原态：探针 rc=$RC2（期望 0）"
if [ "$RC" = "0" ] || [ "$SAWFAIL" != "1" ] || [ "$RC2" != "0" ]; then
  echo "[fixture] 判定：夹具没牙或断言失效（变异 rc=$RC 报红=$SAWFAIL / 还原 rc=$RC2）"
  echo "[fixture] 全部日志在 $LOGDIR"; exit 1
fi
echo "[fixture] 判定：夹具有效 —— 注 case 必红、拔了必绿。日志目录 $LOGDIR"
