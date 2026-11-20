#!/bin/sh
#
# 安装/更新 Steam Link 主菜单的 wiliwili 启动入口（阶段 3.5a）
#
# 机制（与 /home/apps/moonlight 一致）：
#   shell 扫描 /home/apps/<dir>/toc.txt 生成主菜单图标；
#   点图标后 shell 退出，/home/steam/shell.sh 读出 /tmp/launch_cmdline.txt
#   并 eval 执行 toc.txt 里的 run= 脚本。
#
# 本脚本只做增量安装，不动 shell.sh / rc.local / moonlight：
#   /home/apps/wiliwili/
#     toc.txt  wiliwili.png  wiliwili.sh
#     bin/{wiliwili,slplayer_daemon}
#     resources/
#   /usr/local/wiliwili/    <- HOME（持久化，保存 B 站登录态）
#
# 用法（在 WSL 里）：
#   DEVICE=root@<设备地址> PASS=<设备密码> sh tools/steamlink-app/install.sh
#
# 卸载：
#   ssh root@<设备> "rm -rf /home/apps/wiliwili /usr/local/wiliwili"
set -e

DEVICE=${DEVICE:-}
PASS=${PASS:-}
if [ -z "$DEVICE" ] || [ -z "$PASS" ]; then
    echo "请先设置 DEVICE 和 PASS 环境变量（不要写入仓库）" >&2
    exit 1
fi
APP_NAME=${APP_NAME:-wiliwili}
APP_DIR=/home/apps/$APP_NAME
APP_HOME=/usr/local/$APP_NAME
VERSION=${VERSION:-$(git -C "$(dirname "$0")/../.." describe --tags --always --dirty 2>/dev/null || echo dev)}
COMMIT=$(git -C "$(dirname "$0")/../.." rev-parse HEAD 2>/dev/null || echo unknown)
WILI_BUILD=${WILI_BUILD:-$HOME/build-wiliwili}
PLAYER_BUILD=${PLAYER_BUILD:-$HOME/build-slplayer}
HERE=$(cd "$(dirname "$0")" && pwd)

SSH="sshpass -p $PASS ssh -o StrictHostKeyChecking=no $DEVICE"
SCP="sshpass -p $PASS scp -o StrictHostKeyChecking=no"

echo "== 1/6 校验本地产物 =="
for f in "$WILI_BUILD/wiliwili" "$PLAYER_BUILD/slplayer_daemon"; do
    [ -f "$f" ] || { echo "缺少产物: $f"; exit 1; }
done
[ -d "$WILI_BUILD/resources" ] || { echo "缺少资源目录: $WILI_BUILD/resources"; exit 1; }
echo "ok"

echo "== 2/6 创建目录 =="
$SSH "mkdir -p $APP_DIR/bin $APP_HOME"

# 安装包和真机安装目录保持同一版本信息，避免旧 VERSION.txt 残留。
VERSION_FILE=$(mktemp)
trap 'rm -f "$VERSION_FILE"' EXIT
printf 'version=%s\ncommit=%s\nbuilt=%s\n' "$VERSION" "$COMMIT" "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" > "$VERSION_FILE"
$SCP "$VERSION_FILE" "$DEVICE:$APP_DIR/VERSION.txt"

echo "== 3/6 部署小文件（toc/脚本/图标）=="
$SCP "$HERE/toc.txt" "$HERE/wiliwili.sh" "$HERE/wiliwili.png" "$DEVICE:$APP_DIR/"
$SSH "chmod 755 $APP_DIR/wiliwili.sh $APP_DIR/wiliwili.png"

echo "== 4/6 部署二进制 =="
# 先传临时名再原子替换：正在运行的二进制可以直接被 rename 覆盖，
# 不需要 kill 正在播放的 wiliwili（强杀会造成页面闪回主菜单并留下 VPU 脏状态）。
$SCP "$WILI_BUILD/wiliwili" "$DEVICE:$APP_DIR/bin/wiliwili.new"
$SCP "$PLAYER_BUILD/slplayer_daemon" "$DEVICE:$APP_DIR/bin/slplayer_daemon.new"
$SSH "chmod 755 $APP_DIR/bin/wiliwili.new $APP_DIR/bin/slplayer_daemon.new && \
      mv -f $APP_DIR/bin/wiliwili.new $APP_DIR/bin/wiliwili && \
      mv -f $APP_DIR/bin/slplayer_daemon.new $APP_DIR/bin/slplayer_daemon"

echo "== 5/6 部署资源（tar，避免 scp -r 在设备端报 realpath）=="
tar czf /tmp/wiliwili_res.tar.gz -C "$WILI_BUILD" resources
$SCP /tmp/wiliwili_res.tar.gz "$DEVICE:/tmp/"
$SSH "cd $APP_DIR && tar xzf /tmp/wiliwili_res.tar.gz && rm -f /tmp/wiliwili_res.tar.gz"
rm -f /tmp/wiliwili_res.tar.gz

echo "== 6/6 检查设备内存与 /tmp 占用 =="
# 设备只有 269MB 内存且无 swap，/tmp 是 tmpfs（内存盘）——往 /tmp 放东西
# 等于直接吃掉可用内存。2026-11-17 曾因 /tmp 堆了 88MB 测试产物，可用内存
# 只剩 3.4MB，播放中 gal3d 分配显存触发 OOM killer 杀掉 wiliwili。
# 所以安装完必须看一眼，别在内存盘里留垃圾。
$SSH "free | head -2; echo; echo '/tmp 是 tmpfs（内存盘），占用即吃内存:'; df -k /tmp | tail -1"
TMP_KB=$($SSH "df -k /tmp | tail -1 | tr -s ' ' | cut -d' ' -f3" | tr -d '\r')
if [ -n "$TMP_KB" ] && [ "$TMP_KB" -gt 10240 ] 2>/dev/null; then
    echo ""
    echo "!! 警告：/tmp 已占用 ${TMP_KB}KB，会直接减少可用内存"
    echo "   建议清理测试产物："
    echo "     ssh root@<设备> \"rm -rf /tmp/slplayer /tmp/wiliwili /tmp/resources /tmp/res.tar.gz\""
fi

echo "== 完成 =="
$SSH "echo 目录结构:; ls $APP_DIR; echo; echo 磁盘:; df -h / | tail -1"
cat <<'EOF'

重启主菜单后即可看到图标（powermanager 管着主菜单，杀 shell 会由 shell.sh 循环重建）：
  ssh root@<设备> "killall shell"

排查启动问题看日志：
  ssh root@<设备> "cat /tmp/wiliwili.log"
EOF
