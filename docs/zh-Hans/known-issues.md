<!-- translation: lang=zh-Hans source=docs/known-issues.md source-hash=5eeac606a78a314239e9902de22d9ec16fec574a reviewed=no -->
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

## 帧生成

- **在 Linux 上仅支持 2x 和 SDR**。开启 HDR 时，菜单中的该行会显示它已关闭以及原因。
- **Windows 上的 AMD FSR 3.1 帧生成增加的帧很少**：游戏的大部分帧缺少它在 Windows 上所需的深度和运动信息，因此只有一小部分帧会得到一个生成帧。
- **贴近墙壁快速转动镜头时，生成帧的边缘会出现拖影**，Navi 的光晕也是如此。当游戏渲染在 40 fps 左右时，这是帧生成的常见现象。更高的渲染帧率会让它变小。

## 操作

- **游戏菜单中的鼠标还不能点击暂停菜单顶部的标签栏**（切换页面）。标签请用 L 和 R 或对应的按键；菜单的其他部分可以用鼠标。

## 着色器和管线

- **安装、更新或驱动程序更新后的首次启动，以及首次进入某个区域时，可能会出现停顿**，因为此时正在构建着色器和管线。之后的启动会从缓存中读取它们。
- **被强制结束**而不是正常关闭的游戏，在缓存更新后可能没有写入管线缓存。下次运行时会写入它。

## 地图追踪器

- **在标题画面上，Ocarina Atlas 会在游戏内存中搜索** Link，直到载入存档为止。追踪器 API 把这种搜索限制在每分钟约两秒的读取，达到限制时日志会记录一次。

## 报告问题

请在 https://github.com/Larbaco/NX2Saria/issues 提交问题，并附上游戏文件夹中的 `logs\nx2saria.log`（Linux 上为 `logs/nx2saria.log`）。
