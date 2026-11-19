# 已封存的旧测试程序

这里保存 `compat_test.c` 和 `test-3.5c.sh`，只作为历史 IPC/watchdog 生命周期故障注入记录备查，**不要再运行，也不要作为回归验收依据**。

原因：用户实测旧用例在脏 VPU 环境下出现“有声音无画面”，干净环境重启后又出现“有画面无声音”，不能可靠反映音视频输出和画面呈现。新回归必须基于真实 wiliwili 播放页，使用 `--diag --sync-debug` 抓日志，并结合用户真机听感和截图判断。

`compat_test` 构建目标已改为显式开启：`cmake -DBUILD_DEPRECATED_COMPAT_TEST=ON ...`。
