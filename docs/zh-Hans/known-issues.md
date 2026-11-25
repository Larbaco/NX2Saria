<!-- translation: lang=zh-Hans source=docs/known-issues.md source-hash=d38a6864bdb7072724d2ef92b4a8c01e00132e1b reviewed=no -->
<!-- languages -->
[English](../known-issues.md) · [Português (Brasil)](../pt-BR/known-issues.md) · [Русский](../ru/known-issues.md) · **简体中文**

> 本页译自[英文版](../known-issues.md)。如两者有出入，以英文版为准。
>
> 本译文尚未经母语者审校。欢迎在[问题跟踪器](https://github.com/Larbaco/NX2Saria/issues)中提出修改。
<!-- /languages -->
# 已知问题

当前版本中我们已知的问题，以及避免它们的方法。某个问题修复后，它会从此列表中移除，发布说明也会注明。[README](../../README.zh-Hans.md#现状与已知问题)介绍了项目的总体状态。

## 图形

- **AMD FSR 3.1 和 FSR 4 在 Windows 上可能出现画面瑕疵**：当场景以小于画面的尺寸渲染时，镜头或 Link 移动时会出现黑色或暗色的斑块和闪烁，移动的角色身后也会出现拖影。使用双线性超分辨率，或者以窗口大小渲染场景且不使用超分辨率器，可以避免这些斑块。已作为 tangerineturtle1/NX2navi#794、tangerineturtle1/NX2navi#475 和 tangerineturtle1/NX2navi#736 报告给原项目。
- **Radeon RX 6000（RDNA 2）上的 FSR 4**：AMD 的驱动程序在这些显卡上提供 FSR 4，它也能运行，但消耗的 GPU 时间比在较新的显卡上多得多（在 RX 6750 XT 上 1440p 下每帧约 12 ms）。在这些显卡上，FSR 3.1 是更轻量的选择。
- **在使用 Mesa 26.2 或 26.3 的 Linux 上，支持 mesh shader 的 Radeon 显卡（RX 6000 及更新型号）会通过 mesh fallback 绘制部分场景。** 这些 Mesa 版本的 task shader 存在缺陷，曾使游戏在会话开始几秒后以“the GPU device was lost”结束（在 RX 6800S 上出现过）。游戏会在这种情况下自动使用 mesh fallback：草和其他同类物体会降低一些帧率，并且在一次运行中第一次出现时会稍晚显示。日志中的 `vk: mesh path:` 行会说明这一点。目前还没有任何 Mesa 版本包含修复；有了之后，游戏的后续版本会在该版本上恢复原生绘制。在 `nx2saria.ini` 中设置 `mesh_fallback = 0` 可在任何 Mesa 上原生绘制。
- **在 Linux 上，Radeon RX 400 和 RX 500 显卡可能以“the GPU device was lost”结束游戏。** 会话报告显示，使用 Mesa 26.2 和 26.3 的 RX 480 和 RX 580 会在会话开始后几秒或几分钟出现此问题，并且在每秒 45 帧及以上时远比 30 帧时频繁。原因尚不清楚。将*帧率上限*设为 30 可以减少出现次数。如果您遇到此问题，请提交 issue，并附上 `logs/nx2saria.log` 和崩溃后的内核日志行（`journalctl -k -b`）。

## 帧生成

- **在 Linux 上仅支持 2x 和 SDR**。开启 HDR 时，菜单中的该行会显示它已关闭以及原因。
- **贴近墙壁快速转动镜头时，生成帧的边缘会出现拖影**，Navi 的光晕也是如此。当游戏渲染在 40 fps 左右时，这是帧生成的常见现象。更高的渲染帧率会让它变小。

## 着色器和管线

- **安装、更新或驱动程序更新后的首次启动，以及首次进入某个区域时，可能会出现停顿**，因为此时正在构建着色器和管线。之后的启动会从缓存中读取它们。
- **在 NVIDIA 上，更新或首次启动会重新构建着色器**（更新长时间停留在“Built ... shaders for your graphics card”，或每次启动都像安装后一样出现停顿），原因是驱动程序的着色器缓存已满：驱动程序会丢弃较旧的着色器来腾出空间，游戏只好重新构建它们。在 NVIDIA 控制面板的“管理 3D 设置”中，将**着色器缓存大小**设为 10 GB 或“无限制”。
- **被强制结束**而不是正常关闭的游戏，在缓存更新后可能没有写入管线缓存。下次运行时会写入它。

## 地图追踪器

- **在标题画面上，Ocarina Atlas 会在游戏内存中搜索** Link，直到载入存档为止。追踪器 API 把这种搜索限制在每分钟约两秒的读取，达到限制时日志会记录一次。

## 报告问题

请在 https://github.com/Larbaco/NX2Saria/issues 提交问题，并附上游戏文件夹中的 `logs\nx2saria.log`（Linux 上为 `logs/nx2saria.log`）。
