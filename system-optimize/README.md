# Waydroid 可选系统组件

[English](#english)

这里存放可单独安装的 Android 系统侧组件。KernelSU 模块与独立 overlay 部署的回退方式不同，分别见下文。它们不是硬件加速的前置条件，不随仓库的构建、部署或 WSL 自启动而启用。后台进程的保留、冻结和回收沿用原镜像策略。

## 独立 overlay 部署（非 KernelSU）

[Digitalis ARM64 转译层：构建包、部署与恢复](digitails/README.md)。适用于项目验证过的 Android 16 QPR2 x86_64 Waydroid，不随图形加速层或下列模块部署。

## 现有模块

| 模块 | 用途与验证状态 | 文件 |
| --- | --- | --- |
| SurfaceFlinger 按需编译着色器（0.1 alpha） | 当前 1080p 镜像上，设置和计算器切换后 SF 的 PSS 少约 168 MiB。首次转场 P99 可能变慢；没有关闭应用自己的着色器编译。 | [模块源码](modules/sf-demand-shaders/) · [KernelSU 安装包](dist/waydroid-acc-sf-demand-shaders-0.1.zip) · [对照结果和原始数据](results/sf-prime/README.md) |

SurfaceFlinger 模块只设一个非持久属性：`service.sf.prime_shader_cache=false`。不替换系统文件，不需要挂载模块，也不会在启动后强制重启 SurfaceFlinger。已用 KernelSU 3.3.1 在 Waydroid x86_64、Android 16 上验证启用与禁用。安装时会校验设备、SDK 和实测 SurfaceFlinger 的 SHA-256；镜像升级前请先禁用，避免绕过仅在安装时执行的版本检查。

## SurfaceFlinger 模块：安装、验证与禁用

1. 在 KernelSU 管理器选择上面的 ZIP 安装；已经安装的可直接启用。**完整重启 Waydroid 容器**，无需重启 WSL。切换前先保存前台应用进度。
2. 在 Android shell 执行 `getprop service.sf.prime_shader_cache`。启用后应返回 `false`。要核实实际收益，可在同一桌面运行 `dumpsys meminfo surfaceflinger`，对比总 PSS 和 Native Heap；只读到属性不代表本次 SF 启动时用到了它。
3. 在管理器禁用或卸载模块，再完整重启容器。本测试镜像的属性恢复为空，SF 按默认逻辑预热。

## SurfaceFlinger 模块：从源码打包

在本目录执行（WSL Bash、Python 3，不需要 NDK）。ZIP 根直接包含三个模块文件；测量记录和 README 不打进安装包。

```bash
mkdir -p dist
(cd modules/sf-demand-shaders && python3 -m zipfile -c ../../dist/waydroid-acc-sf-demand-shaders-0.1.zip module.prop system.prop customize.sh)
python3 -m zipfile -t dist/waydroid-acc-sf-demand-shaders-0.1.zip
```

源码在 `modules/`，可安装产物在 `dist/`，单项分析和验证数据在 `results/`。仅有采样结果的方向不列为可用模块。

## 未打包的方向

[游戏全屏图层与静态缓存：采样和不启用原因](results/fullscreen-composition/README.md)。

[CPU/GPU 调度：前台线程等待与 WSL GPU 队列能力](results/scheduling/README.md)。

---

<a id="english"></a>

# Optional Waydroid system components

This directory contains Android system components that can be installed independently. KernelSU modules and the standalone overlay use different restore procedures, described below. Neither is required for hardware acceleration, and neither is enabled by the repository build, deployment, or WSL startup. Background-process retention, freezing and reclamation follow the stock image's policy.

## Standalone overlay deployment (not KernelSU)

[Digitalis ARM64 translation: archive, installation and restore](digitails/README.md#english). This targets the project's tested Android 16 QPR2 x86_64 Waydroid image and is not deployed with the graphics layer or the modules below.

## Available modules

| Module | Purpose and validation | File |
| --- | --- | --- |
| SurfaceFlinger on-demand shaders (0.1 alpha) | SF PSS was about 168 MiB lower after switching between Settings and Calculator on this 1080p image. First-transition P99 can worsen. Application shader compilation remains enabled. | [Module sources](modules/sf-demand-shaders/) · [KernelSU ZIP](dist/waydroid-acc-sf-demand-shaders-0.1.zip) · [Results and raw data](results/sf-prime/README.md#english) |

The SurfaceFlinger module sets one non-persistent property: `service.sf.prime_shader_cache=false`. It does not replace system files, require a module mount, or force a SurfaceFlinger restart after boot. Enabling and disabling it were verified with KernelSU 3.3.1 on Waydroid x86_64, Android 16. Installation checks the device, SDK and SHA-256 of the tested SurfaceFlinger binary. Disable the module before updating the image: the version checks run only at installation.

## SurfaceFlinger module: install, verify and disable

1. Install the ZIP above in KernelSU Manager, or enable the module if already installed. **Fully restart the Waydroid container**; WSL need not restart. Save foreground app progress before switching.
2. In an Android shell, run `getprop service.sf.prime_shader_cache`. It should return `false` when enabled. To verify the actual benefit, run `dumpsys meminfo surfaceflinger` on the same Home screen and compare total PSS and Native Heap. The property alone does not prove that this SurfaceFlinger instance used it at startup.
3. Disable or uninstall the module in the manager, then fully restart the container. On the tested image the property becomes empty and SurfaceFlinger returns to its default warm-up behavior.

## SurfaceFlinger module: package from source

Run the following in this directory (WSL Bash and Python 3; no NDK required). The ZIP contains the three module files at its root. Measurements and the README are not included in the installation package.

```bash
mkdir -p dist
(cd modules/sf-demand-shaders && python3 -m zipfile -c ../../dist/waydroid-acc-sf-demand-shaders-0.1.zip module.prop system.prop customize.sh)
python3 -m zipfile -t dist/waydroid-acc-sf-demand-shaders-0.1.zip
```

Source is in `modules/`, installable artifacts in `dist/`, and individual analyses and validation data in `results/`. Investigations supported only by measurements are not listed as available modules.

## Investigations not packaged as modules

[Fullscreen layers and static caching: evidence and why caching is not enabled](results/fullscreen-composition/README.md#english).

[CPU/GPU scheduling: foreground waits and WSL GPU queue capabilities](results/scheduling/README.md#english).
