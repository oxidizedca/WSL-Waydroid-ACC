# Digitalis：Waydroid overlay 部署

[English](#english)

此目录保存当前实机使用的 **Digitalis Android 16 ARM64 → x86_64 转译层**，目录名沿用本项目的 `digitails`。它不是 KernelSU 模块，不随仓库根目录的图形加速部署脚本安装。部署所需文件均在本目录，不联网，不依赖原来的 WSL 源码工作区。

| 项目 | 内容 |
| --- | --- |
| [构建包](digitalis-prebuilts-2026-11-20-8a2c30cb.tar.gz) | 转译层文件编译自 Digitalis 的 Android 16 分支源码；归档含 74 个运行和配置文件，不含 AOSP 源码。 |
| 版本 | 转译器提交 `8a2c30cb5383f89f8175eacc06ab1065ab7c5b6f`；打包工程提交 `8c47635`。归档内有构建清单和逐文件哈希。 |
| [归档 SHA-256](SHA256SUMS) | `691aba8e8f6e01f64682f6df0072bfe032d7bf16ae7762ea17057c686471f186` |
| [Digitalis 上游许可证](Digitalis-LICENSE) | MuMu 的库不包含在包内，脚本也不会下载、安装或删除它。 |

单独分发时可直接发送上表的压缩包，无需另附声明文件。解压后，`digitalis-prebuilts/` 根目录包含 [Digitalis 许可证](Digitalis-LICENSE)、[第三方声明原文](THIRD-PARTY-NOTICES.txt)和[逐产物 NOTICE 对应表](NOTICE-MAP.tsv)。这些文件不安装进 Android。包内的 `SHA256SUMS` 仍只列原来的 74 项运行和配置文件；`NOTICE.sha256` 单独校验这三份文档。

## 使用

在 WSL Bash 中进入本目录（从仓库根目录执行 `cd system-optimize/digitails`）：

```bash
bash deploy.sh verify        # 校验归档、74 项运行和配置文件及 3 份声明；不需要 root 或运行中的容器
sudo bash deploy.sh check    # 核对正在运行的 Waydroid；只读，不重启
sudo bash deploy.sh install  # 安装。仅当需要变更时备份、停容器并重启
```

若只收到压缩包，可在解压后的 `digitalis-prebuilts/` 目录执行 `sha256sum --strict -c SHA256SUMS NOTICE.sha256` 核对两份清单；要确认归档来源，另与上表独立发布的归档 SHA-256 比对。

**适用范围：** 已配置本项目启动器的 WSL Waydroid，设备 `waydroid_x86_64`，Android 16 QPR2 / SDK 36、构建号 `BP4A.251205.006`，已有 ARM64 ABI、ISA 映射和执行支持。安装前正常启动容器并保存前台应用进度，不要同时运行启动器或其他部署。工具使用 WSL 的 Bash、GNU coreutils/tar、Python 3、systemd、LXC 和 `waydroid-acc-start`；不需要 NDK、编译器或 KernelSU。脚本不负责将纯 x86 镜像转换成支持 ARM64 的镜像。

**安装改什么：** 写入 `/var/lib/waydroid/overlay/` 下的 74 个 Digitalis 文件、该镜像的 `system/etc/init/ndk_translation.rc` 和 `vendor/build.prop`。保留 vendor 的其他属性，不改 ABI 顺序；停用原 NDK 的 ARM64 注册规则，避免与 Digitalis 争用。启动后只核对或切换 `arm64_dyn`、`arm64_exe` 两个已知 Android binfmt 处理器，不清空注册表，不动 `WSLInterop`。由于注册表可能与 WSL 共享，遇到未知或已禁用的同名规则会报错，而不是覆盖。保留启动器记录的窗口模式和分辨率。

**怎样判断生效：** `check` 分别核对 overlay 和容器可见的 74 个文件，检查 Zygote 实际映射 `libberberis_arm64.so`、bridge 属性和两个 ARM64 runner。只看属性或只运行独立 ELF 都不足以证明游戏可启动；应用验证应再从实际游戏进程检查文件视图并冷启动。已验证组合和可选 MuMu 单库见 [MISC.md 第 5 节](../../MISC.md#digitalis-arm64)。已存在 Digitalis KernelSU 模块或有可写 overlay 文件遮挡安装目标时，脚本拒绝安装；不要直接删除整个 overlay。

## 从安装前备份恢复

发生变更前，脚本会打印一个由 root 管理的备份目录，如 `/var/lib/waydroid/digitalis-backups/before-XXXXXXXX`。它保留被覆盖的文件及元数据、哪些目标原来不存在、原 bridge、两个处理器的状态和窗口参数。**恢复目标是本次安装前的状态**，不是固定的“出厂镜像”。已部署且核验一致时直接退出，不重启也不创建空备份。

```bash
# 将最后一个参数替换为 install 实际打印的完整备份路径
sudo bash deploy.sh restore /var/lib/waydroid/digitalis-backups/before-XXXXXXXX
```

若变更开始后安装或重启失败，使用已经打印的恢复命令；恢复不要求容器事先成功启动。不自动删除备份或回滚整个 Waydroid，也不会清应用数据。恢复会撤销清单内 76 个路径自该备份以来的变化；不要把其他实例的备份混用。

---

<a id="english"></a>

# Digitalis: deployment through the Waydroid overlay

This directory contains the **Digitalis Android 16 ARM64 → x86_64 translation layer** used on the test system. Its directory name, `digitails`, follows the project's existing spelling. It is not a KernelSU module and is not installed by the graphics deployment script at the repository root. Everything needed for deployment is here; neither network access nor the original WSL source workspace is required.

| Item | Details |
| --- | --- |
| [Prebuilt archive](digitalis-prebuilts-2026-11-20-8a2c30cb.tar.gz) | Built from Digitalis Android 16 branch sources. Includes 74 runtime and configuration files, not the AOSP source tree. |
| Version | Translator commit `8a2c30cb5383f89f8175eacc06ab1065ab7c5b6f`; packaging project `8c47635`. The archive contains the build manifest and per-file hashes. |
| [Archive SHA-256](SHA256SUMS) | `691aba8e8f6e01f64682f6df0072bfe032d7bf16ae7762ea17057c686471f186` |
| [Digitalis upstream license](Digitalis-LICENSE) | The MuMu library is not included. The script does not download, install or remove it. |

The archive listed above can be distributed on its own. Once extracted, the root of `digitalis-prebuilts/` contains [Digitalis license](Digitalis-LICENSE), [Third-party notice texts](THIRD-PARTY-NOTICES.txt) and [Per-artifact notice map](NOTICE-MAP.tsv); these documents are not installed into Android. The bundled `SHA256SUMS` still covers only the original 74 runtime and configuration files. `NOTICE.sha256` separately verifies the three documents.

## Usage

From the repository root in WSL Bash, run `cd system-optimize/digitails` to enter this directory:

```bash
bash deploy.sh verify        # Verify archive, 74 payloads and 3 notices; no root or running container needed.
sudo bash deploy.sh check    # Check the running Waydroid instance; read-only, no restart.
sudo bash deploy.sh install  # Install; back up and restart only if changes are needed.
```

If you received only the archive, run `sha256sum --strict -c SHA256SUMS NOTICE.sha256` in the extracted `digitalis-prebuilts/` directory to verify both manifests. To check the archive itself against an independently published value, compare its SHA-256 with the table above.

**Requirements:** WSL Waydroid with this project's launcher already configured; device `waydroid_x86_64`, Android 16 QPR2 / SDK 36, build `BP4A.251205.006`, with ARM64 ABI, ISA mapping and execution support already in place. Start the container normally and save foreground app progress before installing. Do not run the launcher or another deployment at the same time. The script uses WSL Bash, GNU coreutils/tar, Python 3, systemd, LXC and `waydroid-acc-start`; it does not require the NDK, a compiler or KernelSU. It does not add ARM64 support to an x86-only image.

**What changes:** The installer writes 74 Digitalis files under `/var/lib/waydroid/overlay/`, plus the image's `system/etc/init/ndk_translation.rc` and `vendor/build.prop`. It preserves other vendor properties and ABI ordering, and disables the original NDK ARM64 registration rules to avoid a conflict. After startup it checks or switches only the two known Android binfmt handlers, `arm64_dyn` and `arm64_exe`; it neither clears the registry nor changes `WSLInterop`. Since WSL may share this registry, the installer stops rather than overwriting an unknown or disabled rule with the same name. The launcher's recorded window mode and resolution are preserved.

**Verification:** `check` verifies the 74 files in both the overlay and the running container, checks that Zygote actually maps `libberberis_arm64.so`, and checks the bridge property and both ARM64 runners. Neither a property nor a standalone ELF test proves a game can launch. To verify an app, cold-start it and check the files visible to its actual process. See [MISC section 5](../../MISC.md#digitalis-arm64-en) for the tested combination and the optional single MuMu library. Installation refuses to proceed if a Digitalis KernelSU module is already present or a writable overlay file masks a target. Do not delete the whole overlay.

## Restoring the pre-installation backup

Before changing anything, the script prints the path of a root-managed backup, such as `/var/lib/waydroid/digitalis-backups/before-XXXXXXXX`. It records the previous files and metadata, previously absent targets, the original bridge, both handlers' states, and window parameters. **Restore returns to the state immediately before that installation**, not to a fixed factory image. If the installed files already match and pass verification, the script exits without restarting or creating an empty backup.

```bash
# Replace the last argument with the full backup path printed by install.
sudo bash deploy.sh restore /var/lib/waydroid/digitalis-backups/before-XXXXXXXX
```

If installation or restart fails after changes begin, run the restore command already printed. The container need not start successfully first. The script does not remove backups automatically, roll back the whole Waydroid installation, or clear app data. Restoring reverts changes made since that backup to the 76 listed paths; never mix backups from different instances.
