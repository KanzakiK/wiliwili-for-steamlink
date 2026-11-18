# wiliwili for Steam Link

这是一个把 [wiliwili](https://github.com/xfangfang/wiliwili) 的 UI/API 移植到 Steam Link 的实验性分支。它复用 wiliwili 的界面与 B 站业务层，但把播放内核替换为 Steam Link 原生 SLVideo/SLAudio，并在 wiliwili 与播放器之间加入 libmpv 兼容层和独立 IPC daemon。

本分支不是 wiliwili 上游的正式发行版，也不引入 mpv 本体；它只在本仓库内通过构建期补丁替换播放相关胶水，不重写 UI、不修改 wiliwili 布局或样式。

## 当前状态

P0 播放语义对齐已经完成并经真机多轮验证：暂停/恢复、快进快退、续播、音轨切换、弹幕与长时间播放均可使用。已知未解决问题是高清源切换时的 UI/GPU 51.84MB SHM 分配失败（任务 D14）；在降低图片请求分辨率后，整体内存稳定性明显改善。

已确认的能力边界：

- 视频最高 1080P60（`qn<=116`）；FLV/MP4 支线的 720P 内部编号 `qn=192` 作为兼容档保留。
- 不提供 4K、8K、HDR、HEVC、AV1 选项；编码锁定 H.264。
- 音频仅提供 AAC High / Medium / Low 三档。
- 对 SLVideo 硬解无效的播放器图像调节和倍速入口已隐藏。
- 视频由 SLVideo 硬件层全屏输出，GUI/弹幕通过透明 GLES2 层叠加。

## 架构

```text
wiliwili UI / MPVCore
    目标状态、事件、属性
           |
libmpv 兼容层（IPC 客户端）
    mpv 语义翻译、事件队列、文件代次、watchdog
           |
slplayer_daemon（独立进程）
    SlPlayer + HttpSource + FFmpeg 解封装
           |
SLVideo / SLAudio
    硬件视频层 / 48kHz 音频输出
```

详细设计见 [docs/05-播放器替换设计.md](docs/05-播放器替换设计.md) 和 [docs/06-VPU鲁棒性架构.md](docs/06-VPU鲁棒性架构.md)。

## 构建与部署

构建环境为 Windows + WSL2，使用 Steam Link SDK 交叉编译。需要先准备：

- `~/steamlink-sdk`：Steam Link SDK / toolchain / rootfs。
- `~/refs/wiliwili`：wiliwili 上游源码（当前适配版本 `v1.6.0` / `88e5876`）。
- `~/steamlink-curl`：交叉编译的 libcurl/cpr 依赖。
- `sshpass`，以及可通过 SSH 访问的 Steam Link 设备。

构建播放器与 UI：

```sh
cmake -S player -B ~/build-slplayer
cmake --build ~/build-slplayer -j4

sh tools/build-wiliwili.sh
```

部署到设备：

```sh
DEVICE=root@<device-address> PASS=<device-password> sh tools/steamlink-app/install.sh
```

`DEVICE` 和 `PASS` 必须通过环境变量提供，仓库里不保存设备凭据。安装脚本使用临时文件加原子替换，避免覆盖正在执行的二进制。

更多命令、日志位置和排查步骤见 [docs/04-构建与真机调试.md](docs/04-构建与真机调试.md)。

## 仓库结构

- `player/`：SlPlayer、IPC daemon、音频管线和 HTTP 源。
- `tools/mpv-stub/`：libmpv 兼容层。
- `tools/patches/`：对 wiliwili/borealis 的构建期补丁。
- `tools/steamlink-app/`：安装、启动和真机测试脚本。
- `docs/`：设计、任务看板、开发日志与排查经验。

---

# 上游 wiliwili 信息

以下内容来自 wiliwili 上游 README，用于保留上游项目的产品介绍、安装方式、文档入口和致谢信息。本仓库的 Steam Link 改动不代表上游项目立场；上游问题请提交到 [xfangfang/wiliwili](https://github.com/xfangfang/wiliwili)。

wiliwili 是一个专为手柄用户设计的第三方 B 站客户端，提供接近官方 PC 客户端的浏览体验，支持触屏、鼠标、键盘与手柄操作。

## 上游特点

- 多语言：简体、繁体、日语、韩语、英语等。
- 搜索页：热搜、视频、番剧、影视。
- 筛选页：快速找到影视内容。
- 动态页：关注 UP 主的最近视频动态。
- 直播页：关注主播与系统推荐。
- 播放页：视频、番剧、电影、纪录片、综艺，支持弹幕与评论。
- 个人页：扫码登录、历史记录、收藏、追番、追剧。
- 主题：深浅两色主题，可跟随系统自动切换。

## 上游安装与平台

- Nintendo Switch：从 [wiliwili releases](https://github.com/xfangfang/wiliwili/releases) 下载 `wiliwili-NintendoSwitch.zip`，将 `wiliwili.nro` 放入内存卡 `switch` 目录。默认 OpenGL 版最高 4K@30，deko3d 版可支持 4K@60。
- PS Vita：下载 `wiliwili-PSVita.vpk` 安装；开启硬解后可流畅播放 720P 横屏、480P 竖屏视频。
- PS4：下载 `wiliwili-PS4.pkg` 安装；只支持软解，4K@60 需要开启低画质解码。
- PC：支持 Windows 7+、macOS 10.11+、Linux；提供硬件解码切换、秒开和鼠标操作。

更多使用技巧见 [wiliwili Wiki](https://github.com/xfangfang/wiliwili/wiki)。

## 上游开发

上游主分支 `yoga` 为最新版本代码，开发分支 `dev` 用于新 PR。PC 本地开发示例：

```sh
git clone --recursive https://github.com/xfangfang/wiliwili.git
cd wiliwili
cmake -B build -DPLATFORM_DESKTOP=ON
cmake --build build -j$(nproc)
```

上游贡献指南、翻译协作与平台移植说明同样见 [wiliwili Wiki](https://github.com/xfangfang/wiliwili/wiki) 和 [上游 README](https://github.com/xfangfang/wiliwili/blob/yoga/README.md)。

## 上游致谢

wiliwili 的开发依赖多个开源项目和工具链，包括 devkitPro、switchbrew、vitaSDK、OpenOrbis、PacBrew、borealis、FFmpeg、mpv、cpr、nlohmann/json、OpenCC、pystring、lunasvg、mongoose、libwebp 等。完整致谢列表见[上游 README](https://github.com/xfangfang/wiliwili/blob/yoga/README.md)。
