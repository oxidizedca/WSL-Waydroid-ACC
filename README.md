# WSL-Waydroid-ACC

简体中文 | [English](README.en.md)

## 0. 注意

本项目完全由 deepseek v4.1 flash 1017 与 gpt-6.2 astra 完成, 未经过充分测试, 可能不会长期维护, 更多是抛砖引玉, 希望能有更好的实现

本项目修改了安卓系统库, 可能会被检测, 若由于本项目导致账号封禁, 本人概不负责

## 1. 项目简介

WSL-Waydroid-ACC 是一个面向 **WSL2、x86_64 Waydroid 和 Windows 宿主 GPU** 的 alpha 实验项目。Android 应用照常使用 EGL / OpenGL ES，Mesa 的 D3D12 后端把绘制提交给 Windows 显卡驱动。符合条件的应用图层和物理显示合成结果以共享纹理传递，最终通过 WSLg 中的嵌套 Weston 显示在 Windows 桌面上。

**当前部署基线（2026-11-21）：`built.0L1nxt11`。** 这是固化时本机的 release 名，不是 Git 提交号。`runtime/` 固化了与实机逐项核对的 11 个二进制和 12 个脚本/配置文件；宿主 EGL 与 Gallium 另见 [host 清单](host/SHA256SUMS)。`out/` 只用于后续构建，不作为恢复来源。安装完成后，宿主服务和 Android 容器从各自的安装位置加载运行件，源码和 NDK 只在编译时使用。[基线 SHA-256 清单](runtime/SHA256SUMS)

[可选 KernelSU 模块](system-optimize/README.md) 独立于图形加速基线，默认部署不会安装。可选的 ARM64 应用兼容方案使用 Waydroid overlay，不是 KernelSU 模块，见 [Digitalis 与 MuMu 兼容库](MISC.md#digitalis-arm64)。[Digitalis 预编译包](system-optimize/digitails/README.md)已内含许可证和第三方声明，可单独分发。

## 2. 项目路线

### 一帧如何到达 Windows

1. **在 Android 进程内接入 D3D12。** EGL/GLES 转发库接收 Android 的图形调用。项目的 ELF 加载器处理 Bionic 与 glibc 用户态库之间的符号、重定位和线程局部存储，让 Mesa Gallium 的 D3D12 驱动能够调用 WSL 提供的 DXCore 和 D3D12 库，经 `/dev/dxg` 使用宿主 GPU。[加载器](src/loader_so.c) · [EGL 转发](src/egl_shim.c)
2. **把应用窗口交给 SurfaceFlinger 合成。** 窗口照常从 Android BufferQueue 取得缓冲区。符合尺寸、格式和 usage 条件时，生产者导出 GPU 纹理及就绪信号；宿主输入 broker 按缓冲区 ID 和内容代次管理它们。SurfaceFlinger 的 RenderEngine 能直接采样这一代的纹理，同代重复绑定可复用已导入资源。gralloc 仍负责 Android 缓冲区分配、句柄和布局，供 CPU 消费者使用的镜像按需生成。[窗口发布](src/window.h) · [输入 broker](src/broker.c) · [RenderEngine 输入](src/input.h)
3. **把物理显示的 CLIENT 合成目标交给 Weston。** RenderEngine 将最终画面复制到可共享的 GPU 纹理。HWC 通过项目的 Wayland 协议让嵌套 Weston 导入并采样纹理，成功时省去 GPU 到 CPU 的读回和整帧 `wl_shm` 拷贝。已验证的尺寸和生产者可异步请求导入；首次导入需要确认。失败时保留原有画面并请求 CPU 镜像恢复。[合成目标导出](src/producer.h) · [HWC 提交](src/client-hwc.h) · [Weston 导入](weston/libweston/renderer-gl/texture.c)

### 目前的瓶颈

- **当前 WSL2 图形栈缺少从 Android gralloc 到宿主 GPU 的 dma-buf 直通路径。** Waydroid 可以在 Linux 客体内分配和传递 dma-buf；宿主显卡则通过 `/dev/dxg` 和 D3D12 提供渲染资源。在本机的 dxgkrnl、Mesa D3D12 和 gralloc 组合下，缺少让两端直接导入同一分配的接口。即使把 gralloc 的 fd 传给 Weston，也无法据此采样宿主 GPU 刚画好的纹理。[WSL GPU 虚拟化架构](https://devblogs.microsoft.com/directx/directx-heart-linux/) · [Linux dma-buf 导入与导出](https://docs.kernel.org/6.18/driver-api/dma-buf.html)

  因此，当前实现保留 Android 缓冲区用于排队和归还，另行传递 D3D12 共享纹理。嵌套 Weston 以 `GL_HANDLE_TYPE_OPAQUE_FD_EXT` 导入这份纹理。共享路径命中时可以省去整帧 CPU 读回，但物理 CLIENT 合成目标仍需生成 GPU 快照并等待就绪；CPU 消费者需要像素时仍需生成镜像。[Mesa 纹理导出](mesa/src/mesa/state_tracker/st_cb_readpixels.c) · [Weston 纹理导入](weston/libweston/renderer-gl/texture.c)
- **GPU 快照仍有一次拷贝和同步。** 预分配能提前创建目标纹理，但 RenderEngine 仍需从当前帧缓冲区执行 blit、提交命令并传递就绪信号。此前设置页测试中，创建、提交和处理句柄三段耗时合计的 P50 从 3.19 ms 降至 1.54 ms。[分段计时](reference/RESULT.md)
- **缓冲区归还和跨进程应答仍会拉长尾帧。** 生产者可能等 BufferQueue 归还可用槽位，也可能等 broker 确认可见性。允许 broker 等 GPU 查询时处理其他窗口的请求后，应答 P95 在两批实验中的变化方向不一致。通知栏场景优化全开时，显示帧 P95 分别为 16.45 和 17.68 ms，后一批超过 60 Hz 的单帧预算 16.67 ms。这些测量尚不能把超时单独归因给 broker。[对照数据](reference/display.csv) · [请求排队与应答计时](reference/stages-reverse.csv)
- **共享纹理的覆盖面受缓冲区契约限制。** 格式、stride、usage 或尺寸不符合条件，消费者要求 CPU 读像素，或 Weston 无法导入时，仍可能发生读回、格式转换和共享内存复制。移植到不同镜像或分辨率时，需要重新核对 gralloc 布局、HWC 接口和窗口筛选条件。[窗口筛选](src/window.h) · [HWC 回退](src/adapter.c)

### 当前测试镜像的已知问题

- **关闭锁屏时，冷启动后从桌面打开设置，splash 展开时看似透明。** 这版 LineageOS 23.2 的 Quickstep 在收到首次导航栏位置回调前，把默认值 0 作为任务动画的裁剪底边。在原先关闭锁屏的环境中，仅重启桌面也能复现；展开并收起一次通知栏即可恢复。用户对照：开启滑动锁屏后，冷启动上滑解锁，首次启动动画即正常，无需操作通知栏。此项作为镜像问题记录，不修改 Launcher、SystemUI 或加速层，也不在启动脚本模拟下拉。[实机对照与根因](reference/splash/README.md)

## 3. 测试记录

**2026-11-21 基线核对。** 运行中的 Weston、输入 broker 和纹理 consumer 均映射自同一个 release；按安装文件核对了 Android 图形库、截图工具及宿主服务配置。当前基线保留以下已部署的内存优化和窗口行为，不启用系统调度实验。[固化范围与复核命令](reference/baseline-20261121.md) · [CPU/GPU 调度测量（只读）](system-optimize/results/scheduling/README.md)

**1080p 显示与内存：已合入的优化。** 默认物理显示为 1920×1080，无 Weston 顶栏。设置、滚动及返回桌面的采样中，Weston 成功导入 67 个全尺寸 EGLImage；设置滚动两批的显示帧 P50 为 7.50 / 7.81 ms，P95 为 9.31 / 9.67 ms。以下对照的场景和统计对象不同，不能跨行累加，也不能与 1920×1048 的结果直接作同条件对比。[1080p 显示采样](reference/display-1080.csv)

| 基线改动 | 同条件对照结果 |
|---|---|
| [broker 全量 BLAKE3 校验](reference/shade-1080/README.md) | 保留包括行填充在内的逐字节覆盖；计算器上通知栏展开和收起合计 P95：19.26 → 15.22 ms。单看展开仍是 19.00 ms。 |
| [复用同代 GPU 导入对象](reference/shade-1080/validated-import/README.md) | 仅在 broker 完成本次可写缓冲区校验后复用。展开 P95：19.02 → 16.15 ms；仍有 22/452 帧超出 60 Hz 预算。 |
| [AMD 驱动共享只读页](reference/memory-1080/loader-system/README.md) | 整栈工作集：桌面 4862 → 2977 MiB，依次打开设置、Jelly、计算器后 5884 → 4047 MiB；上述检查点均无 swap。 |
| [回收 ELF 解析映射与 fd，共享 Gallium 只读页](reference/memory-1080/loader-lifetime/deployed/README.md) | 以 AMD 共享已开启为对照，桌面和多应用的整栈工作集分别再少 178.4、217.8 MiB。 |
| [CPU 暂存槽复用](reference/memory-1080/remaining/pool-deployed/README.md) | 多次打开搜索并返回桌面，搜索进程从 8 个槽降至 1 个（63.28 → 7.91 MiB）。四个操作的实测显示帧 P95 未变差。 |
| [libcrypto 与 libstdc++ 共享只读页](reference/memory-1080/remaining/shared-split/README.md) | 多应用下仅开一项分别省 65.05、28.46 MiB，同时开启省 93.55 MiB（12 个图形进程的目标映射按物理页去重，非整机节省）。未测得可稳定归因于某一库的通知栏展开尾帧退化。 |

以下为 2026-11-20 从 Windows、运行中的 WSL 发行版和 Waydroid 容器读取的实机配置。

| 项目 | 实测版本或配置 |
|---|---|
| Windows | Windows 11 专业版 Insider Preview，x64，OS Build `29708.1003` |
| GPU 和 Windows 驱动 | AMD Radeon RX 7900 XTX，显示驱动 `32.0.32029.2010` |
| WSL / WSLg | WSL `3.0.5.0`，发行版运行模式 WSL 2；WSLg `1.0.79` |
| WSL 发行版 | Ubuntu `26.04.1 LTS`（Resolute Raccoon），x86_64 |
| WSL 内核 | `uname -r` 实测 `7.2.8-microsoft-standard-WSL2`，由 `.wslconfig` 指定；`wsl --version` 报告的随附内核为 `6.18.58.1-1` |
| Waydroid | `1.6.3`，容器 vendor 类型 `MAINLINE` |
| Waydroid 系统镜像 | Android 16，LineageOS 23.2，`ro.lineage.version=23.2-20260717-UNOFFICIAL-WayDroidATV-waydroid_x86_64`。OTA 通道为 `a16-qpr2`，system 为 `GAPPS`，安全补丁日期为 `2026-07-01` |
| 实际渲染与显示 | SurfaceFlinger 报告 `D3D12 (AMD Radeon RX 7900 XTX)`、OpenGL ES 3.1、Mesa 26.0.8；嵌套 Weston 14.0.2，当日 Android 物理显示 1920 × 1048；当前基线默认 1920 × 1080 |

**2026-11-20 版本**通过两种尺寸、12 代共享纹理的离屏校验。部署重启后，从 Windows 侧检查了桌面、设置和返回桌面的画面，随后完成手动试用确认。[基线离屏输出](reference/baseline-build.log) · [像素与完成信号校验](validation/check.c)

**性能对照**采自此前的共享纹理优化轮次。两批正反顺序测试中，设置滚动的 SurfaceFlinger 显示帧 P50 从 11.53 / 11.43 ms 降至 6.97 / 7.08 ms。同期 Windows 侧另录了 12 秒、720 帧，覆盖设置滚动、应用切换、通知栏、最近任务和返回桌面；该段逐帧筛查未发现全黑帧或横向黑条。这组帧时和视频对应当时的 1920×1048 配置，不作为当前基线在 1080p 下的逐帧验收。[性能记录与测量口径](reference/RESULT.md) · [60 FPS 录屏](reference/111-60fps.mp4) · [逐帧记录](reference/111-60fps.csv)

### Blue Archive 前台运行时的 Waydroid 资源占用与游戏帧率

2026-11-20 13:13:08 至 13:14:08（UTC+8），在当日的 1048p 版本上连续采样 60 秒。Unity 提交的游戏缓冲区和 Android 显示输出均为 **1920 × 1048**，游戏内部 3D 渲染分辨率未单独测量。整套 Waydroid 的统计范围包括 Android 容器、嵌套 Weston、输入 broker、纹理 consumer，以及 WSL 侧的容器管理、会话和网络辅助进程。Windows 侧 WSLg 客户端和独立显存占用未计入。

| 指标 | 60 秒实测结果 |
|---|---|
| Waydroid 总 CPU | 平均 **138.5%**，即 1.38 个逻辑核，占 WSL 可用 16 核总容量的 **8.65%**。按 1.5 秒采样，峰值为 230.1% |
| Waydroid 总内存 | 含缓存平均 **12.12 GiB**。扣除约 5.01 GiB 非活跃文件缓存后，估算工作集平均 **7.11 GiB**，峰值 7.24 GiB |
| 其中游戏进程 | 平均 CPU **101.9%**，平均内存 PSS **2.21 GiB**（共享页按比例分摊） |
| 游戏图层就绪帧率 | **3,266 帧，平均 54.43 FPS**。每秒帧数的中位数为 56，最低为 8 |
| 游戏帧就绪间隔 | P50 **17.11 ms**，P95 **26.08 ms**，最长 **522.6 ms** |
| SurfaceFlinger 显示合成 | 同期完成 **3,267 帧，平均 54.45 帧/秒** |

采样开始后第 32–33 秒只有 8 个游戏帧就绪，随后一秒为 20 帧，最长的就绪间隔出现在这一段。游戏图层的实际呈现时间戳返回 `-1`，表中游戏 FPS 按去重的缓冲区就绪时间计算，SurfaceFlinger 的完成帧数用于交叉核对。此次采样无法确定这次停顿的原因。[每 1.5 秒的资源与帧数](reference/bluearchive-20261120-131308.csv) · [游戏逐帧就绪时间](reference/bluearchive-20261120-131308-frames.csv)

## 4. 构建方法

### 上游源码与基线 checkout

复现当前基线时，`build.sh` 解包仓库内的 Mesa 和 Weston 源码，再应用 [mesa/](mesa/) 和 [weston/](weston/) 中的本地文件。下表的 Git 提交用于检出上游原版、比对本地改动；实际构建输入以仓库中的源码包和覆盖文件为准。[源码包 SHA-256](vendor/SHA256SUMS) · [构建脚本](build.sh)

| 上游 | 标签与对应提交 | 本项目使用位置 |
|---|---|---|
| Mesa | `mesa-26.0.8` → `60e95b787857afbc9a00b693b91c0d9c8923a430` | Gallium D3D12 驱动、纹理导出和同步。原始源码归档内的 Git 提交与标签一致。[上游提交](https://gitlab.freedesktop.org/mesa/mesa/-/commit/60e95b787857afbc9a00b693b91c0d9c8923a430) |
| Weston | `14.0.2` → `015b3b4d4c05da44a22349ea6e651d1a8f678c59` | 构建修改后的 `libweston-14.so.0`、`gl-renderer.so` 与私有 `wayland-backend.so`。宿主 Weston 软件包版本为 `14.0.2-5`。[上游提交](https://gitlab.freedesktop.org/wayland/weston/-/commit/015b3b4d4c05da44a22349ea6e651d1a8f678c59) |
| BLAKE3（输入 broker） | 发布版 `1.8.2`，归档中的 Git 提交 `df610ddc3b93841ffc59a87e3da659a15910eb46` | 输入 broker 的 CPU 镜像使用 32 字节摘要。构建直接编译仓库内官方 crate 的 C 与 x86-64 汇编实现，无需 Cargo、网络或运行时的 BLAKE3 动态库。归档含上游许可证。[上游提交](https://github.com/BLAKE3-team/BLAKE3/commit/df610ddc3b93841ffc59a87e3da659a15910eb46) |
| libdeflate（Android PNG 编码） | 发布版 [v1.23](https://github.com/ebiggers/libdeflate/tree/v1.23)，源码归档 SHA-256 见 [vendor 清单](vendor/SHA256SUMS) | 仅编译 x86-64 压缩和校验所需的源码，不依赖运行时共享库。[许可证](vendor/libdeflate-COPYING) |
| Microsoft DirectX-Headers | 实际编译使用 `v1.619.1` → `9e393d6d8a3b30dcc6f2806ef604ec16a27b0d7e` 对应的 Ubuntu 包 `1.619.1-1` | Mesa 的 Meson 记录显示通过 pkg-config 选中了系统头文件。Mesa 包内还保留了备用 checkout `v1.618.1`（`64c650b0c6642ab0f3cdf674e38401e02ccd2460`）。[系统版本上游提交](https://github.com/microsoft/DirectX-Headers/commit/9e393d6d8a3b30dcc6f2806ef604ec16a27b0d7e) · [归档版本上游提交](https://github.com/microsoft/DirectX-Headers/commit/64c650b0c6642ab0f3cdf674e38401e02ccd2460) |
| Wayland | `1.24.0` → `736d12ac67c20c60dc406dc49bb06be878501f86` | Android HWC 使用随仓库保存的客户端头文件，核对与 Ubuntu `libwayland-dev 1.24.0-2` 一致。客户端协议代码标注由 `wayland-scanner 1.24.0` 生成；Weston 端在构建时从本项目的 XML 生成。[上游提交](https://gitlab.freedesktop.org/wayland/wayland/-/commit/736d12ac67c20c60dc406dc49bb06be878501f86) · [共享纹理协议](weston/texture.xml) |
| wayland-protocols | `1.47` → `88223018d1b578d0d8869866da66d9608e05f928` | Weston 使用的标准协议集，Meson 检出的 Ubuntu 包为 `1.47-1`。[上游提交](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/commit/88223018d1b578d0d8869866da66d9608e05f928) |
| Android NDK | `r27c`，包修订号 `27.2.12479018` | Android x86_64 工具链及 sysroot，包括 `platform.cpp` 使用的 ANGLE `EGL/Platform.h`。构建无独立 ANGLE 源码 checkout。[NDK 归档校验](toolchains/android-ndk-r27c/SHA256SUMS) |

**Android HAL 与窗口头文件。** `include/hardware/{hardware,gralloc,fb}.h` 和 `include/cutils/native_handle.h` 最初取自 AOSP GitHub 镜像的 `main` 分支，下载时没有固定提交。下表给出 AOSP `android16-qpr2-release` 中可复查的 checkout，所列示例与对应提交中的文件逐字节一致。仓库中的其余兼容头包含本地裁剪，无法据此推定整个 Android 镜像的 repo manifest。

| AOSP 仓库 | 对照提交 | 已核对的仓库内文件示例 |
|---|---|---|
| `platform/hardware/libhardware` | [`67b44bb0f7d1df962e737cfe9baf716bab45792e`](https://android.googlesource.com/platform/hardware/libhardware/+/67b44bb0f7d1df962e737cfe9baf716bab45792e) | [hwcomposer.h](src/hardware/hwcomposer.h)、[hwcomposer_defs.h](src/hardware/hwcomposer_defs.h)、[gralloc.h](include/hardware/gralloc.h) |
| `platform/system/core` | [`e9d1fa3705d7fbd0ac1c942bc4c276161bfbfed1`](https://android.googlesource.com/platform/system/core/+/e9d1fa3705d7fbd0ac1c942bc4c276161bfbfed1) | [native_handle.h](include/cutils/native_handle.h)、[properties.h](include/android_stub/cutils/properties.h) |
| `platform/frameworks/native` | [`fcbde2bcff56ca1d7bec9cabf8bdca5e288bf103`](https://android.googlesource.com/platform/frameworks/native/+/fcbde2bcff56ca1d7bec9cabf8bdca5e288bf103) | [vndk/window.h](include/android_stub/vndk/window.h)、[nativebase.h](include/android_stub/nativebase/nativebase.h) |
| `platform/system/logging` | [`58dac317ea08b8f275ebd01ce864acab2de30b93`](https://android.googlesource.com/platform/system/logging/+/58dac317ea08b8f275ebd01ce864acab2de30b93) | [log.h](include/android_stub/log/log.h)、[log_id.h](include/android_stub/log/log_id.h) |

**运行环境的版本边界。** Waydroid 容器管理程序使用 `1.6.3`（上游提交 `5b7e2e71be3f6bfaaaab3b461251dacaf1ce4991`）。图形适配层在运行时加载测试镜像自带的 `hwcomposer.vendor-base.so`；其独立源码提交未确认，部署脚本按 SHA-256 `942fad47d8372ea1eb6873a8ccad0954d91592b0fc6137d8eff2f638043f1a29` 锁定该二进制。完整 LineageOS 镜像标识见第 3 节。[Waydroid 上游提交](https://github.com/waydroid/waydroid/commit/5b7e2e71be3f6bfaaaab3b461251dacaf1ce4991) · [HAL 加载入口](src/adapter.c) · [部署校验](deploy.sh)

测试机单独构建的 WSL 内核使用 `ZoeHao2026/wsl-kernel-7.2.8` checkout `53bb4a4b310111d020155333d1a378cbc80af869`。其基底为 Linux stable `v7.2.8`，移植的 Microsoft WSL 改动来自 `linux-msft-wsl-6.18.40.1`（`14794180686c2fb6307fbe359c359bec765249f3`）。[内核移植仓库及版本说明](https://github.com/ZoeHao2026/wsl-kernel-7.2.8/tree/53bb4a4b310111d020155333d1a378cbc80af869) · [Microsoft WSL 内核提交](https://github.com/microsoft/WSL2-Linux-Kernel/commit/14794180686c2fb6307fbe359c359bec765249f3)

### 4.1 WSL 内核与设备

`build.sh` 编译用户态组件。离屏 GPU 测试需要可用的 WSL D3D12 设备；启动完整 Waydroid 还需要 Binder、图形缓冲区、容器网络和镜像挂载支持。下表列出复现本机配置时应核对的内核选项。`=y` 表示内建，`=m` 表示按模块提供。

| 用途 | 测试内核的配置 | 验证点 |
|---|---|---|
| 宿主 GPU 通道 | `CONFIG_HYPERV=y`、`CONFIG_HYPERV_VMBUS=y`、`CONFIG_DXGKRNL=y` | WSL 和容器均可访问 `/dev/dxg`，WSL 提供 `/usr/lib/wsl/lib/libd3d12.so` 和 `libdxcore.so` |
| Android IPC 和共享内存 | `CONFIG_ANDROID_BINDER_IPC=y`、`CONFIG_ANDROID_BINDERFS=y`、`CONFIG_MEMFD_CREATE=y` | `/dev/binderfs` 提供 Binder 设备，容器映射出 `/dev/binder`、`/dev/hwbinder` 和 `/dev/vndbinder` |
| 图形缓冲区与同步 | `CONFIG_DMA_SHARED_BUFFER=y`、`CONFIG_DMABUF_HEAPS=y`、`CONFIG_DMABUF_HEAPS_SYSTEM=y`、`CONFIG_SYNC_FILE=y` | `/dev/dma_heap/system` 存在，可分配系统内存缓冲区 |
| 本镜像使用的 DRM 设备 | `CONFIG_DRM=y`、`CONFIG_DRM_VKMS=m`、`CONFIG_DRM_VGEM=m` | 本机加载 `vkms` 和 `vgem`。`gralloc.gbm.device` 指向 VKMS 的 `/dev/dri/card1`，`renderD128` 属于 VGEM；实际 GPU 渲染经 `/dev/dxg` |
| 虚拟多点触屏 | `CONFIG_INPUT=y`、`CONFIG_INPUT_EVDEV=m`、`CONFIG_INPUT_UINPUT=m`（亦可内建） | 模块须匹配当前内核，由触屏服务按需加载，不用写 modules-load.d |
| Android 镜像和可写层 | `CONFIG_BLK_DEV_LOOP=y`、`CONFIG_EROFS_FS=m`、`CONFIG_EXT4_FS=y`、`CONFIG_OVERLAY_FS=y` | 当前 system 镜像使用 EROFS，vendor 使用 ext4，运行时挂载可写 overlay |
| LXC 和容器网络 | `CONFIG_NAMESPACES=y`，启用 `UTS_NS`、`IPC_NS`、`USER_NS`、`PID_NS`、`NET_NS`；`CONFIG_CGROUPS=y`、`CONFIG_CGROUP_BPF=y`、`CONFIG_VETH=y`、`CONFIG_BRIDGE=m`、`CONFIG_BRIDGE_NETFILTER=m`、`CONFIG_NF_CONNTRACK=y`、`CONFIG_NF_NAT=y` | 本机使用 cgroup v2 和 `waydroid0` 网桥 |

自定义内核需保留 WSL 的 Hyper-V 与 dxgkrnl 支持，模块 VHDX 应与启动内核版本匹配。Windows 用户目录的 `.wslconfig` 可在 `[wsl2]` 下通过 `kernel` 和 `kernelModules` 选择这两份文件。使用 `validation/measure.py` 做分段采样时，另需 `CONFIG_FTRACE=y` 和 `CONFIG_UPROBE_EVENTS=y`。[微软 WSL 内核构建说明](https://github.com/microsoft/WSL2-Linux-Kernel#build-instructions) · [WSL 自定义内核配置](https://learn.microsoft.com/en-us/windows/wsl/wsl-config)

可在 WSL 内核对当前配置和设备。`/proc/config.gz` 由 `CONFIG_IKCONFIG_PROC` 提供。

```bash
uname -r
zgrep -E '^CONFIG_(DXGKRNL|ANDROID_BINDER_IPC|ANDROID_BINDERFS|DMABUF_HEAPS_SYSTEM|SYNC_FILE|DRM_VKMS|DRM_VGEM|EROFS_FS)=' /proc/config.gz
ls -l /dev/dxg /dev/dma_heap/system /dev/binderfs /dev/dri
```

### 4.2 Ubuntu 26.04 LTS 编译依赖

在 x86_64 Ubuntu 的 Bash 中安装宿主编译器、Mesa/Weston 的构建工具和开发包。`directx-headers-dev` 来自 Ubuntu 的 `universe` 组件，`libwayland-bin` 提供 `wayland-scanner`。Android 侧使用仓库自带的 NDK r27c（27.2.12479018）。[Ubuntu DirectX-Headers 软件包](https://packages.ubuntu.com/source/resolute/directx-headers) · [构建脚本](build.sh)

```bash
sudo apt update
sudo apt install build-essential binutils meson ninja-build pkgconf bison flex \
  python3 python3-mako python3-packaging python3-yaml directx-headers-dev \
  libdrm-dev libelf-dev libexpat1-dev zlib1g-dev libzstd-dev libdisplay-info-dev \
  libwayland-bin libwayland-dev wayland-protocols libudev-dev libinput-dev \
  libevdev-dev libxkbcommon-dev libxcb1-dev libpixman-1-dev libgbm-dev \
  libegl-dev libgl-dev libgles-dev libpng-dev libcairo2-dev libpango1.0-dev \
  libfontconfig-dev libglib2.0-dev liblcms2-dev libssl-dev libsystemd0
```

### 4.3 编译、离屏验证与部署

在仓库根目录执行。脚本校验 `toolchains/`、`vendor/` 和 `host/` 的清单，把 NDK 与 Mesa/Weston 构建目录解到 WSL 用户缓存，图形产物（含私有 Wayland 窗口后端）、WSL 虚拟触屏程序、Android PNG 编码程序及其 SHA-256 清单写入 `out/`。只编译时运行 `bash build.sh`；加上 `--offscreen` 会用刚构建出的 Gallium 驱动验证共享纹理。

```bash
unset NDK ANDROID_NDK_HOME ANDROID_NDK_ROOT
JOBS=4 bash build.sh --offscreen
(cd out && sha256sum -c SHA256SUMS)
```

部署脚本面向**已配置本项目 Android 图形库及 overlay 的 Waydroid 实例**，需要 WSLg、systemd 和与仓库匹配的 Weston。迁移到其他机器时，应先核对服务模板中的 WSL 用户、镜像的 vendor HAL，以及容器中的 WSL 图形运行库。执行部署会重启 Waydroid 和嵌套 Weston。[部署脚本](deploy.sh) · [宿主服务模板](services/systemd/) · [Android 图形运行库清单](reference/android-libraries.sha256)

| WSL 命令 | 用途 |
|---|---|
| `sudo bash deploy.sh built` | 部署刚编译的 `out/` 和开发中的 `services/`，不改变冻结基线 |
| `sudo bash deploy.sh apply` 或 `sudo bash deploy.sh restore` | 安装或恢复 `runtime/` 及其冻结的脚本/配置；不需要先构建。省略参数等同 `apply` |
| `sudo waydroid-acc-start` | 从独立安装目录启动或重启当前版本；[同名脚本](services/waydroid-acc-start) |

**窗口与分辨率：** `-w` 是 `--weston` 的简写，`-r` 是 `--resolution` 的简写。参数可单独使用，顺序不限。分辨率是 **Android 内容区**，不含 Weston 标题栏、边框或 Windows DPI 换算。每次执行都是重新启动，不是在线调整当前会话；上次的参数不会作为下次默认。

| WSL 命令 | 显示行为 |
|---|---|
| `sudo waydroid-acc-start` | 以 1920×1080 启动，保留 Weston 原有自由调整大小行为 |
| `sudo waydroid-acc-start -r 1600x900` | 以 1600×900 启动，仍可自由调整窗口 |
| `sudo waydroid-acc-start -w fix -r 1280x720` | 固定 1280×720 内容区，保留移动与关闭，普通拖边不改大小 |
| `sudo waydroid-acc-start --weston ratio --resolution 1280x720` | Android 保持 1280×720，外窗可拖动缩放；按内容原比例居中，剩余区域留黑 |

尺寸格式为 `宽x高`，两轴均接受 128–8192 的整数；这不是所有分辨率都通过显卡、游戏或内存压力测试的保证。比例取自 `-r`，不是强制 16:9。固定模式通过 xdg-shell 请求锁定普通窗口；系统强制贴靠和跨屏 DPI 的处理取决于 WSLg。缩放时使用单个 EGL 窗口的 GL 视口，不读回图像；非原生尺寸时整帧重绘，不复用原分辨率的损伤矩形。虚拟触屏的轴范围跟随启动分辨率。非 1080p 不扩大已验证的共享纹理和 PNG 快速路径白名单。冻结的基线也包含自定义 Wayland 后端，并关闭 Weston 顶栏。[窗口与输入验收](MISC.md#4-weston-窗口与输入)

部署会启动一次容器以验证显示栈，成功后关闭 `waydroid-container.service` 的开机自启，不停止当前会话。下次仅打开 WSL 时，容器不会随 `multi-user.target` 拉起 Weston；需要图形界面时运行 `sudo waydroid-acc-start`。

宿主程序和 Weston 模块安装在 `/usr/local/lib/wsl-waydroid-acc/`，Android 运行件部署到 overlay 和容器数据目录。当前镜像另有一项可选的 `services.jar` 修正，用于开机时的 GPU 状态提醒。[独立运行验证](reference/standalone/README.md) · [GPU 开机通知修正](reference/android-system/README.md)

Windows 侧 ADB 连接、虚拟多点触屏及无损 PNG 截图加速，见 [补充修改与测试记录](MISC.md)。

## 开源许可证

本项目的原创部分采用 [GNU 通用公共许可证第 3 版](LICENSE)，或由你选择任何后续版本（`GPL-3.0-or-later`）。第三方组件保留各自的许可证和版权声明，见[第三方许可清单](THIRD_PARTY_NOTICES.md)。
