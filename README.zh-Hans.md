<!-- translation: lang=zh-Hans source=README.md source-hash=bc7db755a245ca71c7ee004466aceb0b1c9f53be reviewed=no -->
<h1 align="center">
  <img src="docs/images/nx2saria-banner.png" alt="NX2Saria" width="1200">
</h1>

<p align="center">
  <strong>Ocarina of Time，重新编译为原生 PC 游戏</strong><br>
  Windows &amp; Linux · x86-64 · Vulkan
</p>

<p align="center">
  <a href="https://github.com/Larbaco/NX2Saria/releases/latest"><img alt="最新版本" src="https://img.shields.io/github/v/release/Larbaco/NX2Saria?label=release"></a>
</p>

<!-- languages -->
<p align="center"><a href="README.md">English</a> · <a href="README.pt-BR.md">Português (Brasil)</a> · <a href="README.ru.md">Русский</a> · <strong>简体中文</strong></p>

> 本页译自[英文版](README.md)。如两者有出入，以英文版为准。
>
> 本译文尚未经母语者审校。欢迎在[问题跟踪器](https://github.com/Larbaco/NX2Saria/issues)中提出修改。
<!-- /languages -->

<p align="center">
  <a href="docs/zh-Hans/install.md"><strong>安装</strong></a> ·
  <a href="https://github.com/Larbaco/NX2Saria/releases">发布版本</a> ·
  <a href="docs/zh-Hans/controls.md">操作</a> ·
  <a href="docs/zh-Hans/known-issues.md">已知问题</a> ·
  <a href="docs/building.md">从源码构建</a>（英文）
</p>

**NX2Saria** 将 Nintendo Switch 2 版 *The Legend of Zelda: Ocarina of Time*（游戏更新 **1.1.1**）变成原生的 Windows 和 Linux 游戏。游戏代码被静态重新编译为 x86-64，并通过 Vulkan 绘制：底层没有运行任何模拟器。

它面向拥有本游戏、并能从自己的主机上转储游戏的玩家。**开始时，请从[最新版本](https://github.com/Larbaco/NX2Saria/releases/latest)下载 Windows 安装程序或 Linux AppImage，然后按照[安装指南](docs/zh-Hans/install.md)操作**。启动器会用你的转储在你的 PC 上构建游戏，之后让它保持最新。

> [!IMPORTANT]
> **请自备游戏**。本项目不包含任何游戏文件。你需要自己合法获得的游戏副本，并从你自己的主机上转储。本项目不提供获取这些文件的说明。构建完成的游戏包含来自你的游戏的内容：请仅供自己使用，不要分发。

## 为什么选择 NX2Saria

- **安装一次，在启动器中获取更新**。安装程序和 AppImage 会用你的转储构建游戏。启动器会提供每个新版本（默认为 *Stable*，预发布版本为 *Preview*），并保留你的存档、模组和设置。[安装与更新 →](docs/zh-Hans/install.md)
- **Linux 上同样拥有完整画面**。AMD FSR 3.1 超分辨率和 FSR 3.1 帧生成通过游戏自己的着色器通道运行，因此 Linux 不需要任何 AMD 库，只要 GPU 的 Vulkan 驱动程序能运行这些通道即可。在测试所用的 RX 6700（RADV）上，3440x1440 下帧生成每帧约占用 4 ms GPU 时间。[Linux 配置 →](docs/README-BuildKit.md#linux)（英文）
- **超宽屏能用满整个屏幕**。在 21:9 和 32:9 下，暂停、地图和系统菜单会铺满屏幕；如果你愿意，也可以让它们留在中间、两侧为黑边。HUD 可以把心、道具和小地图移到屏幕的角落。[超宽屏上的 HUD 和菜单 →](docs/wide-hud.md)（英文）
- **实时的地图和道具追踪器**。[Ocarina Atlas](docs/ocarina-atlas.md)（英文）2.3.0 及更高版本会在地图上跟随 Link，并在你游玩时勾选收集品；对 [hyrule-tracker](docs/hyrule-tracker.md)（英文）的支持在其项目的一个尚未合并的拉取请求中。追踪器 API 是只读的，只能从你的 PC 访问，并且在你设置 `tracker_api = 1` 之前处于关闭状态。[追踪器 API →](docs/tracker-api.md)（英文）
- **着色器仍在准备时也更流畅**。在更新或驱动程序更新之后，后台的管线构建会等到轻松的时刻（菜单、加载）再进行，而不是与游戏争抢资源。在 Kokiri Forest、1280x720、驱动程序缓存为空的情况下，平均帧率从约 25 fps 提高到 27-28 fps，最慢 1% 的帧从 64-70 ms 降到 59 ms。窗口改变大小或驱动程序拒绝某项设置时，崩溃和错误画面也更少。[详情 →](docs/changelog/v0.15.0.md)（英文）
- **amiibo 不再麻烦**。当游戏请求 amiibo 时，一个列出你自己手办转储的列表会在游戏上方打开。每个手办都保留自己的数据，回写是可选的，每日限制也可以关闭。[amiibo 指南 →](docs/zh-Hans/amiibo.md)
- **设置在你准备好时再应用**。菜单中的更改会立即预览，只有在你选择 **Apply** 时才会保存。**Ctrl+F5** 可以在游戏运行时重新加载 `nx2saria.ini`，当前游戏版本不使用的设置行会被隐藏。
- **汇集社区修复**。来自更广泛社区的修复和提速，例如 AMD 纹理修复、将显存控制在预算之内以及接触阴影，都已合并进来，并在[发布说明](docs/releases/)（英文）中注明其作者。
- **为测试而生**。`boot_map` 可以跳过标题画面，直接从存档在某个区域启动游戏（[直接启动到地图](docs/boot-to-map.md)（英文））；`tools/bench.py` 能以相同的方式把场景测量两次（[性能](docs/performance.md)（英文））；最近的改动会把它们所做的事写入游戏日志，因此一份报告就能看出这些改动是否开启并正常工作。

## 在任何 PC 上

- **帧率**：默认 60 fps，也可以限制在 30 到 240 之间、使用显示器的刷新率或不限制。
- **宽屏**：窗口的比例、21:9 或 32:9。过场动画保持 16:9；HUD 和菜单也可以保持 16:9。
- **画质**：从 1080p 到 4K 及更高的分辨率、超分辨率、抗锯齿、帧生成和 HDR。
- **操作**：键盘、鼠标和手柄，按键重映射、振动和陀螺仪。
- **游戏中**：按 **F1**（手柄上为 **Minus + Plus**）打开设置；按 **F10** 显示性能浮层。[操作与菜单 →](docs/zh-Hans/controls.md) · [所有设置 →](nx2saria.ini.example)（英文）

<details>
<summary><strong>各平台的图形选项</strong></summary>

- **超分辨率和抗锯齿**：RTX GPU 上的 NVIDIA DLSS / DLAA，任何能运行 AMD FSR 3.1 的 GPU 上的 FSR 3.1，Windows 上受支持的 Radeon GPU 上的 AMD FSR 4，或者 FSR 1 和双线性缩放。
- **帧生成**：RTX 40 / 50 GPU 上的 NVIDIA DLSS（在 Linux 上需要 NVIDIA 单独提供的 DLSS-G 库，并且启动时已开启 DLSS 超分辨率）。AMD FSR 3.1 在 Windows 上使用 AMD 的库；在 Linux 上由游戏自己运行这些通道，仅支持 2x 和 SDR，可搭配任意超分辨率选项。
- **HDR**：游戏原生的 HDR，或增强的色调映射画面，带有显示器、游戏和 HUD 的亮度控制。当所需的显示格式不可用时，增强 HDR 会在本次运行中回退到 SDR。

可用的选项取决于平台、GPU、驱动程序和库。[构建套件指南](docs/README-BuildKit.md)（英文）介绍了配置方法；[架构指南](docs/architecture.md)（英文）介绍了渲染器。

</details>

## 开始使用

### 系统要求

- **游戏**：你自己的 Nintendo Switch 2 版 `.nup` 转储，带有更新 **1.1.1**。构建过程会检查版本；不要以为其他更新也兼容。
- **CPU 和内存**：支持 SSE4.2 的 x86-64 处理器，16 GB 或更多内存。如果只有 16 GB，请在游玩前阅读[内存和页面文件建议](docs/README-BuildKit.md#playing)（英文）。
- **存储空间**：除转储外，游戏需要约 34 GB 可用空间；Windows 启动器及其工具还需要约 1.5 GB。
- **图形**：支持 `VK_EXT_external_memory_host` 和 `VK_EXT_mesh_shader` 的 Vulkan 1.3，以及最新的驱动程序。
- **系统**：Windows 10 或更高版本，或者内核 6.7 或更高版本的 Linux。

### 安装

每个[发布版本](https://github.com/Larbaco/NX2Saria/releases)都提供 Windows 版的 `NX2Saria-Setup-<version>.exe` 和 Linux 版的 `NX2Saria-Linux-<version>-x86_64.AppImage`。[安装指南](docs/zh-Hans/install.md)列出了具体步骤，包括从 NX2navi 安装的启动器如何切换到 NX2Saria 的发布版本。

### 从源码构建

[从源码构建](docs/building.md)（英文）介绍如何搭建工具链并获取代码；[构建套件指南](docs/README-BuildKit.md)（英文）介绍如何从准备好的套件构建。在 Linux 上，安装好[所需的软件包](docs/README-BuildKit.md#linux)（英文）之后：

```sh
python3 -I tools/make_game.py --nup "/path/to/your-game.nup" --out "$HOME/Games/Ocarina"
"$HOME/Games/Ocarina/ocarina"
```

构建会在空闲内存足够时才启动每个编译任务，因此内存较少的电脑也无需任何选项；`--jobs` 和 `--mem-limit` 只用于进一步限制（[构建的内存控制](docs/build-memory.md)（英文））。

## 现状与已知问题

**开发中**。游戏可以游玩，但图形、稳定性和性能仍因场景、硬件和驱动程序而异。

- 安装或驱动程序更新后的首次启动，以及首次进入某个区域时，可能会在构建着色器和管线时出现停顿。
- 16 GB 内存的系统在 Hyrule Field 可能会内存不足：请参阅[内存建议](docs/README-BuildKit.md#playing)（英文）。
- 某些 AMD 集成显卡可能因主机内存堆的限制而在标题画面失败。
- 仍有画面错误和崩溃，包括游戏粒子代码中一个罕见的崩溃。Linux 上的长时间游玩和 Steam Deck 硬件仍在验证中（[Linux 说明](docs/building.md#linux-in-progress)（英文））。

[已知问题](docs/zh-Hans/known-issues.md)列出了当前版本尚未解决的问题以及避免它们的方法。

**发现了 bug？** 请在[问题跟踪器](https://github.com/Larbaco/NX2Saria/issues)上提交问题，并附上游戏文件夹中的 `logs\nx2saria.log`（Linux 上为 `logs/nx2saria.log`）。请说明你使用的版本或提交、操作系统、GPU 和驱动程序，以及如何复现；[CONTRIBUTING.md](CONTRIBUTING.md)（英文）有完整的清单。检查附件中是否含有私人路径，并且绝不要上传游戏文件或密钥。

启动器还可以发送每次会话运行情况的简短报告以及评论，无需 GitHub 账户：只在你选择时发送。[会话报告与评论](docs/zh-Hans/telemetry.md)列出了每一个字段。

## 文档

| 主题 | 指南 |
|---|---|
| 游玩 | [安装与更新](docs/zh-Hans/install.md) · [操作](docs/zh-Hans/controls.md) · [设置](nx2saria.ini.example)（英文） · [amiibo](docs/zh-Hans/amiibo.md) · [已知问题](docs/zh-Hans/known-issues.md) · [会话报告](docs/zh-Hans/telemetry.md) |
| 追踪器 | [追踪器 API](docs/tracker-api.md)（英文） · [Ocarina Atlas](docs/ocarina-atlas.md)（英文） · [hyrule-tracker](docs/hyrule-tracker.md)（英文） |
| 发布 | [发布说明](docs/releases/)（英文） · [更新日志](docs/changelog/)（英文） |
| 构建 | [构建套件](docs/README-BuildKit.md)（英文） · [从源码构建](docs/building.md)（英文） · [启动器](docs/launcher.md)（英文） · [安装程序](docs/installer.md)（英文） |
| 测试 | [性能](docs/performance.md)（英文） · [直接启动到地图](docs/boot-to-map.md)（英文） |
| 参与贡献 | [贡献指南](CONTRIBUTING.md)（英文） · [架构](docs/architecture.md)（英文） · [Maxwell 渲染器说明](docs/maxwell.md)（英文） |

**名称**。设置文件是 `nx2saria.ini`，环境变量以 `NX2SARIA_` 开头，发布文件以 `NX2Saria-` 开头。来自 NX2navi 的设置、存档和配置档案仍然可用；[名称与兼容性](docs/branding.md)（英文）列出了旧名称。

**游戏内容**。构建输出保存在被忽略的 `build/` 和 `generated/` 文件夹中，运行时的着色器以源码形式保存。提交之前请运行 `python tools/check_content.py`。

## 致谢与来源

NX2Saria 由 **[Larbaco](https://github.com/Larbaco)** 作为独立项目维护。它是 **[NX2navi](https://github.com/tangerineturtle1/NX2navi)** 的分支，NX2navi 的重新编译流程、运行时、渲染器和启动器是它的基础：这些成果归功于 NX2navi 的作者。社区贡献者的改动在发布说明中注明，每一项贡献都在提交历史中保留其作者。

感谢 [Triforce 项目](https://git.triforceproject.com/triforce/source)提供的思路：它的原生帧生成设计影响了 NX2Saria 在 Linux 上的 FSR 3.1 帧生成，游玩闸门（play gate）、管线缓存标记和呈现检查也源自它的思路。这里没有使用它的任何代码。

NX2Saria 的名称和美术作品用于标识本项目。它们并不意味着这是 NX2navi 的官方版本或任天堂的项目。出于兼容性考虑，一些文件和设置名称有意保留了 NX2navi 的拼写；[名称与兼容性](docs/branding.md)（英文）列出了它们。

## 许可与声明

源代码公开供阅读。其新增和修改部分保留所有权利，见 [LICENSE-ADDITIONS](LICENSE-ADDITIONS)（英文）：未经书面许可，不得复制或发布其中任何部分。基础导入部分仍适用其 [MIT 许可证](LICENSE)（英文），第三方代码保留各自的许可证，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)（英文）。任何许可证都不对任天堂的游戏授予任何权利。

NX2Saria 是一个独立的非商业粉丝项目，与任天堂无关，也未获得任天堂的认可。*The Legend of Zelda*、*Ocarina of Time* 和 *Nintendo Switch* 是任天堂的商标。

本 README 及各许可文件以英文文本为准。
