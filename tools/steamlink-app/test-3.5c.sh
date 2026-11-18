#!/bin/sh
# 3.5c watchdog / 断连恢复 / 孤儿清理真机回归脚本（在 Steam Link 上运行）
#
# 用法：
#   sh /home/apps/bilibili/3.5c-test/test-3.5c.sh kill-daemon
#   sh /home/apps/bilibili/3.5c-test/test-3.5c.sh freeze-daemon
#   sh /home/apps/bilibili/3.5c-test/test-3.5c.sh kill-parent
#   sh /home/apps/bilibili/3.5c-test/test-3.5c.sh all
#
# 注意：所有日志/视频/二进制都在 /home/apps/bilibili（flash），不写测试视频到 /tmp。
set -e

APP=/home/apps/bilibili/3.5c-test
SOCKET=/tmp/slplayer-ipc-35c.sock
MODE=${1:-all}
PAUSE_AFTER=${SL_TEST_PAUSE_AFTER_SEC:-5}
POLL_MS=${SL_WATCHDOG_POLL_MS:-200}
NOHB_MS=${SL_WATCHDOG_NOHB_MS:-2000}
STALL_MS=${SL_WATCHDOG_STALL_MS:-1500}
GRACE_MS=${SL_WATCHDOG_GRACE_MS:-1000}

test_files() {
    for f in "$APP/slplayer_daemon" "$APP/compat_test" "$APP/test.mp4"; do
        [ -f "$f" ] || { echo "FAIL: 缺文件 $f"; exit 1; }
    done
}

cleanup() {
    local pid
    pid=$(cat "$APP/test.pid" 2>/dev/null || true)
    if [ -n "$pid" ]; then kill -9 "$pid" 2>/dev/null || true; fi
    sleep 1
    local dpid
    dpid=$(daemon_pid)
    if [ -n "$dpid" ]; then kill -9 "$dpid" 2>/dev/null || true; fi
    rm -f "$APP/test.pid" "$SOCKET"
    sleep 1
}

daemon_pid() {
    ps | grep -F "$APP/slplayer_daemon" | grep -v grep | awk '{print $1}' | head -1
}

mpv_pid() {
    ps | grep -F './compat_test ./test.mp4' | grep -v grep | awk '{print $1}' | head -1
}

start_case() {
    local log="$APP/$1.log"
    local pauseAfter=${2:-0}
    local pauseOnPlayback=${3:-0}
    cleanup
    rm -f "$log"
    cd "$APP"
    SL_DAEMON_PATH="$APP/slplayer_daemon" \
    SL_IPC_SOCKET="$SOCKET" \
    SL_WATCHDOG_POLL_MS="$POLL_MS" \
    SL_WATCHDOG_NOHB_MS="$NOHB_MS" \
    SL_WATCHDOG_STALL_MS="$STALL_MS" \
    SL_WATCHDOG_GRACE_MS="$GRACE_MS" \
    SL_TEST_PAUSE_AFTER_SEC="$pauseAfter" \
    SL_TEST_PAUSE_ON_PLAYBACK="$pauseOnPlayback" \
        nohup ./compat_test ./test.mp4 40 >"$log" 2>&1 &
    echo $! >"$APP/test.pid"

    local i=0
    while [ $i -lt 60 ]; do
        if grep -q 'PLAYBACK_RESTART' "$log" 2>/dev/null; then return 0; fi
        i=$((i + 1)); sleep 0.2
    done
    echo "FAIL: 首帧未出现，日志=$log"
    tail -30 "$log"
    exit 1
}

assert_restarted() {
    local old=$1 log=$2
    local i=0 new=""
    # 脏 VPU 清洗在新 daemon 启动时可能耗时数秒，30s 是保守上限。
    while [ $i -lt 150 ]; do
        new=$(daemon_pid)
        if [ -n "$new" ] && [ "$new" != "$old" ]; then break; fi
        i=$((i + 1)); sleep 0.2
    done
    [ -n "$new" ] && [ "$new" != "$old" ] || {
        echo "FAIL: daemon 未重启（old=$old new=$new）"; tail -40 "$log"; exit 1;
    }
    # 新 PID 先出现，恢复 open 日志由接收线程随后写出；再等 10s。
    i=0
    while [ $i -lt 50 ] && ! grep -q '恢复播放:' "$log"; do
        i=$((i + 1)); sleep 0.2
    done
    if ! grep -q '恢复播放:' "$log"; then
        echo "FAIL: 没有恢复播放日志"; tail -40 "$log"; exit 1;
    fi
    if ! grep -q 'daemon 退出(reason=.*signal=9(SIGKILL)' "$log"; then
        echo "FAIL: 没有 SIGKILL 退出状态日志"; tail -40 "$log"; exit 1;
    fi
    echo "PASS: old=$old new=$new"
    grep -E 'daemon 退出|恢复播放|watchdog 判定|playback-time =' "$log" | tail -12
}

case_kill_daemon() {
    echo "== kill-daemon =="
    start_case kill-daemon
    old=$(daemon_pid)
    [ -n "$old" ] || { echo "FAIL: 启动时没有 daemon"; exit 1; }
    before=$(grep 'playback-time =' "$APP/kill-daemon.log" | tail -1)
    kill -9 "$old"
    assert_restarted "$old" "$APP/kill-daemon.log"
    sleep 3
    after=$(grep 'playback-time =' "$APP/kill-daemon.log" | tail -1)
    echo "进度: before=[$before] after=[$after]"
    cleanup
}

case_kill_daemon_paused() {
    echo "== kill-daemon-paused =="
    start_case kill-daemon-paused 0 1
    # 首帧后再暂停，避免开流慢时暂停落在首帧之前。
    local i=0
    while [ $i -lt 100 ] && ! grep -q '首帧后，3.5c' "$APP/kill-daemon-paused.log"; do
        i=$((i + 1)); sleep 0.2
    done
    grep -q '模拟用户暂停' "$APP/kill-daemon-paused.log" || {
        echo "FAIL: 暂停命令未注入"; tail -30 "$APP/kill-daemon-paused.log"; exit 1;
    }
    local old
    old=$(daemon_pid)
    [ -n "$old" ] || { echo "FAIL: 启动时没有 daemon"; exit 1; }
    kill -9 "$old"
    assert_restarted "$old" "$APP/kill-daemon-paused.log"
    if ! grep -q '恢复播放:.*pause=1' "$APP/kill-daemon-paused.log"; then
        echo "FAIL: 恢复时未保留暂停态"; tail -40 "$APP/kill-daemon-paused.log"; exit 1;
    fi
    echo "PASS: 暂停中杀 daemon 后恢复暂停态"
    cleanup
}

case_freeze_daemon() {
    echo "== freeze-daemon =="
    start_case freeze-daemon
    old=$(daemon_pid)
    [ -n "$old" ] || { echo "FAIL: 启动时没有 daemon"; exit 1; }
    kill -STOP "$old"
    assert_restarted "$old" "$APP/freeze-daemon.log"
    # 冻结进程时，「无新帧」阈值 1.5s 先于「无心跳」2s 命中，两者都是有效判定。
    if ! grep -q 'watchdog 判定卡死' "$APP/freeze-daemon.log"; then
        echo "FAIL: watchdog 未判定卡死"; tail -40 "$APP/freeze-daemon.log"; exit 1;
    fi
    echo "PASS: watchdog 已强杀冻结进程并恢复"
    cleanup
}

case_kill_parent() {
    echo "== kill-parent =="
    start_case kill-parent
    old=$(daemon_pid)
    parent=$(cat "$APP/test.pid")
    [ -n "$old" ] && [ -n "$parent" ] || { echo "FAIL: 缺少父/子 PID"; exit 1; }
    kill -9 "$parent"
    rm -f "$APP/test.pid" # 父进程已死，避免 cleanup 误杀复用后的 PID
    local i=0 new="$old"
    while [ $i -lt 20 ]; do
        new=$(daemon_pid)
        [ -z "$new" ] && break
        i=$((i + 1)); sleep 0.2
    done
    [ -z "$new" ] || { echo "FAIL: 父进程死后 daemon 未退出 pid=$new"; exit 1; }
    if [ -e /tmp/slplayer_vpu.lock ]; then
        echo "FAIL: Dirty Flag 未清除"; ls -l /tmp/slplayer_vpu.lock; exit 1;
    fi
    echo "PASS: 父进程死后 daemon 已清理，Dirty Flag 已删除"
    cleanup
}

test_files
case "$MODE" in
    kill-daemon)       case_kill_daemon ;;
    kill-daemon-paused) case_kill_daemon_paused ;;
    freeze-daemon) case_freeze_daemon ;;
    kill-parent)  case_kill_parent ;;
    all) case_kill_daemon; case_kill_daemon_paused; case_freeze_daemon; case_kill_parent ;;
    *) echo "未知模式: $MODE"; exit 2 ;;
esac
echo "ALL_DONE"
