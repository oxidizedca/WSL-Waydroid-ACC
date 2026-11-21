# 安装目录独立运行验证

[English](#english)

**结论：已部署的图形栈不从源码目录加载运行件。** 宿主 broker、consumer、EGL、Gallium 及 Weston 模块从独立安装目录加载；Android EGL、GLES、Gallium 和 HWC 从容器部署位置加载。编译环境只用于重建。当前组件范围及校验方式见[基线清单](../baseline-20261121.md)。

2026-11-20 的独立性验证用 systemd 的 `InaccessiblePaths` 对三个宿主服务屏蔽开发目录，重启服务并从 `/` 执行 `/usr/local/sbin/waydroid-acc-start`。逐个确认服务挂载命名空间无法读取源码；Android 完成启动，设置、计算器均报告 `Status: ok`。当次物理显示为 1920×1048，不是当前默认尺寸。该验证针对服务及容器重启，不代表全新机器安装测试。

[重启输出](restart.log) · [桌面](home.jpg) · [设置](settings.jpg) · [计算器](calculator.jpg) · [返回桌面](home-final.jpg)

---

<a id="english"></a>

# Running independently of the source tree

**The deployed graphics stack does not load runtime files from the source directory.** The host broker, consumer, EGL, Gallium and Weston modules load from the standalone installation; Android EGL, GLES, Gallium and HWC load from their deployed container paths. The build environment is only needed to rebuild. See [Baseline manifest](../baseline-20261121.md#english) for the current component list and verification procedure.

On 2026-11-20 we used systemd's `InaccessiblePaths` to hide the development directory from three host services, restarted them, and ran `/usr/local/sbin/waydroid-acc-start` from `/`. We checked that each service's mount namespace could not read the source. Android finished booting; Settings and Calculator both reported `Status: ok`. The physical display was 1920×1048 in this run, not the current default. This verifies service and container restarts, not installation on a clean machine.

[Restart output](restart.log) · [Home](home.jpg) · [Settings](settings.jpg) · [Calculator](calculator.jpg) · [Return Home](home-final.jpg)
