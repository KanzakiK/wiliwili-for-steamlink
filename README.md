# wiliwili for Steam Link

[English](README_EN.md)

这是一个把 [wiliwili](https://github.com/xfangfang/wiliwili) 的 UI/API 移植到 Steam Link 的实验性分支。它复用 wiliwili 的界面与 B 站业务层，但把播放内核替换为 Steam Link 原生 SLVideo/SLAudio，并在 wiliwili 与播放器之间加入 libmpv 兼容层和独立 IPC daemon。

本分支不是 wiliwili 上游的正式发行版，也不引入 mpv 本体；它只在本仓库内通过构建期补丁替换播放相关胶水，不重写 UI、不修改 wiliwili 布局或样式。

## 能力边界

- 视频最高 1080P60（`qn<=116`）；FLV/MP4 支线的 720P 内部编号 `qn=192` 作为兼容档保留。
- 不提供 4K、8K、HDR、HEVC、AV1 选项；编码锁定 H.264。
- 音频仅提供 AAC High / Medium / Low 三档。
- 对 SLVideo 硬解无效的播放器图像调节和倍速入口已隐藏。
- 视频由 SLVideo 硬件层全屏输出，GUI/弹幕通过透明 GLES2 层叠加。

## 真机性能

Steam Link 真机（ARMv7 单核、约 263MB 可用内存）在 `tools/steamlink-app/perf-sample.sh` 下的典型稳定播放数据。CPU 是 `/proc/<pid>/stat` ticks 差值换算的单核百分比中位数；RSS 是进程常驻内存中位数；片源帧率来自 SlPlayer 元信息日志，不代表实际上屏帧率。

| 清晰度 | 片源 | 系统 MemFree | wiliwili RSS / CPU | daemon RSS / CPU | PE_Single_CPU RSS / CPU |
|---|---|---:|---:|---:|---:|
| 720P30 | 1280x720@30，qn=30064 | 66MB | 55MB / 26% | 18MB / 13% | 19MB / 24% |
| 1080P30 | 1920x1080@30，qn=30080 | 66MB | 55MB / 30% | 19MB / 3% | 19MB / 19% |
| 1080P+30 | 1920x1080@30，qn=30112 | 52MB | 55MB / 25% | 31MB / 10% | 19MB / 17% |
| 1080P60 | 1920x1080@60，qn=30116 | 64MB | 55MB / 27% | 21MB / 5% | 19MB / 19% |
| 竖屏 1080P+30 | 1080x1920@30，qn=30112 | 118MB | 55MB / 73% | 19MB / 1% | 15MB / 14% |

RSS 不包含 SLVideo/VDEC 的独立 SHM/GPU 池；MemFree 只反映系统可用内存，不能直接代表 VPU 可分配的连续共享内存块。快速切换视频时 UI 纹理缓存会产生短时峰值，这类切换尖峰未计入上表。

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


## 参考与致谢

本项目实际使用或参考的项目：

- [wiliwili](https://github.com/xfangfang/wiliwili)：UI、API、borealis 界面框架与工程结构的主要来源；本仓库的构建补丁基于上游 `yoga` 分支。
- Steam Link SDK：SLVideo / SLAudio 硬解与音频输出接口，是播放内核替换的基础；SDK 本身不随本仓库分发，只在构建环境中使用。
- Kodi 的 Steam Link 解码器实现：用于核对 SLVideo 的 stream 创建/销毁、参数配置和 VPU 生命周期思路，属于实现参考。

## 许可证

本项目继承 wiliwili 上游的 GPLv3 许可证，详见 [LICENSE](LICENSE)。

---

# 上游 wiliwili README（原文）

<p align="center">
    <img src="resources/svg/cn.xfangfang.wiliwili.svg" alt="logo" height="128" width="128"/>
</p>
<p align="center">
  一个专为手柄用户设计的第三方 <a href="https://www.bilibili.com">B站</a> 客户端
</p>
<p align="center">
<b><a href="#特点">特点</a></b>
|
<b><a href="#安装">安装</a></b>
|
<b><a href="#文档">文档</a></b>
|
<b><a href="#开发">开发</a></b>
</p>

- - -

[![GitHub release (latest by date)](https://img.shields.io/github/v/release/xfangfang/wiliwili)](https://github.com/xfangfang/wiliwili/releases)
![GitHub All Releases](https://img.shields.io/github/downloads/xfangfang/wiliwili/total)
![GitHub stars](https://img.shields.io/github/stars/xfangfang/wiliwili?style=flat)
![GitHub forks](https://img.shields.io/github/forks/xfangfang/wiliwili)
[![Crowdin](https://badges.crowdin.net/wiliwili/localized.svg)](https://crowdin.com/project/wiliwili)
![NS](https://img.shields.io/badge/-Nintendo%20Switch-e4000f?style=flat&logo=Nintendo%20Switch)
![PSV](https://img.shields.io/badge/-PSVita-003791?style=flat&logo=PlayStation)
![PS4](https://img.shields.io/badge/-PS4-003791?style=flat&logo=PlayStation)
![MS](https://img.shields.io/badge/-Windows%207+-357ec7?style=flat&logo=Windows)
![mac](https://img.shields.io/badge/-macOS%2010.11+-black?style=flat&logo=Apple)
![Linux](https://img.shields.io/badge/-Linux-lightgrey?style=flat&logo=Linux&logoColor=white)
[![fedora](https://img.shields.io/badge/fedora-copr-blue?logo=fedora)](https://copr.fedorainfracloud.org/coprs/mochaa/wiliwili/)
[![Scoop Version (extras bucket)](https://img.shields.io/scoop/v/wiliwili?bucket=extras)](https://scoop.sh/#/apps?q=wiliwili)
[![aur](https://img.shields.io/aur/version/wiliwili?color=blue&logo=archlinux)](https://aur.archlinux.org/packages/wiliwili/)
[![Flathub](https://img.shields.io/flathub/v/cn.xfangfang.wiliwili)](https://flathub.org/apps/cn.xfangfang.wiliwili)
[![nightly.link](https://img.shields.io/badge/nightly.link-%E6%B5%8B%E8%AF%95%E7%89%88-green)](https://nightly.link/xfangfang/wiliwili/workflows/build.yaml/dev)
[![layout](https://img.shields.io/badge/wiliwili-自定义布局-yellow)](https://github.com/xfangfang/wiliwili_theme)

<br>

# 特点

wiliwili 拥有非常接近官方PC客户端的B站浏览体验  
同时支持**触屏**、**鼠标**、**键盘** 与 **手柄**操控  
无论是电脑还是游戏掌机都能获得全新的使用体验

多语言：简、繁、日、韩、英 ...   
搜索页：热搜 视频 番剧 影视  
筛选页：快速找到想看的影视内容  
动态页：关注的UP主最近视频动态  
直播页：关注的主播与其他系统推荐  
播放页：视频 番剧 电影 纪录片 综艺，支持弹幕与评论  
个人页：扫码登录 历史记录 个人收藏 我的追番 我的追剧  
主题色：拥有深浅两色主题，跟随系统自动切换

<br>

# 安装

### Nintendo Switch

1. 下载 `wiliwili-NintendoSwitch.zip`：[wiliwili releases](https://github.com/xfangfang/wiliwili/releases)
2. 将 wiliwili.nro 放置在**内存卡** `switch` 目录下。
3. 在主页 `按住` R键打开任意游戏进入 hbmenu，在列表中选择 wiliwili 点击打开即可。
4. [可选] 在应用内安装桌面图标，入口：设置/实用工具/使用教程

<details>

<br>

桌面图标会优先尝试打开 `switch/wiliwili.nro`，如果其不存在，则尝试打开 `switch/wiliwili/wiliwili.nro`，如果这两个路径都不存在，则打开
hbmenu 自行选择路径。

默认提供的为 OpenGL 版本，最高只能播放 4k@30，你也可以下载到支持原生图形 api
的 [deko3d 版本](https://nightly.link/xfangfang/wiliwili/workflows/build.yaml/dev)，可以流畅播放 4k@60，不过可能会偶尔崩溃。

</details>

### PSVita

下载 `wiliwili-PSVita.vpk` 安装即可：[wiliwili releases](https://github.com/xfangfang/wiliwili/releases)

开启硬解后可以流畅播放 720P 横屏视频，480P 竖屏视频，部分直播 1080P 原画。

### PS4

下载 `wiliwili-PS4.pkg` 安装即可：[wiliwili releases](https://github.com/xfangfang/wiliwili/releases)

只支持软解，如果想播放 4k@60 需要在设置中开启低画质解码。

### PC

PC客户端支持切换硬件解码、秒开流畅适合老电脑、支持鼠标操控（左键点击 右键返回 中键刷新）

下载对应系统的安装包运行即可：[wiliwili releases](https://github.com/xfangfang/wiliwili/releases)

> [!TIP]
> 现在 Linux & Steam Deck 用户可以通过系统自带的软件商店（如Discover、GNOME Software）搜索 `wiliwili` 进行下载。  
> 更多使用技巧请参考 [项目 WIKI](https://github.com/xfangfang/wiliwili/wiki)  

<br>

# 文档

在各位开发者的帮助下，wiliwili 支持了一系列包管理器，同时 wiliwili 还拥有丰富的自定义选项，包括：使用 Anime4K
提升观感，自定义字体及图标等等  
前往 [项目 WIKI](https://github.com/xfangfang/wiliwili/wiki) 查看更多使用技巧

<br>

# TODO list

如果你有其他改进的想法或创意，欢迎在讨论区交流：[Discussions](https://github.com/xfangfang/wiliwili/discussions/categories/ideas)

<details>

- [x] 初步完成底层基础组件、首页各类推荐视频、用户视频播放页
- [x] 微调页面、解决播放器启动速度慢、解决播放页面退出卡顿
- [x] 临时解决异步加载导致的空指针问题（图片异步加载某些情况还会出现问题，待修复）
- [x] 添加番剧/影视播放、添加扫码登录、播放历史、用户收藏夹（收藏夹相关部分工作不稳定）
- [x] 初步添加搜索
- [x] 播放页新增分集与UP主最新投稿
- [x] 完善视频播放页用户评论内容
- [x] 重构图片异步加载逻辑
- [x] 解决收藏夹、搜索页某些情况导致闪退的问题
- [x] 完善搜索页：番剧、影视 转为竖图
- [x] 完善播放页投稿列表：调整结构、自动加载下一页
- [x] 播放页展示合集与推荐
- [x] 添加动态页
- [x] 添加视频检索页
- [x] 完善设置页
- [x] 弹幕相关设置
- [x] 点赞、投币、收藏
- [x] 拖拽调节进度
- [x] 增加单手模式使用一个手柄来控制播放器
- [x] NSP forwarder自动检查多个位置的nro文件，避免无法打开
- [x] 增加设置使首页无法通过返回退出，避免误触
- [x] 使用教程添加未指明的快捷键说明
- [x] 重压摇杆临时快进
- [x] 支持切换按键图标
- [x] 应用内多语言切换
- [x] 重构搜索页面
- [x] 评论@显示不同颜色
- [x] 完善评论图片
- [x] 评论大表情包所在行增加行高
- [x] 支持webp图片
- [ ] 搜索支持搜索用户
- [ ] 长按一键三连
- [ ] 支持个人主页
- [ ] 评论跳转进度
- [ ] 评论跳转搜索
- [ ] 评论下方的更多信息 (up主点赞等内容)
- [ ] 投票评论
- [ ] 互动视频

</details>

<br>

# 反馈问题前要做的事

1. 网络相关的问题附加 `网络诊断截图`，入口：应用内设置/实用工具/网络诊断
2. [Switch用户] 要确保 `大气层`和`系统固件` 更新到 **最新** ，`内存卡`为 **FAT32**
3. [Switch用户] 如果打开应用黑屏时间过长，可以尝试删除内存卡目录 `config/wiliwili` 重新进入
4. 确保 `系统时间`正确、系统`网络设置`正确（主要是DNS）、如果使用了`网络代理`请在反馈前关闭并重新测试
5. 查找有没有其他人出现过类似的问题：[Issues](https://github.com/xfangfang/wiliwili/issues?q=is%3Aissue)
6. **完整且详细地** 描述你的问题，最好附加演示视频、截图。
7. 尝试复现问题，尽力找到BUG出现的规律

<br>

# 贡献

### 软件移植

本应用基于 nanovg 绘制界面，nanovg 底层可移植切换到任意图形库，已有 OpenGL/Vulkan/Metal 等支持。   
视频播放部分则使用 FFMPEG + MPV 绘制，默认使用 OpenGL，有 D3D11/Deko3d/Gxm 或软件渲染支持。  
触摸/按键/输入法等平台相关功能通过 GLFW 或 SDL 来支持，也可以脱离二者直接实现，比如 Gxm 版 PSV。

如果你要移植的设备支持 OpenGL(ES) 那么一般来说，直接编译就能正常运行。  
如果你要移植的设备使用其他底层图形库，那么首先需要移植 nanovg，这可以确保应用主要界面正常，
其次为了更好的性能表现需要 ffmpeg 的硬解和 mpv 的渲染支持。  

如果你有想要移植的设备欢迎发一条 issue 讨论，Android / iOS 不在讨论之内。

### 新功能

如果你有想完成的创意，请在开发前发布一个 issue 讨论，避免和别人的创意撞车浪费了时间

### 多语言支持

如果你想为软件添加多语言的翻译支持，或者发现了某些翻译存在问题需要订正，请查看 [#52](https://github.com/xfangfang/wiliwili/issues/52)
了解如何贡献翻译

### 代码分支

主分支 yoga 为最新版本的代码  
开发分支 dev 为正在开发中的代码，任何新的 PR 都需要向 dev 分支提交

<br>

# 开发

```shell
# 拉取代码
git clone --recursive https://github.com/xfangfang/wiliwili.git
cd wiliwili
```

### PC本地运行

目前 wiliwili 支持运行在 Linux macOS 和 Windows上

<details>

#### macOS

```shell
# macOS: install dependencies
brew install mpv webp

cmake -B build -DPLATFORM_DESKTOP=ON
make -C build wiliwili -j$(sysctl -n hw.ncpu)
```

#### Linux

不同 Linux 的编译过程或依赖可能不同，这里是一份总结：[#89](https://github.com/xfangfang/wiliwili/discussions/89)

欢迎在上面的链接中写出你所使用系统的编译过程供大家参考。

```shell
# Ubuntu: install dependencies
sudo apt install libssl-dev libmpv-dev libwebp-dev

cmake -B build -DPLATFORM_DESKTOP=ON
make -C build wiliwili -j$(nproc)
```

```shell
# 如果你想安装在系统路径，并生成一个桌面图标，请使用如下内容编译
cmake -B build -DPLATFORM_DESKTOP=ON -DINSTALL=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX:PATH=/usr
make -C build wiliwili -j$(nproc)
sudo make -C build install

# uninstall (run after install)
sudo xargs -a build/install_manifest.txt rm
```

#### Windows

```shell
# Windows: install dependencies (MSYS2 MinGW64)
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make \
  git mingw-w64-x86_64-mpv mingw-w64-x86_64-libwebp

cmake -B build -G "MinGW Makefiles" -DPLATFORM_DESKTOP=ON
mingw32-make -C build wiliwili -j$(nproc)
```


</details>

### 交叉编译 Switch 可执行文件 (wiliwili.nro)

推荐使用docker构建，本地构建配置环境略微繁琐不过可用来切换底层的ffmpeg或mpv等其他依赖库更灵活地进行调试。

<details>

以下介绍 OpenGL 下的构建方法，deko3d (更好的硬解支持)请参考：`scripts/build_switch_deko3d.sh`

#### Docker

```shell
docker run --rm -v $(pwd):/data devkitpro/devkita64:20251117 \
  bash -c "/data/scripts/build_switch.sh"
```

#### 本地编译

```shell
# 1. 安装devkitpro环境: https://github.com/devkitPro/pacman/releases

# 2. 安装依赖
sudo dkp-pacman -S switch-glfw switch-libwebp switch-cmake switch-curl devkitA64

# 3. 安装自定义依赖
# devkitpro提供的提供的 ffmpeg/mpv 无法播放网络视频
# 手动编译方法请参考：scripts/README.md
base_url="https://github.com/xfangfang/wiliwili/releases/download/v0.1.0"
sudo dkp-pacman -U \
    $base_url/switch-ffmpeg-7.1-1-any.pkg.tar.zst \
    $base_url/switch-libmpv-0.36.0-3-any.pkg.tar.zst

# 4. build
cmake -B cmake-build-switch -DPLATFORM_SWITCH=ON
make -C cmake-build-switch wiliwili.nro -j$(nproc)
```

</details>

### 交叉编译 PSV 可执行文件

使用本地环境来编译可以参考：
 - [borealis 编译指南](https://github.com/xfangfang/borealis/wiki/PS-Vita)
 - [wiliwili vita 编译指南](https://gist.github.com/xfangfang/305da139721ad4e96d7a9d9a1a550a9d)

注意: 我们使用自定义的 mbedtls, curl 和 ffmpeg 作为依赖, 在使用本地环境编译时，请先卸载 vitasdk 中的相关库，再安装[指定的](https://github.com/xfangfang/wiliwili/tree/yoga/scripts/psv)依赖.

<details>

```shell
# 构建 OpenGL ES 2.0 版
docker run --rm -v $(pwd):/src/ xfangfang/wiliwili_psv_builder:latest \
    "cmake -B cmake-build-psv -G Ninja -DPLATFORM_PSV=ON \
        -DMPV_NO_FB=ON -DUSE_SYSTEM_CURL=ON -DUSE_SYSTEM_SDL2=ON \
        -DCMAKE_BUILD_TYPE=Release && \
        cmake --build cmake-build-psv"

# 构建 Gxm 版 (推荐)
docker run --rm -v $(pwd):/src/ xfangfang/wiliwili_psv_builder:latest-gxm \
    "cmake -B cmake-build-psv -G Ninja -DPLATFORM_PSV=ON \
        -DUSE_SYSTEM_CURL=ON -DUSE_GXM=ON -DUSE_VITA_SHARK=OFF \
        -DCMAKE_BUILD_TYPE=Release && \
        cmake --build cmake-build-psv"
```

</details>

### 交叉编译 PS4 可执行文件

使用本地环境来编译可以参考: 
 - [PacBrew 环境安装](https://github.com/PacBrew/pacbrew-packages)
 - [borealis 编译指南](https://github.com/xfangfang/borealis/wiki/PS4)
 - [编译 wiliwili 依赖的第三方库](https://github.com/xfangfang/wiliwili/blob/dev/scripts/ps4/Dockerfile)

<details>

```shell
docker run --rm -v $(pwd):/src/ xfangfang/wiliwili_ps4_builder:latest \
    "cmake -B cmake-build-ps4 -DPLATFORM_PS4=ON \
        -DMPV_NO_FB=ON \
        -DUSE_SYSTEM_CPR=ON && \
        make -C cmake-build-ps4 -j$(nproc)"
```

</details>

### GLFW or SDL

wiliwili 使用 nanovg 绘制图形和文字，对于创建窗口、按键触摸、输入法等支持是通过 GLFW(默认) 或 SDL 完成的。

因为 GLFW 支持平台有限，在移植到新平台时可以使用 SDL 或者自行实现上述对应接口。

```shell
# 示例
cmake -B build -DPLATFORM_DESKTOP=ON -DUSE_SDL2=ON
cmake --build build
```

<br>

# 应用截图

<p align="center">
<img src="docs/images/screenshot-3.jpg" alt="screenshot">
<img src="docs/images/screenshot-4.jpg" alt="screenshot">
</p>

# Acknowledgement

The development of wiliwili cannot do without the support of the following organization and open source projects.

- Toolchain: devkitpro, switchbrew, vitasdk OpenOrbis and PacBrew
    - https://github.com/devkitPro
    - https://github.com/switchbrew/libnx
    - https://github.com/vitasdk
    - https://github.com/OpenOrbis
    - https://github.com/PacBrew
- UI Library: natinusala and XITRIX
    - https://github.com/natinusala/borealis
    - https://github.com/XITRIX/borealis
- Video Player: Cpasjuste, proconsule, fish47 and averne
    - https://github.com/Cpasjuste/pplay
    - https://github.com/proconsule/nxmp
    - https://github.com/fish47/FFmpeg-vita
    - https://github.com/fish47/mpv-vita
    - http://github.com/averne/FFmpeg
    - http://github.com/averne/mpv
- Misc
    - https://github.com/libcpr/cpr
    - https://github.com/nlohmann/json
    - https://github.com/nayuki/QR-Code-generator
    - https://github.com/BYVoid/OpenCC
    - https://github.com/imageworks/pystring
    - https://github.com/sammycage/lunasvg
    - https://github.com/cesanta/mongoose
    - https://chromium.googlesource.com/webm/libwebp
    - https://github.com/fancycode/MemoryModule

# Special thanks

- Thanks to Crowdin for supporting [open-source projects](https://crowdin.com/page/open-source-project-setup-request).
