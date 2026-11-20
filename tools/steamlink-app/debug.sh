#!/usr/bin/env bash
#
# D27 统一 debug 启动入口（在开发机/WSL 运行）。
#
# 职责：生成 run id、拉取旧日志、设置 launch_cmdline、前台启动 wiliwili、
# 采集 dmesg，退出时把统一日志与附件拉回 debug-runs/<run-id>/。
#
# 用法：
#   DEVICE=root@<ip> PASS=<password> sh tools/steamlink-app/debug.sh \
#     --content video:BV1Nvx16TEzY --scene seek_forward \
#     --arg target=120 --seconds 30 --sync-offset-ms 20 --diag --sync-debug
#     --health-force --health-window-ms 1000 --health-progress-ms 100000
#
# 注意：本脚本不改正式启动入口；wiliwili.sh 仍负责清空 /tmp/wiliwili.log。
# 启动前必须先拉回旧日志，拉取失败则中止，避免丢历史。
set -u

PROJECT_DIR=${PROJECT_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}
DEVICE=${DEVICE:-}
PASS=${PASS:-}
CONTENT=${CONTENT:-}
SCENE=${SCENE:-}
RUN_SECONDS=${RUN_SECONDS:-30}
SYNC_OFFSET_MS=${SYNC_OFFSET_MS:-}
HEALTH_FORCE=0
HEALTH_WINDOW_MS=${HEALTH_WINDOW_MS:-}
HEALTH_PROGRESS_MS=${HEALTH_PROGRESS_MS:-}
HEALTH_NET_GRACE_MS=${HEALTH_NET_GRACE_MS:-}
AUDIO_DUMP=0
DEBUG_ARGS=()
SCENE_ARGS=()
WILI_FLAGS=()
RUN_ID=""
RUN_DIR=""
STARTED=0
COLLECTED=0

usage() {
    sed -n '2,18p' "$0"
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --content) CONTENT=${2:-}; shift 2 ;;
        --scene) SCENE=${2:-}; shift 2 ;;
        --arg) SCENE_ARGS+=("${2:-}"); shift 2 ;;
        --seconds) RUN_SECONDS=${2:-30}; shift 2 ;;
        --sync-offset-ms) SYNC_OFFSET_MS=${2:-}; shift 2 ;;
        --health-force) HEALTH_FORCE=1; shift ;;
        --health-window-ms) HEALTH_WINDOW_MS=${2:-}; shift 2 ;;
        --health-progress-ms) HEALTH_PROGRESS_MS=${2:-}; shift 2 ;;
        --health-net-grace-ms) HEALTH_NET_GRACE_MS=${2:-}; shift 2 ;;
        --audio-dump) AUDIO_DUMP=1; shift ;;
        --diag|--sync-debug|--http-debug|--stop-debug|--diag-all)
            WILI_FLAGS+=("$1"); shift ;;
        -h|--help) usage ;;
        *) echo "未知参数: $1" >&2; usage ;;
    esac
done

if [ -z "$DEVICE" ] || [ -z "$PASS" ]; then
    echo "请先设置 DEVICE 和 PASS 环境变量（不要写入仓库）" >&2
    exit 2
fi

RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
RUN_DIR="$PROJECT_DIR/debug-runs/$RUN_ID"
mkdir -p "$RUN_DIR"
SSH="sshpass -p $PASS ssh -o StrictHostKeyChecking=no $DEVICE"
SCP="sshpass -p $PASS scp -o StrictHostKeyChecking=no"

{
    echo "run_id=$RUN_ID"
    echo "content=$CONTENT"
    echo "scene=$SCENE"
    echo "args=${SCENE_ARGS[*]:-}"
    echo "flags=${WILI_FLAGS[*]:-}"
    echo "sync_offset_ms=${SYNC_OFFSET_MS:-0}"
    echo "health_force=$HEALTH_FORCE"
    echo "health_window_ms=${HEALTH_WINDOW_MS:-}"
    echo "health_progress_ms=${HEALTH_PROGRESS_MS:-}"
    echo "health_net_grace_ms=${HEALTH_NET_GRACE_MS:-}"
    echo "audio_dump=$AUDIO_DUMP"
    echo "created_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host_time=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "device_time=$($SSH date 2>/dev/null || echo unavailable)"
    echo "wiliwili_md5=$(md5sum "$HOME/build-wiliwili/wiliwili" 2>/dev/null | awk '{print $1}' || true)"
    echo "daemon_md5=$(md5sum "$HOME/build-slplayer/slplayer_daemon" 2>/dev/null | awk '{print $1}' || true)"
} > "$RUN_DIR/manifest.txt"

# 启动前必须保住旧日志：文件不存在是正常情况；文件存在但拉取失败则中止。
if $SSH "test -f /tmp/wiliwili.log" 2>/dev/null; then
    if ! $SCP "$DEVICE:/tmp/wiliwili.log" "$RUN_DIR/prestart.log" 2>/dev/null; then
        echo "拉取旧 /tmp/wiliwili.log 失败，已中止（设备日志未清理）" >&2
        exit 1
    fi
else
    : > "$RUN_DIR/prestart.log"
fi

$SSH dmesg > "$RUN_DIR/dmesg.before.txt" 2>/dev/null || true

scene_args_joined=""
for a in "${SCENE_ARGS[@]:-}"; do
    [ -n "$scene_args_joined" ] && scene_args_joined="$scene_args_joined;"
    scene_args_joined="$scene_args_joined$a"
done
wili_flags_joined=""
for f in "${WILI_FLAGS[@]:-}"; do
    wili_flags_joined="$wili_flags_joined $f"
done
offset_export=""
if [ -n "$SYNC_OFFSET_MS" ]; then
    offset_export=" SL_SYNC_AUDIO_OFFSET_MS=\"$SYNC_OFFSET_MS\""
fi
health_export=""
if [ "$HEALTH_FORCE" = "1" ]; then
    health_export=" SL_HEALTH_FORCE=1"
fi
if [ -n "$HEALTH_WINDOW_MS" ]; then
    health_export="$health_export SL_HEALTH_WINDOW_MS=\"$HEALTH_WINDOW_MS\""
fi
if [ -n "$HEALTH_PROGRESS_MS" ]; then
    health_export="$health_export SL_HEALTH_PROGRESS_MS=\"$HEALTH_PROGRESS_MS\""
fi
if [ -n "$HEALTH_NET_GRACE_MS" ]; then
    health_export="$health_export SL_HEALTH_NET_GRACE_MS=\"$HEALTH_NET_GRACE_MS\""
fi
audio_dump_export=""
if [ "$AUDIO_DUMP" = "1" ]; then
    audio_dump_export=" SL_AUDIO_DUMP_PCM=/tmp/wiliwili-audio.pcm"
fi

cat > "$RUN_DIR/launch_cmdline.txt" <<EOF
export http_proxy=; export https_proxy=; cd "/home/apps/wiliwili"; setup_app_home.sh "/home/apps/wiliwili/.home"; HOME="/home/apps/wiliwili/.home" SL_DEBUG_RUN_ID="$RUN_ID" SL_DEBUG_CONTENT="$CONTENT" SL_DEBUG_SCENE="$SCENE" SL_DEBUG_ARGS="$scene_args_joined"${offset_export}${health_export}${audio_dump_export} /home/apps/wiliwili/wiliwili.sh$wili_flags_joined
EOF
$SCP "$RUN_DIR/launch_cmdline.txt" "$DEVICE:/tmp/launch_cmdline.txt" >/dev/null 2>&1 || {
    echo "写入 launch_cmdline.txt 失败" >&2
    exit 1
}

collect() {
    [ "$STARTED" -eq 1 ] || return 0
    [ "$COLLECTED" -eq 1 ] && return 0
    COLLECTED=1
    $SSH "killall wiliwili >/dev/null 2>&1 || true" >/dev/null 2>&1 || true
    sleep 1
    $SCP "$DEVICE:/tmp/wiliwili.log" "$RUN_DIR/wiliwili.log" 2>/dev/null || true
    if [ "$AUDIO_DUMP" = "1" ]; then
        $SCP "$DEVICE:/tmp/wiliwili-audio.pcm" "$RUN_DIR/audio.pcm" 2>/dev/null || true
    fi
    $SSH dmesg > "$RUN_DIR/dmesg.after.txt" 2>/dev/null || true
    echo "collected_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$RUN_DIR/manifest.txt"
    ARCHIVE_SIZE=$(du -sb "$RUN_DIR" 2>/dev/null | awk '{print $1}' || echo 0)
    echo "archive_size_bytes=${ARCHIVE_SIZE:-0}" >> "$RUN_DIR/manifest.txt"
}
trap collect EXIT
trap 'collect; exit 0' INT TERM

STARTED=1

debug_started=0
for attempt in 1 2 3; do
    # 每轮都重新写入 launch_cmdline 并结束旧播放进程，避免 shell.sh 的
    # 重启时序或残留 launch_cmdline 让调试进程没有真正启动。
    $SCP "$RUN_DIR/launch_cmdline.txt" "$DEVICE:/tmp/launch_cmdline.txt" >/dev/null 2>&1 || continue
    $SSH "killall wiliwili >/dev/null 2>&1 || true; sleep 1; killall shell >/dev/null 2>&1 || true" >/dev/null 2>&1 || true
    for _tick in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        if $SSH "grep -F 'run=$RUN_ID' /tmp/wiliwili.log >/dev/null 2>&1" 2>/dev/null; then
            debug_started=1
            break 2
        fi
        sleep 1
    done
done
if ! $SSH "grep -F 'run=$RUN_ID' /tmp/wiliwili.log >/dev/null 2>&1" 2>/dev/null; then
    echo "设备日志未出现 run=$RUN_ID，启动可能失败；查看 $RUN_DIR" >&2
    exit 1
fi

echo "run=$RUN_ID"
echo "artifacts=$RUN_DIR"

start_snapshot=0
ok_snapshot=0
deadline=$(( $(date +%s) + RUN_SECONDS ))
cmd_prefix="[CMD][run=$RUN_ID]"
while [ "$(date +%s)" -lt "$deadline" ]; do
    if [ "$start_snapshot" -eq 0 ] && \
       $SSH "grep -F '$cmd_prefix' /tmp/wiliwili.log 2>/dev/null | grep -q 'status=start'" 2>/dev/null; then
        $SSH dmesg > "$RUN_DIR/dmesg.$SCENE.start.txt" 2>/dev/null || true
        start_snapshot=1
    fi
    if [ "$ok_snapshot" -eq 0 ] && \
       $SSH "grep -F '$cmd_prefix' /tmp/wiliwili.log 2>/dev/null | grep -q 'status=ok'" 2>/dev/null; then
        $SSH dmesg > "$RUN_DIR/dmesg.$SCENE.ok.txt" 2>/dev/null || true
        ok_snapshot=1
    fi
    sleep 1
done
