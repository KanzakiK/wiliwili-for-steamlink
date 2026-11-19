#!/usr/bin/env bash
#
# D26/D27：只消费 debug.sh 归档，不连接设备、不启动应用。
#
# 用法: bash tools/steamlink-app/check-debug-run.sh debug-runs/<run-id>
#
# 检查：
#   - manifest / prestart / 统一日志 / dmesg before-after / launch_cmdline 齐全
#   - CMD start/ok 对应的 dmesg 边界快照齐全
#   - boot 行含同一 run id
#   - 场景有 CMD status=start 与 status=ok
#   - 播放器场景的 session 在 CMD、UI content start、daemon meta 中一致
#   - 统一日志不含签名 URL 参数
set -u

RUN_DIR=${1:-}
if [ -z "$RUN_DIR" ]; then
    echo "用法: $0 debug-runs/<run-id>" >&2
    exit 2
fi

FAILURES=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAILURES=$((FAILURES + 1)); }
check_file() {
    if [ -s "$RUN_DIR/$1" ]; then pass "$1 存在且非空"; else fail "$1 缺失或为空"; fi
}
check_pattern() {
    if grep -F "$2" "$RUN_DIR/$1" >/dev/null 2>&1; then
        pass "$1 含 $3"
    else
        fail "$1 缺少 $3"
    fi
}

check_file manifest.txt
check_file prestart.log
check_file wiliwili.log
check_file dmesg.before.txt
check_file dmesg.after.txt
check_file launch_cmdline.txt

RUN_ID=$(sed -n 's/^run_id=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
SCENE=$(sed -n 's/^scene=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
if [ -z "$RUN_ID" ]; then
    fail "manifest 缺少 run_id"
elif [ "$RUN_ID" != "$(basename "$RUN_DIR")" ]; then
    fail "manifest run_id=$RUN_ID 与目录名不一致"
else
    pass "run_id 与目录一致: $RUN_ID"
fi
if [ -z "$SCENE" ] || [ "$SCENE" = "none" ]; then
    fail "manifest 缺少有效 scene"
fi
check_file "dmesg.$SCENE.start.txt"
check_file "dmesg.$SCENE.ok.txt"

check_pattern wiliwili.log "[boot] run=$RUN_ID" "boot run id"
check_pattern wiliwili.log "[CMD][run=$RUN_ID][session=" "CMD run/session 前缀"
check_pattern wiliwili.log "scene=$SCENE status=start" "CMD status=start"
check_pattern wiliwili.log "scene=$SCENE status=ok" "CMD status=ok"

case "$SCENE" in
    play|seek_forward|seek_backward|pause_resume)
        SESSION=$(grep -F "scene=$SCENE status=ok" "$RUN_DIR/wiliwili.log" 2>/dev/null |
            sed -n 's/.*\[session=\([^]]*\)\].*/\1/p' | grep -v '^-' | head -1)
        if [ -z "$SESSION" ]; then
            fail "播放器场景没有有效 session"
        else
            pass "播放器场景 session=$SESSION"
            check_pattern wiliwili.log "[UI][run=$RUN_ID][session=$SESSION] content start" "UI content start"
            check_pattern wiliwili.log "run=$RUN_ID session=$SESSION" "daemon session"
            check_pattern wiliwili.log "meta ok run=$RUN_ID session=$SESSION" "meta 完成"
        fi
        ;;
esac

if grep -E 'upsig=|trid=|buvid=|[?][^ ]*[=]' "$RUN_DIR/wiliwili.log" >/dev/null 2>&1; then
    fail "统一日志含签名 URL 参数"
else
    pass "统一日志未发现签名 URL 参数"
fi

HEALTH_FORCE=$(sed -n 's/^health_force=//p' "$RUN_DIR/manifest.txt" 2>/dev/null | head -1)
if [ "$HEALTH_FORCE" = "1" ]; then
    check_pattern wiliwili.log "[mpv-compat][health]" "compat 软卡顿检测"
    check_pattern wiliwili.log "[UI][health]" "播放页健康提示"
    HEALTH_LINES=$(grep -c -F '[mpv-compat][health]' "$RUN_DIR/wiliwili.log" 2>/dev/null || true)
    if [ "$HEALTH_LINES" -eq 1 ]; then
        pass "软卡顿提示每个 run 只发生一次"
    else
        fail "软卡顿提示次数=$HEALTH_LINES，期望 1"
    fi
    if grep -E "watchdog 判定卡死|END_FILE\(ERROR\)" "$RUN_DIR/wiliwili.log" >/dev/null 2>&1; then
        fail "软卡顿提示不应触发 watchdog 或 ERROR"
    else
        pass "软卡顿提示未触发 watchdog/ERROR"
    fi
fi

if [ "$FAILURES" -eq 0 ]; then
    echo "RESULT: PASS $RUN_DIR"
    exit 0
fi
echo "RESULT: FAIL $RUN_DIR ($FAILURES)"
exit 1
