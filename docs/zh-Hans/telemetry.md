<!-- translation: lang=zh-Hans source=docs/telemetry.md source-hash=8591ed12455f02a6ea746c22c0412a34e2522815 reviewed=no -->
<!-- languages -->
[English](../telemetry.md) · [Português (Brasil)](../pt-BR/telemetry.md) · [Русский](../ru/telemetry.md) · **简体中文**

> 本页译自[英文版](../telemetry.md)。如两者有出入，以英文版为准。
>
> 本译文尚未经母语者审校。欢迎在[问题跟踪器](https://github.com/Larbaco/NX2Saria/issues)中提出修改。
<!-- /languages -->
# 会话报告与评论

启动器可以向本项目自己的服务器发送两样东西，而且只在你同意时发送：

- **会话报告**：在你游玩之后，一份关于游戏在你的电脑上运行情况的简短摘要。在你用 *Send* 回答启动器的提问之前，它是关闭的。
- **评论**：你在 *Send a Comment* 中写下的文字，在你按下 *Send* 时发送。

在你做出选择之前，什么都不会发送。你第一次从游戏返回时，启动器会询问一次，提供 *Send* 和 *Don't Send*，而 *See What Is Sent* 会显示将要发送的确切文本。你可以随时在 **Settings → Session Reports** 中更改答案，在你回答之前，那里显示 *Not Asked Yet*。

没有服务器地址构建的启动器不会显示这些内容：没有提问，没有 Session Reports 界面，也没有 *Send a Comment*。

服务器是本项目在 Cloudflare Workers 上运行的一个小程序，使用 Cloudflare D1 数据库。它的全部源码都在仓库中：[`telemetry/`](../../telemetry/)（英文）。不使用任何其他人的分析服务。

## 会话报告包含什么

游戏结束时会把它写入游戏文件夹中的 `logs/session-summary.json`，因此你可以在发送任何内容之前阅读它。报告只有以下字段，没有其他内容；服务器会拒绝带有任何其他字段的报告。

| 字段 | 含义 | 示例 |
|---|---|---|
| `schema` | 格式的名称和版本 | `nx2saria.session/1` |
| `version` | NX2Saria 的版本 | `0.19.0` |
| `game_version` | 游戏的版本 | `1.1.1` |
| `os.family`、`os.name`、`os.wine` | Windows 还是 Linux，系统的名称和版本号，是否在 Wine 下运行 | `windows`、`Windows 11 (build 26100)`、`false` |
| `cpu.name`、`cpu.threads` | 处理器的型号名称及其线程数 | `AMD Ryzen 5 5600 6-Core Processor`、`12` |
| `ram_mb` | 电脑的内存，以 MB 为单位 | `16384` |
| `gpu.name`、`gpu.vendor`、`gpu.device`、`gpu.driver`、`gpu.vram_mb` | 显卡的型号及其 PCI ID，驱动程序的名称和版本，显卡的显存 | `NVIDIA GeForce RTX 3060`、`0x10de`、`0x2504`、`NVIDIA 581.57`、`12288` |
| `display.output`、`display.render_scale`、`display.fullscreen`、`display.game_aspect`、`display.picture` | 输出分辨率、渲染比例、全屏还是窗口、画面比例设置，以及游戏绘制的画面尺寸 | `1920x1080`、`game`、`true`、`16:10`、`2304x1440` |
| `settings.preset`、`settings.upscaler`、`settings.taa`、`settings.dlss`、`settings.framegen`、`settings.fps_limit`、`settings.vk_present` | 正在使用的画质预设（`low`、`medium`、`high`、`ultra`，若你改过它的某项设置则为 `custom`）以及主要的图形选项。不属于该选项自身可选值的值会以 `other` 发送 | `high`、`fsr`、`dlss`、`dlaa`、`off`、`60`、`fifo` |
| `session.duration_s`、`session.frames`、`session.end` | 游戏运行了多长时间，显示了多少帧，以何种方式结束：`normal`、`fatal`、`device_lost` 或 `crash` | `1834`、`109800`、`normal` |
| `frametime.fps_mean`、`frametime.fps_low_1pct`、`frametime.worst_ms`、`frametime.over_25ms`、`frametime.over_100ms` | 整个会话的平均帧率、其 1% low、最慢的一帧，以及有多少帧耗时超过 25 ms 和超过 100 ms | `59.9`、`52.1`、`88.4`、`14`、`0` |
| `startup.shader_prewarm_ms`、`startup.shaders_translated`、`startup.pipelines_built`、`startup.pipeline_build_ms` | 启动时准备着色器和管线所花的时间（如果有的话） | `5400`、`120`、`900`、`3100` |
| `fatal` | 会话因错误结束时游戏的错误行，其中你的文件夹像日志中一样以简写形式写出（[日志隐私](../log-privacy.md)（英文）），最多 300 个字符 | `null` |

游戏不知道的字段为 `null`。如果游戏崩溃得太严重而无法写入报告，启动器会根据日志中的错误行生成一份：只有 `version`、`game_version`、`os.family`、`session.end`（`crash`）和 `fatal`，其他所有字段都为 `null`。

启动器会随报告一起发送：

- `install_id`：启动器在第一次需要时生成的一个随机数，保存在它的设置中。它不是根据你电脑上的任何东西生成的；它只是让同一个安装的报告能被一起统计、一起删除。
- `launcher_version`：启动器的版本。

服务器会加上报告来自的**国家**，用两个字母表示（`BR`、`DE`），这是 Cloudflare 为每个请求提供的，以及报告到达的时间。

## 评论包含什么

- 你的文字，最多 4000 个字符，**与你写的完全一样**。不要写入你的电子邮件、姓名或其他任何你不想发送的内容。
- 你上一次会话的报告（如上所述），如果有的话；**Session Reports** 设为 *Off* 时不附带。
- 如果你勾选 *Attach the Last Log Lines*：`logs/nx2saria.log` 的最后 200 行，最多 24 KB，其中你的主文件夹、账户名和计算机名像日志中一样以简写形式写出（[日志隐私](../log-privacy.md)（英文））。这些行中的其他文件夹名，例如游戏的安装位置，保持原样。
- 同样的 `install_id`、`launcher_version`、国家和时间。

发送评论不需要打开会话报告：按下 *Send* 就是你的选择。

## 不会发送或不会保存的内容

- **不会保存你的 IP 地址**。与服务器的每个连接都带有它，因此服务器能看到它：服务器在那一刻于内存中使用它来限制滥用（同一地址每分钟最多 20 个请求），并把它作为国家信息的来源，但从不把它写下来。运行服务器的 Cloudflare 会像对待任何网站一样处理这个连接。
- 不发送任何文件：没有存档，没有设置文件，没有完整的日志，没有游戏文件，没有密钥。
- 会话报告不包含账户名、计算机名、文件夹名、电子邮件或硬件序列号：服务器会拒绝带有格式中不存在的字段的报告。评论包含你写的内容，以及在你附加时如上所述的日志行。

## 保存多久

报告和评论在满 180 天后由每日清理删除。只有项目的维护者可以通过一个受密钥保护的页面阅读它们：按版本、显卡、系统和选项的统计数、帧率、最常见的错误以及评论。

## 删除你的数据

**Settings → Session Reports → Delete My Data** 会从服务器删除所有以你的 `install_id` 发送的报告和评论，然后生成一个新的 `install_id`。无论会话报告是开还是关，它都有效。无论服务器是否保存了任何内容，它都给出同样的回答。数据库服务会在有限的时间内保留自己的恢复历史（在 Cloudflare 上最多 30 天）；随着这段历史过期，已删除的行也会从中消失。

## 关闭

把 **Settings → Session Reports** 设为 *Off*。如果项目将来停止收集，服务器会如此回答，启动器在下一个版本之前都会停止发送。

## 已知限制

- 从游戏的设置菜单重启游戏会启动一个新进程，它会在启动器发送第一个进程的报告之前将其删除：只会发送最后一个进程的报告。
- 写评论需要键盘（或屏幕键盘，例如 Steam 的屏幕键盘）；其余界面都可以用手柄操作。
