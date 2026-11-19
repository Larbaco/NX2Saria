<!-- translation: lang=zh-Hans source=docs/controls.md source-hash=e772509093132509302e7571271d7a6efebe3ade reviewed=no -->
<!-- languages -->
[English](../controls.md) · [Português (Brasil)](../pt-BR/controls.md) · [Русский](../ru/controls.md) · **简体中文**

> 本页译自[英文版](../controls.md)。如两者有出入，以英文版为准。
>
> 本译文尚未经母语者审校。欢迎在[问题跟踪器](https://github.com/Larbaco/NX2Saria/issues)中提出修改。
<!-- /languages -->
# 操作

输入来自 SDL3 手柄、键盘和鼠标，合并为一个 Switch 控制器（`runtime/input.c`、`runtime/input_win.c`）。每个已连接的手柄都保持打开，游戏读取最后按下的那个；游玩中打开的手柄会在一秒内被识别。按键取“或”；每个摇杆取偏移更大的那个来源。只有游戏窗口位于前台时，键盘按键和鼠标按键才会生效。`NX2SARIA_INPUT=none` 会同时禁用两者（用于自动化运行）。`NX2SARIA_*` 设置也可以写在 exe 旁边的 `nx2saria.ini` 中（`input = none`、`motion = 0`、`rumble = 0`）；环境变量优先于该文件。旧的 `NX2NAVI_*` 名称和 `nx2navi.ini` 仍然有效（[名称与兼容性](../branding.md)（英文））。`NX2SARIA_RUMBLE_STRENGTH` 是以百分比表示的振动强度（0 到 300，默认 130；0 等同于 `rumble = 0`）。`NX2SARIA_RUMBLE_CURVE` 是在应用强度之前作用于振幅的指数（0.2 到 1.5，默认 0.6；越低，弱振动越强，1 为线性）。`NX2SARIA_RUMBLE_TRIGGERS=1` 还会驱动 Xbox 手柄的脉冲扳机。`NX2SARIA_DS3=1` 会开启 DualShock 3 的直接读取器（见“PlayStation 3 手柄”）。

`NX2SARIA_PAD_LAYOUT`（`pad_layout`，操作标签页中的*按键布局*行，实时生效）决定哪个正面按键是 Switch 的 A：

- `nintendo`（默认）：按位置，与 Switch 上一样（见下表）。
- `xbox`：下 A、右 B、左 X、上 Y，与 Xbox 手柄上的标注一致；在 PlayStation 手柄上，叉为 A、圆圈为 B、方块为 X、三角为 Y。
- `playstation`：与 `xbox` 相同的按键，只是使用 PlayStation 的名称：叉为 A、圆圈为 B、方块为 X、三角为 Y。
- `auto`：对于 SDL 报告其右侧按键标为 A 的手柄（Switch Pro Controller、Nintendo 布局的手柄）使用 `nintendo`，其他手柄使用 `xbox`。

日志会写出当前生效的布局（`input: gamepad layout ...`）以及每个手柄的第一次按键（`input: gamepad "...": first press`）；菜单会在*自动*下方显示该布局。除非*按键提示*另有设置（见下文），游戏的按键提示显示 Switch 的字母。

动作提示（右上角）以菱形显示四个正面按键。游戏按 Switch 的位置绘制它（A 在右、B 在下、X 在上、Y 在左），`nintendo` 保持这种位置。在 `xbox`、`playstation` 和 `auto` 下，无论*按键提示*如何设置，菱形都会改为按这类手柄排列（A 在下、B 在右、X 在左、Y 在上）。

游戏在启动时、在任何手柄打开之前读取一次菱形：

- `auto` 得到的是 Xbox 菱形，因此在 `auto` 下，Switch Pro Controller 会看到 A 在下方，尽管它的 A 在右侧。对这种手柄请选择 `nintendo`。
- 布局的更改要在重启后才会显示；在此之前，*按键提示*会显示*按键提示重启后生效*。
- 修改动作提示的 RomFS 模组会保留它自己的菱形。出于其他原因替换同一个归档的模组，会在其更改之上移动菱形。

日志会在以 `action prompt:` 开头的一行中说明发生了什么。布局的修改方式：`runtime/action_prompt.c`（`UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs` 中的 `DoCommand_00`，从 `<profile>/action-prompt/` 提供）。

## 按键提示

`NX2SARIA_PROMPTS`（`prompts`，操作标签页中的*按键提示*行）决定游戏的按键提示显示哪种按键：`nintendo`（默认，游戏原有的）、`xbox`、`playstation`、`steamdeck` 或 `pc`（*键盘和鼠标*）。它会改变游戏绘制的三样东西：

- 所有文字提示（对话、按键说明、操作说明画面）使用的按键字体：A B X Y、L R ZL ZR、Plus 和 Minus、十字键以及摇杆和它们的方向，会变成该手柄的按键（LB RB LT RT、Menu 和 View；L1 R1 L2 R2、Options 和 Create；它的十字键和摇杆），或者键盘按键。Power、Home 和主机保持不变；
- 动作提示（右上角）中的字母；
- 陶笛的音符按键。

A、B、X 和 Y 显示的是*按键布局*所按下的按键：使用 `nintendo`（按位置）时，Switch 的 A 是右侧正面按键，因此它的提示显示 B 或圆圈，B 显示 A 或叉，X 显示 Y 或三角，Y 显示 X 或方块；使用 `xbox`、`playstation` 和 `automatic`（按标注）时，A 显示 A 或叉，B 显示 B 或圆圈，X 显示 X 或方块，Y 显示 Y 或三角。

`pc` 显示每个 Switch 按键对应的第一个键盘按键（上表，或工具运行时 `nx2saria.ini` 中的 `bind_` 行）：E、Ctrl、Space，Y 显示鼠标及其左键，Q、R，ZL 显示鼠标及其右键，F、Tab、M，摇杆按下为 V 和 C，十字键为 1 到 4，移动为 W A S D，提示所指的方向按键盘上的排列给出。镜头的提示显示移动的鼠标。它不受*按键布局*影响。

游戏在启动时读取一次字体和界面纹理，因此更改要在重启后才会显示（该行会这样提示；菜单的“重启”按钮会重启游戏）。在此标签页中重新映射的按键或键盘按键会保留旧的提示，直到重新生成这些文件夹。

按键提示位于 exe 旁边的覆盖文件夹中：`romfs-patches-xbox`、`romfs-patches-xbox-label`、`romfs-patches-playstation`、`romfs-patches-playstation-label`、`romfs-patches-steamdeck` 和 `romfs-patches-steamdeck-label`（按位置和按标注）以及 `romfs-patches-pc`，`runtime/romfs.c` 会加载它们来代替游戏的文件。`tools/pad_prompts.py` 在玩家的机器上，根据玩家自己的 RomFS 和一个按键图片文件夹绘制它们。这些图片位于仓库的 `prompt-art` 文件夹中：来自一个 CC0 图包，其 `SOURCE.md` 写明了图包名称和来源。每个启动器发布版本、AppImage 和构建套件都带有这些图片（`tools/prompt_art.py`），`tools/make_game.py` 会在最后一步绘制所有文件夹，并在图片、工具或 `bind_` 行发生变化时再次绘制（因此重新绑定按键后重新构建，键盘提示就会更新）。手动生成：

```
python tools\pad_prompts.py --art prompt-art --game <install folder>
```

`--style xbox|playstation|steamdeck|pc` 只生成一种样式；`steamdeck` 显示 Deck 自己的标注（A B X Y 位于 Xbox 手柄上相同的位置，L1 R1 L2 R2 和 L3 R3，Menu 和 View），并由 Xbox 和 PlayStation 的图片绘制而成，因为图片中没有 Deck 样式；键盘按键来自 `<install folder>\nx2saria.ini`（`--binds` 可指定另一个文件）。图片文件夹包含 `XGamepad/Default`、`XGamepad/Light`、`P5Gamepad/Default`、`P5Gamepad/Light`、`Keyboard_Mouse/Dark` 和 `Keyboard_Mouse/White`，文件名见工具中的 `STYLES` 和 `KEY_ART`；没有图片的键盘按键会写在 `Keyboard_Mouse/Blanks` 上。没有这些文件夹时，该行会显示*缺少按键提示文件：请在启动器中更新或重新构建游戏*（Windows 启动器中的 Rebuild the Game，Linux AppImage 中的 Update），并保留 Switch 的提示；当 RomFS 模组替换了其中某个相同的文件时也是如此（*已关闭：RomFS 模组替换了它们的文件*）。日志在启动时会有一行 `button prompts:`，说明构建从哪里绘制了按键提示，或为什么没有绘制（游戏文件夹中的 `button-prompts.txt`），另有一行 `RomFS overlay: ... button prompts`，说明正在使用的文件夹。该工具需要 fontTools、Pillow 和 zstandard（`tools/requirements-py314.txt`）。

各动作是游戏自己的，名称与游戏的 *Basic Controls* 画面（Plus 菜单，System）一致：

| Switch | 手柄（按位置，Nintendo 布局） | 键盘和鼠标 | 游戏世界中的动作 |
|---|---|---|---|
| 左摇杆 | 左摇杆 | W A S D | 移动 |
| 右摇杆 | 右摇杆 | 鼠标；方向键作为第二个来源 | 镜头 |
| A | 右 | E、Left Shift | 动作（交谈、打开、拾起、攀爬、抓住）；按住为冲刺 |
| B | 下 | Left Ctrl、Backspace | 收起、取消、返回 |
| X | 上 | Space | 跳跃 |
| Y | 左 | 鼠标左键 | 用剑攻击 |
| ZL | 左扳机（超过 50%） | 鼠标右键、Z | 锁定（按住或切换，由游戏的 *Targeting Type* 选项决定） |
| ZR | 右扳机（超过 50%） | F、X | 使用所选道具（弹弓、弓、回旋镖、钩爪、炸弹、瓶子） |
| L | 左肩键 | Q | 快速选择道具；游戏菜单中的上一页 |
| R | 右肩键 | R | 快速选择道具；游戏菜单中的下一页 |
| 左摇杆按下 | 左摇杆按下 | V | （游戏的操作说明画面中没有） |
| 右摇杆按下 | 右摇杆按下 | C、鼠标中键 | 把镜头重置到 Link 身后 |
| 十字键左 | 十字键左 | 1、O | 吹奏陶笛 |
| 十字键上 | 十字键上 | 2 | 戴上或取下配饰 |
| 十字键右 | 十字键右 | 3、N | 与 Navi 交谈 |
| 十字键下 | 十字键下 | 4 | 更换靴子 |
| Plus | Start | Tab | 菜单：道具、装备、收集、系统 |
| Minus | Back | M | 地图 |

游戏的 *Swap Jump Controls* 选项会交换 B 和 X（B 跳跃，X 收起）；绑定仍然对应 Switch 按键，因此这时 Space 变为收起，Left Ctrl 变为跳跃。Left Shift 是 A，所以按住它会冲刺；E 按一次 A，用于交谈和打开。Esc 在游戏中没有绑定；它会打开设置菜单并释放鼠标光标（见“鼠标视角”）。Z 和 X 在相邻的按键上重复 ZL 和 ZR。鼠标滚轮在游戏过程中没有绑定；在游戏自身的菜单中它用于滚动（见“在游戏菜单中使用鼠标”）。绑定是 `runtime/input_map.c` 中 `input_map` 的各张表：键盘按键（与平台无关的按键 ID）以及每种布局一张手柄表（按位置的手柄输入）；`runtime/input_win.c` 负责转换 Win32 虚拟键和 SDL 按键。

## 窗口快捷键

| 按键 | 手柄 | 动作 |
|---|---|---|
| F1 或 Esc | 同时按 Minus 和 Plus | 打开或关闭设置菜单；菜单打开时游戏收不到任何输入 |
| F8 | | 错误捕获：把你所在的位置、镜头、画面、设置、日志末尾和你的存档保存到 `logs\captures`，并在屏幕上显示一条提示说明其名称（[bug-capture.md](../bug-capture.md)（英文）） |
| F10 | L3 + R3（同时按下两个摇杆） | 性能浮层：关闭、帧率、完整面板；保存到 `nx2saria.ini`（菜单打开时，这是对菜单草稿的一次更改，见下文） |
| Ctrl+F5 | | 重新读取 `nx2saria.ini` 并在不重启的情况下应用更改（见下文“重新加载 `nx2saria.ini`”） |
| F11 | | 开启或关闭无边框全屏，保存到 `nx2saria.ini`；按住 Ctrl、Shift 或 Alt 时无效，因为浮层和截图录像工具会使用这些组合；它优先于仍在菜单草稿中等待的全屏更改 |

窗口会在 `NX2SARIA_MONITOR`（`nx2saria.ini` 中的 `monitor`）指定的显示器上打开，F11 全屏会覆盖窗口所在的显示器。`nx2saria.exe --list-monitors` 会在不需要游戏文件的情况下列出每个显示器的序号、名称（在 Windows 上还有设备名）、桌面矩形、是否为主显示器以及 DPI；`nx2saria.exe --monitor <value>` 为单次运行设置 `NX2SARIA_MONITOR`，并优先于该文件。值可以是 `auto`（默认）、`primary`、列表中的序号（从 1 开始计数），或显示器名称的一部分（在 Windows 上也可以是设备名，例如 `display3`；在 Linux 上名称包含接口，例如 `dp-2`）；没有任何匹配时，日志会列出所有显示器，窗口按 `auto` 的方式放置。在 Wayland 上窗口无法自行定位：只有全屏（`fullscreen = 1`）会在所选显示器上打开，窗口模式的窗口会在合成器放置的位置打开。

Minus+Plus 组合键永远不会传给游戏。在菜单中，方向键、十字键或左摇杆用于移动，Enter 或 A 用于选择，Q 和 E 或 L 和 R 用于切换标签页，Esc 或 B 用于返回（Switch 按键名称，与上表相同）。使用鼠标时，点击某一行会选中它，点击值两侧带框的两个箭头可以更改设置（按住时箭头会连续触发）；点击该行的其他任何位置都不会更改任何内容。

## 音频输出

声音在系统默认设备上播放，并在 Windows 默认设备改变时跟随它。`NX2SARIA_AUDIO_DEVICE`（`nx2saria.ini` 中 `[audio]` 部分的 `audio_device`）可以指定另一个设备：一个从 1 开始、按 `nx2saria.exe --list-audio-devices` 打印顺序计数的数字，或者设备名称的一部分（不区分大小写）。`--list-audio-devices` 会在不需要游戏文件的情况下打印每个播放设备的序号、名称、声道数和频率，标出系统默认设备，并显示该设置会选中哪个设备；`nx2saria.exe --audio-device <value>` 为单次运行设置 `NX2SARIA_AUDIO_DEVICE`，并优先于该文件。没有任何匹配或设备无法打开时，日志会列出所有播放设备，游戏在系统默认设备上播放；游戏运行时所选设备消失（虚拟输出被移除、耳机被拔出）也会把声音移到系统默认设备。`audio_channels = auto` 会询问所选设备有多少个声道。

## 在菜单中应用更改

在设置菜单中所做的更改在应用之前都只是草稿；更改某一行时不会向 `nx2saria.ini` 写入任何内容（`runtime/ui/option_rows.cpp`）。

- 可以低成本预览的实时设置会立即生效，方便你判断效果：音量、锐度、HDR 亮度和色调映射各行、超分辨率器、抗锯齿、画面比例、低延迟、文字速度、性能浮层、振动、冲刺按键、死区以及按键绑定。草稿会保留它原来的值，*Discard* 会把它恢复。
- 所有在启动时读取的设置（其行显示*重启后生效*），以及帧率限制、垂直同步、HDR、按键布局、步行键、体感、全屏和窗口大小，则会留在草稿中等待；该行显示待定的值，只有 *Apply* 才会改变游戏。这些是 `option_rows.cpp` 中的 `is_staged` 情况。
- 带着更改离开菜单时（Esc、B、*继续*、F1，或同时按 Minus 和 Plus），会打开 *Apply your changes?*，以 `setting: old -> new` 的形式列出更改，并标出只在重启后生效的项。*Apply* 会写入设置、保存 `nx2saria.ini` 并关闭菜单；*Discard* 会撤销预览并关闭菜单；*Continue editing*（默认选项，也可按 Esc 或 B）会返回设置页面并保留草稿。切换标签页不会询问。F1 和手柄组合键只会发出一个由菜单自身线程处理的请求，因此它们永远不会丢失草稿。
- *Apply* 之后，可能会接连出现另外两个对话框（每个都会在前一个关闭后的下一帧打开）。首先，当*全屏*、窗口大小（*Resolution* 行，或者在没有宽屏设置的游戏版本上为*窗口大小*）、*垂直同步*或 *HDR*（在关闭和色调映射之间切换，这会以另一种格式重新创建交换链）发生变化时，会出现 *Keep these display settings?*：这些实时设置可能让玩家得不到可用的画面。它们会立即生效，只有在 *Keep* 之后才写入 `nx2saria.ini`；草稿的其余部分在 Apply 时保存。*Resolution* 行会随窗口大小一起设置游戏分辨率，两者一同处理：它的游戏分辨率在 *Keep* 时保存，在 *Revert* 时恢复，之后不再出现在下面的重启对话框中（`option_rows.cpp` 中的 `group_timed`）。此时会运行 15 秒的倒计时（`plat_now_ns`）；*Revert*（默认选项，也可按 Esc 或 B）、超时、以任何方式关闭菜单以及退出游戏，都会恢复旧值且不保存，而在此期间进行的保存（F10、金手指、关闭窗口）会跳过这些值（`cfg_hold`）。倒计时期间按 F11 表示玩家接管：对话框关闭，所有已应用的内容（包括 F11 的全屏状态）都会保留并保存。然后，当某个已应用的设置在启动时读取时（GPU、HDR 切换到或离开游戏原生 HDR、在关闭低延迟启动的运行中开启低延迟……），会出现 *Some changes take effect after a restart*：它列出这些设置，并提供 *Restart now*（与“重启”按钮相同的重启）和 *Later*（默认选项，也可按 Esc 或 B），后者会关闭菜单，并把这些值保存到下次启动时使用。
- *重启*会先应用并保存草稿，因为正是重启让那些只在重启后生效的更改生效；它的对话框会这样说明。*退出游戏*会丢弃草稿（它的对话框会这样说明），关闭窗口也是如此。
- 改回初始值的更改会离开草稿。在此期间被其他东西改变的设置（*全屏*上的 F11）也会离开草稿，并保留外部的值。由环境变量固定的设置显示为灰色，永远不会进入草稿。
- 草稿打开期间由其他任何操作触发的保存（菜单关闭时的 F10、F11、金手指）不会写入预览中的值：该项在 Apply 之前不参与任何保存（`runtime/cfg.c` 中的 `cfg_hold`、`cfg_revert`）。
- 每一步都会记录在 `logs/nx2saria.log` 中，因此事后可以检查一次运行：某个设置进入或离开草稿时（预览或待定）记录 `menu: draft +` 和 `menu: draft -`，每个打开的对话框及给出的回答（按钮、Esc 或 B、倒计时结束、F11）记录 `menu: box`，`menu: draft applied` / `discarded` 附带每个值，`menu: display kept` / `reverted`，每次保存记录 `cfg: wrote <path>: <keys>`，被保留的预览记录 `cfg: save leaves out <key>`，Ctrl+F5 等待菜单时记录 `cfg: reload: not run`。

## 画质预设

*设置 > 显示 > 画面* 的第一行是 *画质预设*，它一次性设置各项图形选项：*推荐（…）*、*低*、*中*、*高* 或 *极高*。

- *高* 即游戏发售时的原样。*低* 和 *中* 绘制更少，适合较慢的电脑和掌机：更小的 3D 场景和 1080 行高的画面；*低* 还会用更简单的滤镜放大场景，并且仅在集成显卡或掌机上减少纹理过滤、绘制更少的草，并把帧率限制为 30 或 40 fps。在 NVIDIA RTX 显卡上它们使用 DLSS，在 Windows 上的 Radeon RX 9000 显卡上使用 FSR 4。*极高* 绘制更多，显示器越大绘制越多；它从不被推荐，只能手动选择。任何预设都不会开启帧生成。
- *推荐* 会在每次启动时根据显卡、内存和处理器重新选择。在得到 *高* 的电脑上，什么都不会改变。如果你的电脑得到 *低* 或 *中*，而你从未选择过预设，会有一条通知告诉你一次。如需像以前一样游戏，请在 *画质预设* 一行选择 *高*（或在 `nx2saria.ini` 中写入 `preset = high`）。
- 你选择的预设只要电脑不变就会保留；当它不是推荐的预设时，该行会显示 *本机推荐：…*。更换显卡、处理器或增加内存后（驱动程序更新不算），游戏会使用 *推荐*，直到你再次选择，该行会显示 *硬件已更换：正在使用推荐预设*。在那里选择 *推荐*（在其上按左）并应用即可保留它，与选择其他任何预设一样。
- 你手动更改的选项会保留你的值，预设随即显示为 *…（自定义）*。当预设所设置的选项被手动改成其他值时，选择该预设会先列出这些选项：*使用预设* 会把它们改为预设的值，*取消* 则保持原样。
- 选择属于草稿（[见上文](#在菜单中应用更改)）：*Apply* 会把 `preset = <tier>` 写入 `nx2saria.ini`（选择 *推荐* 时不写这一行）。预设更改的分辨率、阴影和草会在重启后生效。
- *场景分辨率* 位于 *Upscaling* 下方，在 DLSS 和 FSR 都未开启时显示，用于设置 3D 场景的绘制尺寸，从画面的 33 % 到 100 %（原生）。越低越快、越模糊，适合较慢的电脑和掌机；HUD 和菜单保持清晰。更改立即生效；比游戏启动时更大的尺寸要在重启后才生效。在这个版本中，`render_scale` 和 `output_res` 不起作用：降低场景尺寸靠的是这一行和预设。
- `logs/nx2saria.log` 的开头会说明选择了什么以及原因：`preset: hardware: ...`（游戏找到的硬件）、`preset: recommended <tier>: <reason>`、`preset: in use <tier> (<source>)`，以及预设设置的每个选项各一行 `preset:   <KEY> = <value> (...)`，说明该值来自预设、你的设置文件还是设备配置。报告画面或帧率问题时请附上日志。

预设如何选择以及每个预设设置了什么：[Quality presets](../presets.md)（英文）。

## 重新加载 `nx2saria.ini`

Ctrl+F5 会在游戏运行时重新读取 `nx2saria.ini`，因此在文本编辑器中修改的一行无需重启或进入菜单即可生效。单独的 F5 不是重新加载：它仍然是一个可以绑定的按键（Ctrl+F5 也会按下绑定到 F5 的操作，因为游戏读取的是键盘状态；如果这造成干扰，请在*操作*标签页中取消 F5 的绑定）。窗口代码（Win32 和 SDL 都一样）会在按键按下时一并看到 Ctrl 的状态，因此这个快捷键不需要改动按键绑定。

- 每个所在行被修改、添加或删除（此时它恢复为默认值）的设置都会取得新值；窗口顶部会显示一条简短的提示 *N changed, M after restart*，如果有值被拒绝，还会附加 *K rejected*。菜单标为*重启后生效*的设置会在内存中更改，并在日志中以 `(takes effect after restart)` 列出。
- 新值的读取方式与启动时相同（值后面的注释会被去掉，`on`、`off`、`true`、`false`、`yes` 和 `no` 都可用于开关），并会先经过检查（`runtime/cfg.c` 中的 `value_ok`）：对超出设置选项范围的值（开关、范围、名称列表；含有数字的列表是菜单的预设，不做检查），使用启动日志所用的同一种检查；对于取值超出该表所列范围、或遇到错误值会停止游戏的设置，则使用它自己的规则（`fps_limit`、`render_scale`、`window_size`、`output_res`、`app_pool`，以及必须完全匹配的 `lang` 和 `operation_mode`）。不符合的值会被拒绝，保留当前值，日志会写出 `cfg: reload KEY = 'x' rejected: invalid`。启动时，拼错的 `lang` 或 `operation_mode` 会让游戏显示一条消息并停止；重新加载则永远不会。菜单中没有列出的键（绑定、模组列表、金手指）不做检查：它们的读取代码会自行回退到默认值。
- 以下内容不会重新加载，并且每一项都会在 `logs/nx2saria.log` 中写明（`cfg: reload ...`）：由环境变量设置的设置（`NX2SARIA_*`），以及在菜单中更改、但写入文件仍在等待中或被菜单草稿持有的设置。正在运行的游戏版本会忽略的设置会被保存，并记录为 `has no effect on this game version`。
- 当设置菜单中有你尚未应用的更改（草稿、*Keep these display settings?* 倒计时，或 *Apply* 之后的对话框）时，重新加载不会运行，提示会显示 *Close the settings menu to reload nx2saria.ini*。
- 重新加载永远不会写入该文件。在重放错误捕获的运行中（`NX2SARIA_CAPTURE_REPLAY`），捕获中的 `settings.ini` 代替 `nx2saria.ini`，此时重新加载不会运行，提示会说无法读取该文件。

## 重新映射

设置菜单的*操作*标签页在按键布局下方列出每个 Switch 按键以及键盘按键所驱动的八个摇杆方向（移动和镜头），每一项有两个键盘按键格和一个手柄格。每一行的说明会写出该按键在游戏中的作用。选中一个格子，按 A 或 Enter，然后按下新的键盘按键、鼠标按键或手柄按键；按 Esc 或五秒内不按任何键会取消，在键盘按键格上按手柄按键也会取消。Delete、Backspace 或 X 会清除选中的格子。已被其他动作使用的键盘按键会移到新的动作上（说明会写出它离开的那个动作）；手柄按键则与被它替换的按键互换。重新映射后的手柄按键是物理按键：从那以后，正面按键（A B X Y）在两种布局中都保持在原位，直到这些按键按当前布局的方式恢复，或选择了*重置所有按键*。F1、F8、F10、F11 和 Esc 不能绑定（菜单、错误捕获和窗口要使用它们）。标点符号键按其在美式键盘上的位置命名；菜单会显示当前键盘布局在该位置上的字符。

该标签页会修改 `nx2saria.ini` 中的行（这些行会加入上文所说的草稿，并在 Apply 时保存），每个 Switch 按键或摇杆方向一行：`bind_<button> = <entry>, <entry>, ...`。按键为 `A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK`，或者摇杆方向 `LSTICK_LEFT LSTICK_UP LSTICK_RIGHT LSTICK_DOWN`（移动，默认为 W A S D）和 `RSTICK_...`（镜头，方向键）。每个条目是 `pad:<name>` 或 `key:<name>`：

- 手柄名称：`south east west north l r zl zr start back lstick rstick dpad_left dpad_up dpad_right dpad_down lpaddle1 rpaddle1 lpaddle2 rpaddle2`（`zl`、`zr` 是扳机；拨片是 Xbox Elite、DualSense Edge 或 Steam Controller 背面的按键，上面一对为 1，下面一对为 2，默认没有任何 Switch 按键使用它们）
- 键盘按键名称：`a` 到 `z`、`0` 到 `9`、`f2` 到 `f7`、`f9`、`f12`、`num0` 到 `num9`、`space tab enter backspace lshift rshift lctrl rctrl lalt capslock insert delete home end pageup pagedown up down left right`、`num_add num_sub num_mul num_div num_decimal`、`minus equals lbracket rbracket semicolon apostrophe comma period slash backslash grave`、`mouse_l mouse_r mouse_m mouse_x1 mouse_x2`
- `key:default` 和 `pad:default` 保留该部分的默认值（`pad:default` 是每种布局自己的按键）。
- `none` 让该按键不绑定任何东西。

一行会替换该按键的所有默认绑定，包括手柄和键盘，因此请列出你仍想保留的键盘按键，或使用 `key:default`。手柄名称是物理位置，因此一行在两种布局中含义相同，并会替换该布局为这个 Switch 按键分配的按键。摇杆方向只接受键盘按键：手柄的摇杆和鼠标视角始终有效。没有对应行的按键保持上表的绑定。名称不区分大小写。无效的条目会记录一行 `input: bind ...` 并被跳过；没有任何有效条目的行不会改变任何内容。`NX2SARIA_BIND_<button>` 环境变量与其他任何设置的用法相同。修改的行会立即生效（`runtime/input.c` 会在下一次轮询时重新构建这些表）。

报告的正面按键互换了的手柄（A/B 和 X/Y，某些 GuliKit 型号就是这样）可以这样修正：

```
bind_A = pad:south, key:default
bind_B = pad:east, key:default
bind_X = pad:west, key:space
bind_Y = pad:north, key:mouse_l
```

## 鼠标视角

鼠标移动（原始输入）会变成右摇杆的偏移。每个呈现的帧会取自上一帧以来移动的计数，除以帧时间，并把偏移设为满偏移的 `counts per second * NX2SARIA_MOUSE_SENS / 3000`，在该帧内保持，因此转动速度不取决于帧率。这不是鼠标直接视角：游戏的摇杆曲线和死区仍然适用，所以非常缓慢的移动没有任何效果，而快速甩动会达到饱和。当镜头速度不为 1 时（`NX2SARIA_STICK_SENS`），偏移会先除以该系数，再限制在最大倾斜以内：给定移动的转动量保持不变，而甩动所能达到的最快转动是游戏的最高速度乘上该系数。鼠标偏移按“偏移更大者优先”的规则与手柄和方向键竞争。

`NX2SARIA_MOUSE_MODE`（`mouse_mode`，操作标签页中的*鼠标视角*行，实时生效）在这种右摇杆偏移（`stick`）和鼠标直接视角（`direct`，默认）之间选择。在 `direct` 下，镜头的输入更新（`main+0x37c300`）会在游戏的摇杆死区、限幅和响应曲线之后，自己取用原始计数（`recomp/patches-v111.txt`，补丁集 `mouse_direct`；`runtime/mouse_direct.c` 的 `camera_input_oot`）。镜头按鼠标移动的距离转动，两个轴相同：在 `NX2SARIA_MOUSE_SENS` 为 1 时每个计数约 0.08 度，与帧率和 `NX2SARIA_STICK_SENS` 无关，没有死区，也没有最高速度。游戏自己的镜头灵敏度（在其 Options 中）会像缩放摇杆一样缩放鼠标。手柄的右摇杆和方向键仍然经过游戏的响应曲线，并与鼠标叠加。当游戏不接受镜头输入时（它关闭了镜头输入，或者镜头重置后的暂停正在进行），鼠标就像上面那样作为右摇杆。`NX2SARIA_MOUSE_DIRECT_PATCH=0` 会关闭该补丁集，此时无论模式如何，鼠标都作为右摇杆。`NX2SARIA_MOUSE_TRACE=1` 会记录每次镜头输入更新（摇杆、取用的计数以及它们增加的量），并且在 `dlaa` 开启时（DLAA、DLSS 或 FSR 3.1，它们会读取游戏的镜头），还会记录每帧镜头的方向，用于测量每个计数的转动量。

`NX2SARIA_MOUSE_SENS`（*鼠标视角灵敏度*行，实时生效；0 到 10，默认 1；越高转得越快，0 会关闭鼠标视角和光标捕获，该行最低可调到 0.05）。当窗口是前台窗口时，光标会被隐藏并限制在客户区内。在失去焦点、最小化、F11 改变窗口期间、调整大小或移动窗口的过程中，以及文字输入框打开时，光标会被释放。Esc 可以手动释放它：光标显示出来，鼠标视角和鼠标按键停止工作，窗口可以被移动、最小化或关闭。在客户区内点击鼠标会再次捕获光标；这次点击不会传给游戏。点击标题栏或边框来激活窗口不会捕获光标，因此可以马上拖动或关闭窗口；Alt+Tab 或在客户区内点击会像之前一样捕获光标。`NX2SARIA_INPUT=none` 也会禁用捕获。

`NX2SARIA_STICK_DEADZONE`（0 到 50，默认 8）是手柄摇杆的死区，以满偏移的百分比表示，两个摇杆相同。死区是圆形的：摇杆在圆内时读作居中，其余行程会被缩放为从圆的边缘处的 0 开始，因此满偏移仍能达到满值。如果摇杆漂移，请调高它。游戏自己的死区在它之后生效。`NX2SARIA_LSTICK_DEADZONE` 和 `NX2SARIA_RSTICK_DEADZONE`（操作标签页*摇杆*下的*左摇杆死区* / *右摇杆死区*行）各自设置一个摇杆，未设置时回退到 `NX2SARIA_STICK_DEADZONE`，因此磨损的摇杆可以获得比另一个更大的死区。连接手柄时，每一行都会显示其摇杆当前在死区之前的偏移（*当前摇杆*）：松开摇杆，把该行设为比它静止时的值高几个百分点。

摇杆方向位（`StickLLeft` 到 `StickRDown`）由摇杆在 50% 偏移处得出。没有连接手柄时，日志会写出 `input: no gamepad`。

键盘按键会让摇杆倾斜到底，因此只用键盘时 Link 总是在跑。`NX2SARIA_WALK_KEY`（`walk_key`，操作标签页*摇杆*下的*步行键*行）指定一个按键，按住它时，键盘按键给出的摇杆值（移动键和镜头键）会缩放为满值的 `NX2SARIA_WALK_SCALE`（`walk_scale`，*步行幅度*行；0.2 到 0.9，默认 0.51），于是 Link 会走路。该值是 `bind_` 行中的一个按键名称，或者 `none`（默认：关闭）。该行提供 Shift、Alt、Ctrl、Tab 和 Caps Lock，并写出所选按键同时会按下的按键：使用默认绑定时，Shift 是 A，Ctrl 是 B，Tab 是 Plus，而 Alt 和 Caps Lock 是空闲的。手柄的摇杆和鼠标视角不会被缩放。默认值略高于一半，因为一个按键满值的一半比方向位所需的 50% 少一点。两个设置都会在游戏运行时立即生效。

## 在游戏菜单中使用鼠标

在游戏 1.1.1 中，鼠标可以在游戏自身的菜单中使用：标题画面及其存档、暂停菜单及其各页、Options 以及存档对话框。当某个菜单拥有焦点，并且最后使用的是鼠标或键盘按键时，光标会从鼠标视角中释放出来，显示在拥有焦点的按钮上。把鼠标移到某个按钮上会选中它，就像十字键那样（带有游戏的高亮和音效）；在按钮上单击左键相当于在它上面按 A，在任意位置单击右键相当于 B，滚轮每滚动一格相当于十字键的上或下，因此列表移动一行。在暂停菜单中，点击顶部的页面名称会切换到该页，在名称上方滚动滚轮则翻页。拿起手柄会隐藏光标，菜单就和在主机上一样；菜单关闭时，镜头会重新接管鼠标。设置菜单（F1 或 Esc）打开时优先获得鼠标。在超宽画面上，指针会跟随 `wide_menus` 和 `wide_hud` 放置菜单的位置。

`NX2SARIA_MENU_CURSOR`（`menu_cursor`，操作标签页上的*在游戏菜单中使用鼠标*行，立即生效；默认 1）设为 0 时关闭此功能。`NX2SARIA_MENU_CURSOR_LOG=1` 会记录焦点进入的每个菜单画面。日志在启动时有 `menu_mouse: on for 1.1.1 ...`，为最先打开的几个菜单各有一行 `menu_mouse: screen <name>: ...`，并为每个使用过鼠标的菜单写一行，包含焦点移动、点击、滚轮格数和命中测试次数。工作原理：[menu-mouse.md](../menu-mouse.md)（英文）。

## Switch 手柄的 HD 振动

游戏为线性马达生成振动：每一侧有一个低频段和一个高频段，每个频段都有一个频率和一个振幅。在大多数手柄上，这会被简化为两个马达的强度。在 `nx2saria.ini` 中设置 `rumble_hd = 1`（`NX2SARIA_RUMBLE_HD`，操作标签页中的 *HD 振动（Switch）* 行，实时生效，默认关闭）后，Switch 手柄会通过手柄自己的振动报告，按游戏生成的原样得到这些值（`runtime/rumble_hd.c`，`runtime/input.c` 中的 "HD rumble"）：

| 游戏读取的手柄 | 报告发往 | 连接方式 |
|---|---|---|
| Switch Pro Controller（`057e:2009`） | 通过 SDL（`SDL_SendGamepadEffect`） | USB 和蓝牙 |
| 单个 Joy-Con | 通过 SDL；取较强一侧的值，因为它只有一个马达 | 蓝牙，或 USB 充电握把 |
| 组合成一个手柄的左右 Joy-Con | 直接发往两个 Joy-Con 的 HID 设备（`runtime/input_hd_pair.c`），因为 SDL 不接受组合手柄的振动数据 | 蓝牙 |
| 其他任何手柄，包括 Switch 2 的手柄 | 马达，与 `rumble_hd = 0` 时相同 | |

在接管某个手柄时，日志会写出是哪种情况：`input: HD rumble available for this gamepad: a Pro Controller, through SDL (rumble_hd = 1)`，或者 `not available` 并附上原因；对于无法接受 HD 振动的手柄，操作标签页中的该行会显示为灰色。在 `rumble_hd = 0` 时不会向手柄写入任何内容，这一行会说明在开启该设置后才会询问其驱动程序。手柄收到的第一份报告会连同其字节一起记录（`input: HD rumble: first report written (left ..., right ...)`）；被手柄或其驱动程序拒绝的报告也会被记录，之后该手柄会重新通过马达振动。频率会被限制在马达能接受的范围内（低频段 41 到 626 Hz，高频段 82 到 1253 Hz），振幅被限制在 0..1，即马达的安全最大值。`rumble_strength` 会缩放振幅（它的默认值 130 就是游戏原本的强度：默认值高于 100 是为了照顾马达，这条路径会除以它），`rumble_curve` 不适用，`rumble = 0` 也会让这条路径静音。报告最多每 15 ms 写入一次（`rumble_hd_ms`，5 到 50：如果蓝牙手柄在开启 HD 振动时卡顿或断开连接，请调高它），且只写最新的值；保持不变的值每 30 ms 重新写入一次，而在游戏停止发送 250 ms、另一个手柄接管、该设置被关闭以及游戏退出时，会发出中性值。HD 振动开启期间，游戏不会为该手柄发出双马达振动调用。直接读取器上的 DualShock 3 无论如何都保留它的马达振动。

## 冲刺按键

在 1.1.1 上，`NX2SARIA_SPRINT_BUTTON`（`sprint_button`，操作标签页中的 *Sprint button* 行，实时生效）提供一个按键，让 Link 不必先翻滚就能冲刺：`l3`（左摇杆按下，它唯一的其他用途是在对话中隐藏文本框）、`b`（游戏的 B 按键，即 Nintendo 布局下方的正面按键），或 `off`（默认）。`NX2SARIA_SPRINT_MODE`（`sprint_mode`，*Sprint mode* 行）为 `toggle`（默认：按一下开始冲刺，再按一下结束）或 `hold`（按住按键时 Link 冲刺）。当 Link 的输入标志包含冲刺输入时，他就会冲刺；`recomp/patches-v111.txt` 中的补丁集 `sprint` 会在 Link 的移动更新以及随后加载这些标志的姿态更新之后调用 `runtime/sprint.c`，冲刺开启期间，宿主函数会添加冲刺输入，因此运行的是游戏自己的冲刺，带有它的速度、动画和镜头。切换式冲刺会在跳跃、滑行、翻滚和落地中持续，并在 Link 处于其他任何状态时（攻击、使用道具、游泳、攀爬、过场动画）、按住 ZL 时，或者静止站立四个 60 Hz 帧后（如果是在站立时按下、Link 还没移动过，则为九十帧）结束，与帧率无关。在游玩期间（Link 的更新在最近八次控制器轮询内运行过，并且没有对话之类的事件在进行），该按键不会传给游戏，因此不会同时触发它自己的动作；B 的按下只有在 Link 移动时开始才会被拦截，并且一直拦截到松开为止，因此在其他情况下 B 会像平常一样收起武器，游戏也永远不会只收到半次按下。在设置菜单、游戏菜单、对话或过场动画中，每个按键都保持原有作用（L3 隐藏文本框），在那里的按下也不会开始冲刺（B 关闭暂停菜单）。当游戏的 Swap Jump Controls 开启时，B 是跳跃，而 Link 移动时 `b` 冲刺按键会把它拦截下来：这时请使用 `l3`。该补丁集的宿主函数位于补丁页的 +0xf40 和 +0xf30。只有选择了按键时该补丁集才会运行，因此冲刺按键关闭时，它的补丁位置没有任何开销。`NX2SARIA_SPRINT_BUTTON_PATCH=0`（或者一个代码补丁修改了相同位置的模组）会让该补丁集保持关闭，并隐藏这些行。

## 陀螺仪

游戏把手柄的陀螺仪和加速度计当作 Switch 的六轴传感器读取，并用它瞄准弹弓、弓、钩爪和回旋镖。任何 SDL 能识别出陀螺仪和加速度计的手柄都可以：Switch Pro 手柄、一对 Joy-Con（使用右侧 Joy-Con 的传感器）、DualSense 或 DualShock 4。操作标签页中的 *陀螺仪* 分组包含其设置，全部实时生效（`runtime/input.c` `motion_options_refresh`、`runtime/motion_fusion.c`）：

| 设置 | 行 | 取值 |
|---|---|---|
| `motion` | *体感操作* | `1`（默认）或 `0`：传感器读数为静止放置的手柄 |
| `gyro_calibration` | *陀螺仪校准* | `1`（默认）：手柄静止片刻时，就记下陀螺仪的静止读数（即其偏差）并将其减去，使准星不漂移；`0` 按手柄发送的原样传递读数 |
| `gyro_deadzone` | *陀螺仪死区* | 慢于所选档位的转动视为不动：`off`、`low`（默认，每秒 0.002 圈，0.72°/s）、`standard`（0.007）或 `high`（0.01），即主机的三档零漂移级别 |
| `gyro_turn_sens`、`gyro_turn_invert` | *转向灵敏度*、*反转转向* | 绕世界竖直轴转动（即左右瞄准）的百分比（10 到 400，默认 100）；`1` 将其反转 |
| `gyro_tilt_sens`、`gyro_tilt_invert` | *倾斜灵敏度*、*反转倾斜* | 其余转动（即上下瞄准）的百分比（10 到 400，默认 100）；`1` 将其反转 |

连接手柄后，把它放在桌上等处静置一秒：日志随后显示 `motion: bias learned +0.31/-0.12/+0.05 deg/s after 1500 ms still ("Nintendo Switch Pro Controller", calibration 1)`，并且每当手柄静止时都会重新记下偏差。每个手柄各自保存自己的偏差。如果游玩 30 秒后仍未记下任何偏差，日志会显示 `motion: no still moment yet on "...": set the controller down for a second so its gyro bias is learned`。工作原理以及每行日志的含义：[Gyro](../gyro.md)（英文）。

## Nintendo Switch 2 手柄

Windows 不会把 Switch 2 Pro Controller 呈现为 SDL 可以打开的手柄（日志显示 `input: no gamepad`），因此它需要一个桥接程序来唤醒它并创建一个虚拟手柄，例如一个模拟 DualSense 的程序：这样就有按键、振动、陀螺仪和加速度计。日志随后会显示 `input: gamepad "DualSense Wireless Controller" ... (rumble, gyro and accelerometer, ...)` 和 `input: motion sensors on`。输出陀螺仪和加速度计数值始终不变的 DualShock 4 的桥接程序，可以提供按键和振动，但没有体感。一次只运行一个桥接程序；Steam Input 也可能占用该手柄，所以如果桥接程序找不到手柄，请关闭 Steam。连接了多个虚拟和真实手柄时，游戏会跟随最后按下的那个。

## PlayStation 3 手柄

在 `nx2saria.ini` 中设置 `ds3 = 1`（默认关闭）后，当没有驱动程序把 DualShock 3 作为手柄呈现给 SDL 或 XInput 时，Windows 上的 DualShock 3 会被直接读取（`runtime/input_ds3_win.c`）。通过 USB 连接它，或者用一个把它作为带有自己名称 "PLAYSTATION(R)3 Controller" 的 HID 设备暴露出来的蓝牙驱动程序进行配对；日志随后会显示 `input: DualShock 3 opened`，手柄的第一个玩家指示灯亮起，接着出现 `input: DualShock 3 is reporting`。读取器会向手柄发送让它开始报告的命令，因为它自己不会这样做。没有打开的手柄时，它每三秒查找一次，因此之后插入或打开的手柄无需重启即可被识别。

| Switch | DualShock 3（按位置） |
|---|---|
| A、B、X、Y | 圆圈、叉、三角、方块（右、下、上、左：按键布局设置与任何手柄一样适用） |
| ZL、ZR | L2、R2（压力超过 50%） |
| L、R | L1、R1 |
| 左、右摇杆按下 | L3、R3 |
| Plus、Minus | Start、Select |
| 十字键 | 十字键 |

PS 按键不使用。操作标签页像对待其他手柄一样绑定和重新映射它的按键，使用相同的手柄名称。振动通过强度、曲线和 *Vibration* 设置工作：手柄的轻型马达只能开或关，因此当游戏要求的振幅达到十六分之一时它就会启动。加速度计和唯一的陀螺仪轴不使用：游戏的体感操作需要全部三个陀螺仪轴，因此 DualShock 3 不提供体感。`nx2saria.ini` 中的 `ds3 = 0`（默认，实时生效）会关闭该读取器。被驱动程序以通用手柄模式呈现、带有其他名称或报告格式的手柄，不会被该读取器识别，而是像以前一样通过 SDL 或 XInput。

## 脚本输入

`NX2SARIA_INPUT_SCRIPT=<file>` 会重放按键。它与真实输入取“或”，并且在 `NX2SARIA_INPUT=none` 下仍然有效。每行一个事件，`#` 开始注释：

```
<frame> <button> [hold_frames]            # A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK; hold defaults to 4
<frame> LSTICK|RSTICK <x> <y> [hold_frames]   # -32767..32767, up is +y
<frame> POINTER <x> <y>                    # the pointer for the game's menus at x, y of the window (0..1 from top left)
<frame> MOUSEL|MOUSER [hold_frames]        # the left or right mouse button, for the game's menus only
<frame> WHEEL <notches>                    # the mouse wheel for the game's menus, up positive
```

`<frame>` 计算自第一帧以来游戏时间中 60 Hz 的刻数（在 60 fps 时即呈现的帧数）。每个事件记录一次（`input: script frame N tick T: A`）。

`NX2SARIA_INPUT_RECORD=<file>`（`nx2saria.ini` 中的 `input_record`）以相同的格式写入合并后的输入，每次按键和每段摇杆移动一行。该文件可以作为脚本重放，因此可以把它作为复现步骤附在错误报告中。

`tools/run_game.py <game folder> --save <save> --seconds <s> --wall <w> [--script <file>] [--shot <png>]` 会在游戏配置档案的一个副本上运行游戏，换入一个存档，并执行脚本（默认为 `tools/input/load_save.txt`：从标题画面进入第一个存档位）。`<save>` 指定游戏的某个存档备份（`profile/save-backup/<time>`；游戏保留最近三个），或者是来自其他地方的存档文件夹或 `.zip`，例如崩溃报告中压缩的配置档案文件夹；单个存档位（`slot_NN`）只会替换该存档位。运行会在存档载入后经过 `<s>` 秒游戏时间时结束，`--shot` 会把那一帧保存为 PNG，`--env NX2SARIA_SHOTS=w<from>-<to>:<step>` 会在途中每 `<step>` 秒游戏时间保存一张（480x270，设置 `NX2SARIA_SHOTS_FULL=1` 时为完整尺寸）；或者在经过 `<w>` 秒实际时间后结束。`--env NX2SARIA_SHOT_DIR=<dir> --env NX2SARIA_SHOT_AFTER=<s> --env NX2SARIA_SHOT_EVERY=<s>` 则改为按实际时间截图，用于永远到不了游戏时间开始计数之处的脚本。在 Windows 上，它也接受 `build\<ver>` 文件夹以及 `build\save-backup` 中的存档。

`NX2SARIA_TEST_PAD=<lx>,<ly>,<rx>,<ry>` 会在启动时连接一个 SDL 虚拟手柄，其摇杆静止在这些原始 SDL 轴值上（-32768 到 32767，向下为 +y），因此无需手柄就能测试漂移或被按住的摇杆：这些值会像真实手柄一样经过手柄映射和 `NX2SARIA_STICK_DEADZONE`，而 `NX2SARIA_INPUT_RECORD` 会显示到达游戏的内容。`NX2SARIA_TEST_PAD_AT=<ms>` 则改为在输入启动这么久之后连接它，并在一秒后按下它的下方按键，用于测试游玩中打开的手柄。

## RenderDoc 捕获（受阻）

`NX2SARIA_RDOC=<frame>[,...]`（`runtime/rdoc_win.c`；`NX2SARIA_RDOC_DIR`、`NX2SARIA_RDOC_DLL`）会在 `vkCreateInstance` 之前加载 RenderDoc。RenderDoc 会隐藏 `VK_EXT_external_memory_host`，而后端需要它来导入客户机内存池（`runtime/vk_memory.c`），因此在它之下设备创建会失败。要进行捕获，首先需要一个基于复制的内存池后备方案。
