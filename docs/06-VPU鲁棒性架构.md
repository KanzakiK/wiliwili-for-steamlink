# VPU 鲁棒性架构（防止 VPU 卡死进入脏状态）

> 本文档只写 VPU 鲁棒性的架构、决策和机制契约。
> 任务状态在 [00-当前状态与任务.md](00-当前状态与任务.md)，复核过程、真机数据和历史记录在 [02-开发进度日志.md](02-开发进度日志.md)。

## 一、定位

本架构是播放器“事前防御 + 事后兜底”的鲁棒性设计：

- **事前防御**：SlPlayer 对 SLVideo 的起调、重建和退出保持干净，降低把 VPU 带入脏状态的概率。
- **事后兜底**：即使播放子进程卡死，也由进程隔离、Dirty Flag、watchdog 与断连恢复把影响限制在播放子进程，不牵连 wiliwili 主进程和 UI。

核心关系：**VPU 生命周期干净化是主体，进程隔离 + Dirty Flag 是兜底，watchdog 是检测与恢复触发层。**

## 二、平台约束

- VPU/MV_PE 状态可跨进程、跨播放残留；重启单个播放器不一定能清掉硬件状态，最终手段是 `reboot -f`。
- SDK 没有全局 Deinit/Reinit；`SLVideo.h` 只暴露 Context/Stream 的 Create/Free 与帧提交接口。最接近复位的是 `FreeContext → 延时 → CreateContext`。
- `std::thread` 无法从外部 kill；卡在内核 ioctl 时 `join()` 会阻塞，因此单进程内无法可靠重启播放线程。
- 硬解视频平面是硬件层状态，不随进程退出复位；正常退出通过 FreeStream/FreeContext 停止，异常强杀可能留下最后一帧。
- 设备没有 `/dev/fb*`，SDK 也没有关闭视频平面/清屏接口；异常残帧只能靠正常退出、watchdog 减少 UI 重启，或未来评估黑帧覆盖。

## 三、已拍板决策

| 项 | 决策 | 理由 |
|---|---|---|
| 总体形态 | 进程隔离：SlPlayer 运行在独立 `slplayer_daemon`，wiliwili 通过 IPC 控制 | 子进程卡死可 SIGKILL + 重启，主进程/UI 不受影响 |
| IPC 范围 | 只走控制面与事件面；视频帧/PCM 仍走 SLVideo/SLAudio 硬件层 | 数据不过 IPC，性能损失可忽略 |
| 脏退出检测 | Dirty Flag 锁文件 `/tmp/slplayer_vpu.lock`，不做开机 1 帧 H.264 探针 | 探针本身要 Create/Destroy stream，可能成为新污染源 |
| watchdog 位置 | 独立线程放在 libmpv 兼容层（主进程侧），不放 SlPlayer | 监控者必须独立于被监控者；卡死的主循环无法自我检测 |
| D22 软卡顿处理 | 只发一次播放页提示，不自动降清晰度、不重开播放 | 不同清晰度/片源压力不同，自动降级会误伤并打断当前 session；用户自行选择更稳妥 |
| watchdog 形态 | 只读无锁 atomic 快照，不取播放线程锁、不调 stats()、不碰 VPU | 避免被播放线程阻塞；检测线程不能成为新的死锁点 |
| 异常处理职责 | watchdog 只判定并触发上层恢复，不在 SlPlayer 内 `_exit()` | SlPlayer 是库，进程生命周期由上层管理 |
| 父进程死亡清理 | `PR_SET_PDEATHSIG` + `getppid()` 复核 + IPC EOF 双保险 | PDEATHSIG 是父线程语义且 handler 中不能调 VPU/锁，必须用 EOF 兜底 |
| 信号处理 | handler 只置 `sig_atomic_t` 标志，主循环复核后走正常 shutdown | 信号处理器中调用 mutex、malloc、fopen、VPU API 都不安全 |
| 恢复位置 | watchdog 恢复使用 daemon 上报的当前呈现时间，不使用 UI 目标时间 | 目标位置可能尚未达到，不能用于恢复 |
| 重建策略 | 减少重复 stream 重建；连续重建至少间隔 400ms | 高 refs 片源短时间多次重建会耗尽 VPU/SHM 连续块 |

## 四、分层与职责

```text
wiliwili 主进程
  wiliwili UI / MPVCore
        │ mpv 命令 / 事件（原 mpv 语义）
  libmpv 兼容层（IPC 客户端）
        │ 控制面命令 + 事件
  watchdog 独立线程 ──读──> heartbeat 快照
        │ 判定卡死 → 上层 SIGKILL / 重启 daemon
        └───────────────┘
                  │ Unix socket
播放子进程 slplayer_daemon
  IPC 控制循环 + SlPlayer
   起调：CreateContext → CreateStream
   退出：停音频 → FreeStream → FreeContext → 清 Dirty Flag
   重建：seek/pause 时按需 Free/Create，关键帧重喂
   数据面：SLVideo/SLAudio 硬件层
```

Dirty Flag 语义：

- 创建 VPU context 前写 `/tmp/slplayer_vpu.lock`。
- 完整 `FreeStream → FreeContext` 之后删除。
- 异常退出来不及删除 → 下次启动先做 `CreateContext → FreeContext → 延时` 清洗，再正式起调。
- `/tmp` 在设备重启时清空，重启也是天然兜底。

## 五、机制契约

### 1. VPU 生命周期干净化

- 起调前写 Dirty Flag；异常残留时先清洗。
- `CreateContext` / `CreateStream` 失败时有限次复位重试：无旧 context 时只延时重试；有旧 context 时先 FreeContext 再重建。
- shutdown 顺序：停音频 → `SLAudio_FreeContext` → `SLVideo_FreeStream` → 释放 AVPacket/AVBSF/AVIO/source → `SLVideo_FreeContext` → 清 Dirty Flag。
- shutdown 必须幂等；回调异常不得绕过资源释放，使用 RAII guard 与外层 try/catch 兜底。
- seek/pause 重建前给硬件清理间隔；连续重建之间另设最小间隔，避免 SHM 碎片化。

### 2. 无锁状态快照

- `heartbeat()` 只读 atomic 字段，不调用 `stats()`，不取锁，不查数据源。
- 快照字段：最后成功提交帧时间、内部 stage、videoFrames、playing、inTransition、currentPaused、targetPaused、seeking、workerAlive。
- `lastFrameTimeMs` 在 `SubmitFrame` 成功后更新；播放线程退出时存活状态必须变为不可用。
- heartbeat 经 IPC 命令/事件上报，兼容层写入 watchdog 可见快照。

### 3. watchdog 与恢复

- 判定条件：无心跳超时，或无新帧且 playing && !inTransition && !pause && 已过恢复宽限期，或 workerAlive=false。
- 命中后 SIGKILL 当前 daemon generation；上层记录真实退出状态（含 signal/SIGKILL）。
- 恢复由兼容层接收线程独占：回收旧 daemon → 清理 socket → 限流检查 → 启动新 daemon → 用最后呈现时间重写 open → 抑制 FILE_LOADED 避免覆盖暂停态 → 首帧后恢复目标暂停态。
- 30 秒内最多自动重启 3 次，避免 fork 风暴。
- 新 daemon 从启动到首帧期间挂起 watchdog，避免慢开流误判。

### 4. D22 软卡顿检测与提示

D22 与 watchdog 分层：

- watchdog 只判定硬故障：无心跳、worker 退出、长时间无新帧，继续 SIGKILL + 重启 daemon。
- D22 只判定软卡顿：仍在持续喂帧，但呈现位置长期不推进或推进断续；命中后发一次播放页提示，不触发进程重启。
- 两者复用 compat 的 500ms heartbeat 轮询，但判据和动作分开；不新增高频线程，`frameAge` 到达硬 watchdog 阈值时不发 D22 提示，避免重复抢判。

heartbeat 扩展字段：

- `presentationMs`：直接用播放线程已有的无锁 `position_`，不调用 `stats()`。
- `videoQueueMs`：用无锁 `videoQueueMs_`，辅助识别队列增长。
- `networkErrorAgeMs`：HTTP 重试/下载失败时刷新原子时间戳，用于近期网络错误排除。

软卡顿判据（阈值先用 env 校准）：

```text
playing=true
&& 未暂停、未 seek、未处于 inTransition
&& 已过首帧与 resume grace
&& frameAge 未到 watchdog 硬故障阈值
&& 最近 3s 没有网络错误
&& 连续 8s 内 presentationMs 高水位推进 < 250ms
&& 期间 videoFrames 仍在增长
```

动作边界：

- compat 发出一次性健康事件，映射到播放页 `VideoView::HINT`。
- 提示文案固定为“播放不流畅，可尝试切换到 720P”。
- 每个 generation/session 只提示一次；新文件、用户切清晰度或 watchdog 重启后重置。
- 不调用 `updateVideoLink()`，不改 `SettingItem::VIDEO_QUALITY`，不自动重开当前播放。
- 如果当前已是 720P 或没有低档，仍只提示一次，不做重复动作。

### 5. 父进程死亡清理

- fork 前记录父 PID，子进程设 `PR_SET_PDEATHSIG` 并通过 `--expected-parent` 接收 PID。
- 收到 SIGTERM 后只置标志；主循环与 accept 等待循环复核 `getppid()`。
- IPC EOF 是第二道保险：控制连接断开即进入退出流程。
- 只有确认父进程已失联或控制通道断开，才执行完整 shutdown；避免多线程 PDEATHSIG 误触发。

### 6. VPU 重建与 SHM

- 新建流后尚未提交帧时的 start seek、暂停已 flush 后的恢复 seek，不重复 Free/Create。
- 用户主动 seek 保持重建语义，因为 SLVideo 没有 flush 接口。
- 连续重建至少 400ms；50ms 的 Free→Create 清理间隔仍用于单次重建内部。
- SHM 连续块不足时不要盲目降低清晰度或改 SPS；先记录 qn/codec/refs、stage、时间戳与分配失败现场。

## 六、验证不变量

- 任何操作循环后，已知正常流仍能播放；否则视为 VPU 污染。
- 正常退出必须清 Dirty Flag；SIGKILL 后必须残留并由下次启动清洗。
- 父进程被杀后，daemon 应在 1 秒内自行退出并清 Dirty Flag，不留孤儿。
- watchdog 不应把暂停、seek 重建、恢复首帧前窗口误判为卡死。
- 视觉与音画结论必须有采集卡截图或用户确认，不能只凭日志。

具体测试矩阵、数据、真机结论与历史复核见 [02-开发进度日志.md](02-开发进度日志.md)；执行纪律见 [07-开发纪律.md](07-开发纪律.md)。
