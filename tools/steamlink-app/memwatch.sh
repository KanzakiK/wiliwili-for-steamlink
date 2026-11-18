#!/bin/sh
#
# 设备内存观察（2026-11-17 加入）
#
# 背景：Steam Link 只有 269MB 内存、无 swap，OOM killer 会直接 SIGKILL
# wiliwili 或 slplayer_daemon，表现成"播放中突然退出""暂停后按钮不变"
# "IPC 接收断开"，极易误判成播放器 bug。要区分"确实泄漏"和"一次性峰值"，
# 必须在出问题时手上有时间序列。
#
# 用法（设备端）：
#   sh /tmp/memwatch.sh &            # 启动，每 2 秒记一行到 /tmp/memwatch.log
#   killall memwatch.sh              # 停止
#   cat /tmp/memwatch.log            # 查看
#
# 判读：
#   wili 持续上升     -> UI 侧泄漏（封面纹理、进度条快照、弹幕对象）
#   daemon 持续上升   -> 播放侧缓冲（音频预缓冲、帧队列）
#   Shmem 上涨        -> /tmp（tmpfs）被写入，等于直接吃掉可用内存
#   MemFree 骤降到 <10MB -> 马上要 OOM

OUT=/tmp/memwatch.log
: > "$OUT"

while true; do
    TS=$(date +%H:%M:%S)
    MF=$(awk '/MemFree/{print $2}' /proc/meminfo)
    SH=$(awk '/Shmem/{print $2}' /proc/meminfo)
    SL=$(awk '/Slab:/{print $2}' /proc/meminfo)
    CA=$(awk '/^Cached:/{print $2}' /proc/meminfo)

    W=$(pidof wiliwili)
    D=$(pidof slplayer_daemon)
    WS=0
    DS=0
    [ -n "$W" ] && WS=$(awk '/VmRSS/{print $2}' /proc/$W/status 2>/dev/null)
    [ -n "$D" ] && DS=$(awk '/VmRSS/{print $2}' /proc/$D/status 2>/dev/null)

    echo "$TS MemFree=${MF:-?} Cached=${CA:-?} Shmem=${SH:-?} Slab=${SL:-?} wili=${WS:-0} daemon=${DS:-0}" >> "$OUT"
    sleep 2
done
