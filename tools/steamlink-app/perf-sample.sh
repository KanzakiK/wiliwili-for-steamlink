#!/bin/sh
#
# 真机性能采样：CPU ticks / RSS / 内存账本 / 当前片源元信息。
# 默认写到 /home/apps/bilibili/perf-metrics.log，避免占用 tmpfs。

OUT=${OUT:-/home/apps/bilibili/perf-metrics.log}
INTERVAL=${INTERVAL:-2}
HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)

: > "$OUT"
echo "hz=$HZ interval=$INTERVAL"

sample_proc() {
    name=$1
    pid=$(pidof "$name" 2>/dev/null | awk '{print $1}')
    if [ -z "$pid" ] || [ ! -r "/proc/$pid/status" ]; then
        echo "${name}=0,0,0,0"
        return
    fi
    rss=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null)
    vsz=$(awk '/VmSize/{print $2}' "/proc/$pid/status" 2>/dev/null)
    threads=$(awk '/Threads/{print $2}' "/proc/$pid/status" 2>/dev/null)
    ticks=$(awk '{print $14 + $15}' "/proc/$pid/stat" 2>/dev/null)
    echo "${name}=${pid},${rss:-0},${vsz:-0},${ticks:-0},${threads:-0}"
}

while true; do
    TS=$(date '+%F %T')
    MF=$(awk '/MemFree/{print $2}' /proc/meminfo)
    CA=$(awk '/^Cached:/{print $2}' /proc/meminfo)
    AN=$(awk '/AnonPages/{print $2}' /proc/meminfo)
    SH=$(awk '/Shmem/{print $2}' /proc/meminfo)
    SL=$(awk '/Slab:/{print $2}' /proc/meminfo)
    MEDIA=$(grep '\[SlPlayer\]\[media\]' /tmp/wiliwili.log 2>/dev/null | tail -1 | sed 's/.*\[SlPlayer\]\[media\] //')

    echo "$TS MemFree=${MF:-0} Cached=${CA:-0} AnonPages=${AN:-0} Shmem=${SH:-0} Slab=${SL:-0} $(sample_proc wiliwili) $(sample_proc slplayer_daemon) $(sample_proc PE_Single_CPU) media=${MEDIA:-none}" >> "$OUT"
    sleep "$INTERVAL"
done
