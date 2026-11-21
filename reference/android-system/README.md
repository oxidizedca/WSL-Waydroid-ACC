# Android 系统镜像与 GPU 开机提醒

[English](#english)

2026-11-20 在 Android 16 的 Waydroid 镜像上验证了 `services.jar` 的可选修正。它位于 Android 系统框架层，按本文流程单独安装。

## GPU 开机通知

`system_server` 在开机阶段 1000 调用 `id.waydro.server.WayDroidService.onBootPhase`。实现位于 `/system/framework/services.jar` 的 `classes3.dex`。这段代码决定是否发布「Waydroid 正在无 GPU 加速模式下运行」通知。

当前 `waydroid.tools_version=1.6.3`，且 `/dev/dma_heap/system` 存在。接下来原版判断 `ro.hardware.egl=angle`、`ro.hardware.vulkan=pastel`。这两个条件在本机同时满足，`ro.waydroid.unsupported_nvidia_kmd` 未设置，按默认值 false 走普通 GPU 警告分支（通知 ID 8）。虽然 `ro.waydroid.software_rendering=0`，原版的这段判断不读取它。现场 `dumpsys SurfaceFlinger` 返回 `GLES: Microsoft Corporation, D3D12 (AMD Radeon RX 7900 XTX), OpenGL ES 3.1 Mesa 26.0.8`，当前活动通知列表为空。[同版 D3D12 离屏验证](../ndk-clean-verification/offscreen.log)

保留的改动只影响开机通知。它将该判断唯一引用的字符串 `pastel` 改为等长的 `paste1`，使检查提前返回。集成工具缺失和 DMA heap 缺失的提醒仍可触发。代价是这段代码也会跳过真实软件回退和不受支持的 NVIDIA 内核驱动提醒。诊断加速状态应查看实际 renderer。更换系统镜像或后端后应重新评估原版判断。

## 用当前镜像复现

以下命令在 WSL 的 Bash 中执行。先进入本 README 所在目录，从只读系统镜像提取原件。生成器只接受本次核过的原版 SHA-256，输出到新文件。它更新 DEX 的 SHA-1、Adler-32 及 ZIP 条目校验，并核验整个 JAR 的 SHA-256。

```bash
set -e
work=$(mktemp -d)
mkdir "$work/image"
sudo mount -t erofs -o loop,ro /var/lib/waydroid/images/system.img "$work/image"
cp "$work/image/system/framework/services.jar" "$work/stock.jar"
sudo umount "$work/image"
python3 make_services_jar.py "$work/stock.jar" "$work/patched.jar"
sha256sum "$work/"*.jar
```

| 文件 | SHA-256 |
|---|---|
| 镜像原版 | `8c5b2d79045de041cde92f83d9949c15a4e7553216aa974f36e273c2da0711d2` |
| 通知补丁版 | `f1f8131ffa032e15eb8195ba3d5205d8f210b2e3f1de068aaa6c571f1faa465c` |

本次从镜像执行上述生成过程，用 `cmp` 确认输出与现场补丁 JAR 逐字节一致。解包对照后，仅 `classes3.dex` 内容有变化。除 DEX 校验字段外，只有偏移 2798184 处的字母 `l` 改成了数字 `1`。

## 在相同镜像上安装和回退

先查看容器实际使用的文件。现场已是目标哈希时，到此即可。

```bash
android=(sudo lxc-attach -P /var/lib/waydroid/lxc -n waydroid --)
"${android[@]}" sha256sum /system/framework/services.jar
"${android[@]}" dumpsys SurfaceFlinger | grep 'GLES:'
```

在本机同款 overlay 布局下，从原版安装时，在前一步的同一 Bash 会话中运行下面的命令。使用停机时的可写层路径，避免修改基础镜像。将当前生效的 JAR 和原件保存在 WSL 的独立安装备份目录，再切换文件。

```bash
source_jar="$work/patched.jar"   # 回退时选 "$work/stock.jar"
active=/var/lib/waydroid/rootfs/system/framework/services.jar
upper=/var/lib/waydroid/overlay_rw/system/system/framework
sudo cmp -s "$active" "$work/stock.jar" || sudo cmp "$active" "$work/patched.jar"
backup=$(sudo mktemp -d /var/lib/waydroid/wsl-waydroid-acc/services.XXXXXXXX)
sudo cp --preserve=all "$active" "$backup/before.jar"
sudo cp "$work/stock.jar" "$backup/stock.jar"
echo "Backup: $backup"
sudo systemctl stop waydroid-container
if findmnt -rn -t overlay -M /var/lib/waydroid/rootfs >/dev/null; then
    sudo waydroid-acc-start
    echo '系统 overlay 仍在挂载，已取消替换。' >&2
    exit 1
fi
sudo mkdir -p "$upper"
tmp=$(sudo mktemp "$upper/.services.XXXXXXXX")
sudo cp --preserve=all "$backup/before.jar" "$tmp"
sudo tee "$tmp" < "$source_jar" >/dev/null
sudo cmp "$source_jar" "$tmp"
sudo mv -fT "$tmp" "$upper/services.jar"
sudo waydroid-acc-start
"${android[@]}" sha256sum /system/framework/services.jar
"${android[@]}" getprop sys.boot_completed
"${android[@]}" cmd notification list
"${android[@]}" dumpsys SurfaceFlinger | grep 'GLES:'
```

回退时使用同一镜像提取的 `stock.jar` 作为 `source_jar`，执行相同替换并核对原版哈希。若因启动失败无法从容器读取生效文件，可以在容器停止、系统 overlay 已卸载时，用此前保存的 `before.jar` 恢复到同一可写层路径，再运行 `sudo waydroid-acc-start`。当前仓库的常规部署不会自动套用这个镜像专用补丁。

## 其他可选模块

[SurfaceFlinger 按需编译着色器：KernelSU 安装与内存、帧时对照](../../system-optimize/README.md)。此模块与上面的 GPU 通知修正相互独立。

---

<a id="english"></a>

# Android image: GPU warning at startup

An optional `services.jar` change was verified in the Android 16 Waydroid image on 2026-11-20. It belongs to the Android framework and is installed separately as described here.

## GPU startup notification

At boot phase 1000, `system_server` calls `id.waydro.server.WayDroidService.onBootPhase` in `classes3.dex` of `/system/framework/services.jar`. This decides whether to post the warning that Waydroid is running without GPU acceleration.

In this image `waydroid.tools_version=1.6.3` and `/dev/dma_heap/system` exists. The stock code next checks `ro.hardware.egl=angle` and `ro.hardware.vulkan=pastel`. Both match. With `ro.waydroid.unsupported_nvidia_kmd` unset (default false), it takes the ordinary GPU warning branch, notification ID 8. It does not read `ro.waydroid.software_rendering=0`. On device, `dumpsys SurfaceFlinger` reported `GLES: Microsoft Corporation, D3D12 (AMD Radeon RX 7900 XTX), OpenGL ES 3.1 Mesa 26.0.8`; the active notification list was empty. [Offscreen D3D12 check on the same build](../ndk-clean-verification/offscreen.log).

The change affects only this boot notification: replace the check's unique string `pastel` with equal-length `paste1` to return early. Warnings for missing integration tools or DMA heap remain. This also suppresses the warning for a genuine software fallback or unsupported NVIDIA kernel driver in this branch. Diagnose acceleration using the actual renderer; re-evaluate the stock logic if the image or backend changes.

## Reproduce from the tested image

In WSL Bash, enter this README's directory and extract the original from the read-only image. The generator accepts only the verified stock SHA-256 and writes a new file. It updates DEX SHA-1, Adler-32 and ZIP entry checksums and validates the complete JAR hash.

```bash
set -e
work=$(mktemp -d)
mkdir "$work/image"
sudo mount -t erofs -o loop,ro /var/lib/waydroid/images/system.img "$work/image"
cp "$work/image/system/framework/services.jar" "$work/stock.jar"
sudo umount "$work/image"
python3 make_services_jar.py "$work/stock.jar" "$work/patched.jar"
sha256sum "$work/"*.jar
```

| File | SHA-256 |
|---|---|
| Stock image | `8c5b2d79045de041cde92f83d9949c15a4e7553216aa974f36e273c2da0711d2` |
| Notification patch | `f1f8131ffa032e15eb8195ba3d5205d8f210b2e3f1de068aaa6c571f1faa465c` |

Generating from the image and comparing with `cmp` gave a byte-identical match to the installed patched JAR. Only `classes3.dex` differed inside the archive. Apart from DEX checksums, the only change was letter `l` to digit `1` at offset 2798184.

## Install or restore on the same image

First inspect the file actually visible in the container. Stop if it already matches the target hash.

```bash
android=(sudo lxc-attach -P /var/lib/waydroid/lxc -n waydroid --)
"${android[@]}" sha256sum /system/framework/services.jar
"${android[@]}" dumpsys SurfaceFlinger | grep 'GLES:'
```

On the tested overlay layout, run the following in the same Bash session when installing from stock. Use the writable layer with the container stopped, not the base image. Save the effective JAR and stock original to the standalone WSL installation's backup directory before switching.

```bash
source_jar="$work/patched.jar"   # to restore, use "$work/stock.jar"
active=/var/lib/waydroid/rootfs/system/framework/services.jar
upper=/var/lib/waydroid/overlay_rw/system/system/framework
sudo cmp -s "$active" "$work/stock.jar" || sudo cmp "$active" "$work/patched.jar"
backup=$(sudo mktemp -d /var/lib/waydroid/wsl-waydroid-acc/services.XXXXXXXX)
sudo cp --preserve=all "$active" "$backup/before.jar"
sudo cp "$work/stock.jar" "$backup/stock.jar"
echo "Backup: $backup"
sudo systemctl stop waydroid-container
if findmnt -rn -t overlay -M /var/lib/waydroid/rootfs >/dev/null; then
    sudo waydroid-acc-start
    echo 'System overlay is still mounted; replacement cancelled.' >&2
    exit 1
fi
sudo mkdir -p "$upper"
tmp=$(sudo mktemp "$upper/.services.XXXXXXXX")
sudo cp --preserve=all "$backup/before.jar" "$tmp"
sudo tee "$tmp" < "$source_jar" >/dev/null
sudo cmp "$source_jar" "$tmp"
sudo mv -fT "$tmp" "$upper/services.jar"
sudo waydroid-acc-start
"${android[@]}" sha256sum /system/framework/services.jar
"${android[@]}" getprop sys.boot_completed
"${android[@]}" cmd notification list
"${android[@]}" dumpsys SurfaceFlinger | grep 'GLES:'
```

To restore stock, set `source_jar` to `stock.jar` extracted from the same image, repeat the replacement and verify the original hash. If Android fails to boot, stop the container, ensure its system overlay is unmounted, restore the saved `before.jar` to the same writable-layer path and run `sudo waydroid-acc-start`. Normal repository deployment does not apply this image-specific change.

## Other optional modules

[SurfaceFlinger on-demand shaders: KernelSU installation, memory and frame times](../../system-optimize/README.md#english). The module is independent of the GPU notification change.
