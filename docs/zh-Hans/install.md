<!-- translation: lang=zh-Hans source=docs/install.md source-hash=31761c484049a7871ae3cc9a97041a6249196ec3 reviewed=no -->
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
2. 赋予它可执行权限，然后运行它。在终端中：`chmod +x NX2Saria-Linux-*.AppImage`。在文件管理器中：右键单击它，
   然后选择 **属性 > 权限 > 允许作为程序执行文件**。在此之前，双击可能会把文件交给其他程序（压缩包管理器或 AppImage
   管理器）打开，而不是运行它。
3. 构建需要你的发行版提供的编译器和几个库。缺少其中任何一个时，AppImage 会指出缺少的是哪个，并给出安装它们的命令。在 Ubuntu 24.04、Linux Mint 22 以及其他基于 Ubuntu 的发行版上：

   ```sh
   sudo apt install clang lld libopus-dev libvpx-dev libdav1d-dev libzstd-dev libvulkan-dev
   ```

   Ubuntu 24.04 没有打包 SDL3，AppImage 自带了它。

首次启动时，AppImage 会将启动器和 Python 运行时安装到 `~/.local/share/nx2saria`，然后打开已安装的启动器。应用菜单中的入口指向这份已安装副本，因此之后可以移动或删除 AppImage。若要修复或重新安装启动器，请保留 AppImage。如果你使用过旧版 AppImage 窗口，新启动器会读取 `~/.config/nx2saria/appimage.json`（以及旧的 `nx2navi` 路径）中记录的转储和游戏文件夹，并原样采用，不会移动这些文件夹。

## 更新

- **启动器会自行提供每个新版本**，从 NX2Saria 0.12.1 起。
- **从原始 NX2navi 项目安装的启动器**会保留安装时使用的更新来源。在它的 `data/launcher.json` 中设置一次 `"feed_repo": "Larbaco/NX2Saria"`，此后它就会提供 NX2Saria 的发布版本。
- 更新时，**你的游戏文件夹、存档、配置档案、模组和 `nx2saria.ini` 都会留在原处**。
- **预发布版本**只会推送给 *Update Channel* 设置为 *Preview* 的启动器。在默认的 *Stable* 下，启动器会等到某个版本被标记为稳定版。

## 验证下载

从 NX2Saria 0.19.0-rc.1 起，每个发布版本的所有文件都由 GitHub Actions 编译并上传，没有任何文件经过任何人的电脑。每个文件都有一份**来源证明**（provenance attestation）：一份由 GitHub 签名的记录，说明是哪个仓库、哪个 workflow、哪个提交编译了它。如果文件在编译后被改动过，它的证明就对不上。

**检查来源证明**：使用 [GitHub CLI](https://cli.github.com/)（`gh`，登录任意 GitHub 账号即可），在你下载文件的文件夹中运行：

```sh
gh attestation verify NX2Saria-Setup-0.19.0.exe --repo Larbaco/NX2Saria \
  --signer-workflow Larbaco/NX2Saria/.github/workflows/release.yml --source-ref refs/tags/v0.19.0 \
  --deny-self-hosted-runners
```

（在 Windows 的 PowerShell 中，请写成一行，并去掉 `\`。）

把 `NX2Saria-Setup-0.19.0.exe` 换成你下载的文件名，并把 `refs/tags/v0.19.0` 中的版本换成它的版本：AppImage、Windows 和 Linux 的 app 与 runtime 压缩包、三个更新清单以及 `SHA256SUMS.txt` 都用同样的方法检查。文件没有问题时，它会显示 `✓ Verification succeeded!` 以及编译它的 workflow。其他任何结果都表示这个文件不是 GitHub Actions 为该版本编译的那个：不要运行它，并在 issue 中告诉我们。

**检查校验和。** 发布页上的 `SHA256SUMS.txt` 列出了其他所有文件的 SHA-256。它说明下载完整无误；再加上 `SHA256SUMS.txt` 本身的来源证明，它也说明了每个文件的来源。

- Linux：`sha256sum -c --ignore-missing SHA256SUMS.txt` 会为你下载的每个文件显示 `OK`。
- Windows（PowerShell）：`Get-FileHash NX2Saria-Setup-0.19.0.exe` 会显示 SHA-256；它必须与 `SHA256SUMS.txt` 中该文件那一行的值相同（不区分大小写）。

每次更新时，启动器都会自行做第二种检查：Windows 会下载 `NX2Saria-update.json`，Linux 会下载 `NX2Saria-update-linux.json`。每个平台只读取自己的清单，并且只有当 app 或 runtime 文件的大小和 SHA-256 与清单一致时才会安装。0.19.0-rc.1 之前的版本是手动上传的，没有来源证明。

## 出了问题？

[已知问题](known-issues.md)列出了我们已知的问题以及避免它们的方法。其他问题请在 https://github.com/Larbaco/NX2Saria/issues 提交问题，并附上游戏文件夹中的 `logs\nx2saria.log`（Linux 上为 `logs/nx2saria.log`）。

[启动器指南](../launcher.md)（英文）详细介绍了它的工作方式。
