#!/bin/sh
#
# Steam Link 主菜单启动入口（阶段 3.5a）
#
# 规范与 /home/apps/moonlight 一致：
#   shell 扫描 /home/apps/<dir>/toc.txt 生成主菜单图标；
#   点图标后 shell 退出，/home/steam/shell.sh 读出 /tmp/launch_cmdline.txt
#   并 eval 执行本脚本（见 shell.sh 的 while 循环）。
#
# 安装位置：/home/apps/wiliwili/
#   toc.txt  wiliwili.png  wiliwili.sh  bin/{wiliwili,slplayer_daemon}  resources/

# HOME 持久化：默认 HOME 不保留（/tmp 重启清空、/home/steam 也不适合放第三方配置），
# 放 onboard flash 的 /usr/local 下，否则 B 站登录态每次重启都要重新扫码。
export HOME=/usr/local/wiliwili
mkdir -p "$HOME"

# libmpv 兼容层 fork/exec 的播放子进程路径
export SL_DAEMON_PATH=/home/apps/wiliwili/bin/slplayer_daemon

# 诊断开关：默认全部关闭；通过启动参数或同名环境变量按次开启。
# 诊断代码保留在源码里，不因为一次定位完成就删除。
#   --diag          轻量播放诊断（stage / 位置 / VPU 队列 / 下载窗口）
#   --sync-debug    音视频对齐日志
#   --http-debug    SlHttp 读取与窗口细节
#   --stop-debug    退出/清理阶段日志
#   --diag-all      以上全部
# 也可在 /tmp/launch_cmdline.txt 用 `SL_PLAY_DIAG=1 /home/apps/wiliwili/wiliwili.sh`。
_wili_pass=""
for _wili_arg do
    case "$_wili_arg" in
        --diag) export SL_PLAY_DIAG=1 ;;
        --sync-debug) export SL_SYNC_DEBUG=1 ;;
        --http-debug) export SL_HTTP_DEBUG=1 ;;
        --stop-debug) export SL_STOP_DEBUG=1 ;;
        --diag-all)
            export SL_PLAY_DIAG=1 SL_SYNC_DEBUG=1 SL_HTTP_DEBUG=1 SL_STOP_DEBUG=1
            ;;
        *)
            if [ -z "$_wili_pass" ]; then
                _wili_pass=$_wili_arg
            else
                _wili_pass="$_wili_pass
$_wili_arg"
            fi
            ;;
    esac
done
_wili_old_ifs=$IFS
IFS='
'
set -f
set -- $_wili_pass
set +f
IFS=$_wili_old_ifs

# 注意：不要在这里设 SL_TRANSPARENT。
# 那是阶段 3.3 验证"透明层能否透出视频"用的 spike 开关（borealis 补丁里
# `if (getenv("SL_TRANSPARENT")) color.a = 0;`），一旦设置会让整个 UI 图层
# 全透明 —— 首页/详情页没有视频层可透，背景就变成黑的。
# 透明只在播放页需要，由兼容层在 loadfile/stop 时运行时切换（见 05 文档第五节）。

# 提升音视频服务优先级，避免单核被 UI 抢占（照 moonlight 的做法）
renice -10 -p "$(pidof PE_Single_CPU)" 2>/dev/null

cd /home/apps/wiliwili || exit 1

# 内存基线 + /tmp 体检
#
# 这台设备只有 269MB 内存且无 swap，**OOM killer 是首要故障源**：
# 它会把 wiliwili 或 slplayer_daemon 直接 SIGKILL，表现为"播放十几秒突然
# 退出""暂停后按钮不变""IPC 接收断开"，很容易被误判成播放器 bug。
#
# 特别注意 **/tmp 是 tmpfs（内存盘）**：往里放多少东西，就等于占掉多少内存。
# 2026-11-17 就因为 /tmp 堆了 88MB 测试产物（mp4 样本 + 旧二进制副本 + 旧
# resources），可用内存只剩 3.4MB，播放中 gal3d 分配显存时触发 OOM，wiliwili
# 被杀。测试产物请放 /home/apps/wiliwili/ 或用完即删。
#
# 每次启动先记一行基线，事后复盘不必靠猜。
: > /tmp/wiliwili.log
{
    echo "[boot] run=${SL_DEBUG_RUN_ID:-main} $(date) MemFree=$(awk '/MemFree/{print $2}' /proc/meminfo)kB Shmem=$(awk '/Shmem/{print $2}' /proc/meminfo)kB"
    df -h /tmp | tail -1 | sed 's/^/[boot] tmpfs /'
} >> /tmp/wiliwili.log

# 用 exec 省掉一个常驻 shell 进程（与 moonlight 一致）。
# 安全性：`eval $cmd` 执行的是 shell.sh fork 出的**子进程**，exec 替换的是
# 该子进程，不影响 shell.sh 的 while 循环；wiliwili 退出后循环继续，
# 自动重新拉起 shell（主菜单）。
# 注意用 >> 而非 >，否则会把上面记的基线冲掉。
if nice -n -10 true 2>/dev/null; then
    exec nice -n -10 ./bin/wiliwili "$@" >> /tmp/wiliwili.log 2>&1
else
    exec ./bin/wiliwili "$@" >> /tmp/wiliwili.log 2>&1
fi
