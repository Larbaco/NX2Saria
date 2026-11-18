<!-- translation: lang=zh-Hans source=docs/install.md source-hash=f8e8f3a5886a3ae9be266f1f458b4e4fed167173 reviewed=no -->
<!-- languages -->
[English](../install.md) · [Português (Brasil)](../pt-BR/install.md) · [Русский](../ru/install.md) · **简体中文**

> 本页译自[英文版](../install.md)。如两者有出入，以英文版为准。
>
> 本译文尚未经母语者审校。欢迎在[问题跟踪器](https://github.com/Larbaco/NX2Saria/issues)中提出修改。
<!-- /languages -->
# 安装和更新 NX2Saria

[发布页面](https://github.com/Larbaco/NX2Saria/releases)上的每个版本都有一个 Windows 安装程序和一个 Linux AppImage。两者都不含任何游戏数据：它们都会用你自己的 Nintendo Switch 2 版 `.nup` 转储（带有更新 1.1.1）在你的 PC 上构建游戏。[README](../../README.zh-Hans.md#系统要求)列出了你的 PC 需要满足的条件。

## Windows

1. 从发布版本中下载 **`NX2Saria-Setup-<version>.exe`**。
2. 把它放在你的 `.nup` 转储旁边，然后运行它。
3. 由于安装程序没有签名，Windows SmartScreen 会在首次运行时发出警告：选择 *More info*，然后选择 *Run anyway*。

安装程序会安装启动器，启动器会先构建一次游戏，然后启动它。

## Linux

1. 从发布版本中下载 **`NX2Saria-Linux-<version>-x86_64.AppImage`**。
2. 赋予它可执行权限（`chmod +x NX2Saria-Linux-*.AppImage`），然后运行它。
3. 构建需要你的发行版提供的编译器和几个库。缺少其中任何一个时，AppImage 会指出缺少的是哪个，并给出安装它们的命令。在 Ubuntu 24.04、Linux Mint 22 以及其他基于 Ubuntu 的发行版上：

   ```sh
   sudo apt install clang lld libopus-dev libvpx-dev libdav1d-dev libzstd-dev libvulkan-dev
   ```

   Ubuntu 24.04 没有打包 SDL3，AppImage 自带了它。

在桌面环境中启动时，它会打开一个窗口，用于安装、更新和游玩。

## 更新

- **启动器会自行提供每个新版本**，从 NX2Saria 0.12.1 起。
- **从原始 NX2navi 项目安装的启动器**会保留安装时使用的更新来源。在它的 `data/launcher.json` 中设置一次 `"feed_repo": "Larbaco/NX2Saria"`，此后它就会提供 NX2Saria 的发布版本。
- 更新时，**你的游戏文件夹、存档、配置档案、模组和 `nx2saria.ini` 都会留在原处**。
- **预发布版本**只会推送给 *Update Channel* 设置为 *Preview* 的启动器。在默认的 *Stable* 下，启动器会等到某个版本被标记为稳定版。

## 出了问题？

[已知问题](known-issues.md)列出了我们已知的问题以及避免它们的方法。其他问题请在 https://github.com/Larbaco/NX2Saria/issues 提交问题，并附上游戏文件夹中的 `logs\nx2saria.log`（Linux 上为 `logs/nx2saria.log`）。

[启动器指南](../launcher.md)（英文）和[安装程序指南](../installer.md)（英文）详细介绍了两者的工作方式。
