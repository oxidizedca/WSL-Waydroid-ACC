# 离线部署 / Offline deployment

从仓库根目录运行下列命令。也可以单独复制对应的整个部署目录，再从中运行 `bash deploy.sh …`；两个包可各自离线校验，运行包不会自动安装 Digitalis。Digitalis 的安装和恢复需要已配置的 Waydroid-ACC 环境及已安装的启动命令。若从 Git 克隆，请先用 Git LFS 取回实际归档。

| 组件 | 只校验，不安装 | 安装（会中断图形会话） |
| --- | --- | --- |
| WSL Waydroid 运行包（GPU 加速、窗口与输入、ADB 与截图） | `bash deploy/wsl-waydroid-acc/deploy.sh verify` | `sudo bash deploy/wsl-waydroid-acc/deploy.sh apply` |
| Digitalis 转译层 | `bash deploy/digitails/deploy.sh verify` | `sudo bash deploy/digitails/deploy.sh install` |

预编译包按 `<项目>-prebuilts-<日期>.tar.gz` 命名。同一天更新的包用 SHA-256 区分。

运行包内含当前版本的运行文件、宿主 EGL/Gallium 和许可证。部署脚本先校验压缩包，再在临时目录解压并逐项校验；`restore` 重新安装该预编译包中的版本。仅开发模式 `built` 需要完整仓库和已编译的 `out/build/`。维护者更新当前版本的校验清单后可运行 `bash deploy/wsl-waydroid-acc/package.sh` 重新打包。环境要求见[主文档](../README.md)。

Digitalis 部署时请保留同目录的 `binfmt.py`、校验清单和原始压缩包。支持已配置 ARM64 的 Waydroid Android 16 QPR2 测试镜像（`BP4A.251205.006`）。如需回退，在 `deploy/digitails` 下运行 `sudo bash deploy.sh restore <备份目录>`，使用安装脚本打印的完整路径，恢复到本次安装前的状态。[转译器补丁说明](../system-optimize/digitails/README.md)。

## English

Run the commands in the table below from the repository root. Alternatively, copy either deployment directory on its own and run its local `deploy.sh`. Either package can be verified offline on its own; installing the runtime does not install Digitalis. Digitalis installation and restore require a configured Waydroid-ACC environment and its installed launcher. For a Git checkout, fetch the actual archives with Git LFS first.

| Component | Verify only | Install (interrupts the graphics session) |
| --- | --- | --- |
| WSL Waydroid runtime (GPU acceleration, windowing, input, ADB and screenshots) | `bash deploy/wsl-waydroid-acc/deploy.sh verify` | `sudo bash deploy/wsl-waydroid-acc/deploy.sh apply` |
| Digitalis translator | `bash deploy/digitails/deploy.sh verify` | `sudo bash deploy/digitails/deploy.sh install` |

Prebuilt archives use `<project>-prebuilts-<date>.tar.gz`. Use SHA-256 to distinguish archives updated on the same day.

The runtime archive contains the current runtime, host EGL/Gallium and licenses. The installer verifies the archive, extracts it into temporary storage, and verifies the individual payloads. Use `restore` to reinstall the version in that archive. Only development mode (`built`) requires the full checkout and completed build output. Maintainers can regenerate the archive with `bash deploy/wsl-waydroid-acc/package.sh` after updating the checksum manifests for the current version. See the [main README](../README.en.md) for prerequisites.

Keep the Digitalis archive, checksum, installer and `binfmt.py` together. The tested target is an ARM64-enabled Waydroid Android 16 QPR2 image (`BP4A.251205.006`). To restore the pre-installation state, run `sudo bash deploy.sh restore <backup-directory>` from `deploy/digitails`, using the full backup path printed by the installer. See the [translator patch notes](../system-optimize/digitails/README.md#english).
