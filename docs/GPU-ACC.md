# GPU 加速：从 Android 图形调用到 Windows 窗口

[项目首页](../README.md) · [English](#english)

这套实现解决两个不同的问题：**让 Android 进程能用 Windows GPU 画图**，以及**让画好的像素交给下一个进程时尽量不经过 CPU**。前者靠 Android 进程内的 EGL/GLES 转接层和 glibc 库加载器；后者靠修改过的 Mesa、Waydroid 合成 HAL 和嵌套 Weston。只接通 D3D12，不等于应用、SurfaceFlinger 和 Weston 就能共用同一块显存。

## 1. 原有系统提供什么，项目改了什么

| 边界 | 沿用的能力 | 本项目的改动 |
|---|---|---|
| Windows 与 WSL2 内核 | Windows 显卡驱动、WSL 的 DXCore/D3D12 用户态库和内核设备 `/dev/dxg` 已提供宿主 GPU 通道。WSLg 可显示 Linux Wayland 客户端。 | 在测试内核中配齐 dxgkrnl、Binder、dma-buf heap 和同步支持；运行包构建的是用户态组件，不编译 Windows 驱动，也不向 WSLg 安装补丁。 |
| Android 应用与图形驱动之间 | 应用、HWUI 和游戏仍使用 Android 的 EGL/GLES 与原生窗口接口。 | 在镜像的 64 位图形驱动入口安装 EGL/GLES 转接库，在调用者自己的进程中加载并调用面向 glibc 的 Mesa D3D12。 |
| 应用与 SurfaceFlinger 之间 | gralloc 分配缓冲区；BufferQueue 管理槽位、排队和归还；SurfaceFlinger 决定图层的合成。 | 在合格窗口的缓冲区之外发布一份 GPU 纹理；在 RenderEngine 使用的 EGLImage 路径导入它。需要 CPU 像素时仍提供 gralloc 镜像。 |
| SurfaceFlinger 与桌面之间 | 镜像自带的 Waydroid HWC 管理 Wayland 表面和提交；WSLg 管理最外层窗口。 | 包装原有 HWC，增加共享纹理提交与恢复；在 WSL 发行版内另起一个带私有协议和 GL 导入支持的 Weston，作为 WSLg 的客户端。 |

**适用前提。** 这是对已配置的 x86_64 Waydroid 实例的改造，不是一键把原版 WSL 或任意 Android 镜像变成 GPU 镜像。测试机的 VKMS、VGEM 和 dma-heap 用于 Android 设备及缓冲区配置；出现 `/dev/dri/card1` 或 `renderD128` 不表示游戏通过它们驱动 Radeon，实际 D3D12 渲染走 `/dev/dxg`。部署器预期已有匹配的 vendor HAL、平台库和容器内可用的 D3D12 与显卡用户态驱动。Digitalis 与可选的 KernelSU 模块都不是这条图形路径的依赖。[测试内核与设备](../README.md#51-wsl-内核与设备) · [部署器及其前提](../deploy/wsl-waydroid-acc/deploy.sh)

## 2. 第一段：Android 进程如何调用宿主 GPU

1. **接住 Android 的调用。** [构建脚本](../build.sh)用 Android NDK 编译 `libEGL_angle.so` 和 `libGLESv2_angle.so`，装到镜像的 `/system/lib64/`。这里的 `_angle` 是安装入口名，不表示加速路径用原版 ANGLE 绘制。应用通过直接导出的 GLES 符号或 `eglGetProcAddress` 取得入口；窗口交换等必须由项目处理的函数仍返回转接层入口，而不是绕过它直接调用 Mesa。GLES 转接库查找进程内已经加载的 EGL 转接库，避免同一进程出现两套上下文和线程局部状态。[EGL 入口](../src/egl_shim.c) · [GLES 分发](../src/gles_shim.c)
2. **跨过 Bionic/glibc 边界。** Android 使用 Bionic；WSL 的图形库是 Linux 用户态 ELF，不能把路径交给 Android 的普通动态链接器就当作 Android 驱动使用。[专用 ELF 加载器](../src/loader_so.c)映射库的可加载段、加载依赖、做符号解析与重定位，并处理线程局部存储和所需的运行库调用。它也将驱动查找引向容器内准备好的库目录。这是在进程内接入一组已知图形库，不是替换 Android 的 libc，也不是把每次 GL 调用通过 socket 发给 WSL。
3. **先画到 Mesa，再交给 Android 窗口。** Mesa 以 surfaceless EGL 和 Gallium D3D12 后端构建。在转接路径上，`eglCreateWindowSurface` 保存 Android 的 `ANativeWindow`，为实际绘制建立 Mesa 的离屏 surface（pbuffer）。绘制命令由 Mesa 经 D3D12、DXCore 和宿主驱动执行；到 `eglSwapBuffers` 时，转接层才负责把结果送入对应 Android 窗口的队列。**因此“GPU 已经画完”和“Android 已经拿到一帧”是两个不同的时点。** [窗口创建与交换](../src/egl_shim.c) · [窗口提交](../src/window.h)

## 3. 第二段：应用画面如何进入 SurfaceFlinger

这里并行传递两种东西：**Android 缓冲区标识这帧排在哪个槽里；共享纹理保存该代可采样的 GPU 像素。** 不能用同一个槽位 ID 代替内容版本，也不能用纹理句柄代替 Android 的归还协议。

1. **选槽并检查布局。** 生产者从原来的 `ANativeWindow` dequeue 一个槽，等待 acquire fence，再通过 mapper 查询真实分配。代码检查角色与窗口尺寸、单层 RGBA、stride、usage、FourCC、modifier 和 buffer ID；不是看到 EGL 窗口就一律开启。普通应用与系统窗口分别协商能力；满足延迟 CPU 镜像的条件后才采用相应 usage。失败时取消仍由自己持有的槽，不能把没有准备好的内容入队。[生产者与准入条件](../src/window.h)
2. **导出这一代像素。** 项目为 [Mesa state tracker](../mesa/src/mesa/state_tracker/st_cb_readpixels.c) 增加内部入口：从当前读帧缓冲取源纹理，按需要处理方向，以 GPU blit 写入可共享的 RGBA8 资源，然后提交命令。返回的是两个不同的 fd：一个引用纹理，一个来自 [D3D12 完成 fence](../mesa/src/gallium/drivers/d3d12/d3d12_screen.cpp)。拿到资源句柄本身不表示 GPU 已写完。
3. **发布，再入队。** 生产者通过 Unix 域套接字将纹理 fd、完成 fd 和 Android 分配的 plane fd 一起交给 WSL 的 `input-broker`。这里的 *input* 是“合成器的输入图层”，不是鼠标服务。broker 检查发送进程身份，将分配与内容代次关联；生产者收到对应 ID 和序号的成功应答后才向 BufferQueue 提交原槽。对需要判断首帧内容的已支持路径，broker 可以在 GPU 上查询纹理是否全零；这不是对任意黑色画面一律丢帧。[broker](../src/broker.c) · [身份与包信息](../src/packages.h) · [可见性查询](../src/visibility.h)
4. **合成时绑定纹理。** SurfaceFlinger 仍使用自己的 RenderEngine；项目改变的是它经过的 [EGLImage 适配](../src/egl_image.c)，没有替换 SurfaceFlinger 可执行文件。绑定 Android 原生缓冲区时，[输入导入代码](../src/input.h)按分配和代次查找 GPU 纹理。已确认的同代纹理可复用导入对象；可被 CPU 改写的分配仍须让 broker 重新核验，摘要覆盖行尾填充，不能因为句柄相同就跳过。导入失败则请求 CPU 镜像，不能把旧代纹理当作新帧。[镜像摘要](../src/mirror-digest.h)

## 4. 第三段：合成结果如何到达 Weston

上一段传的是**一层应用内容**；这一段传的是**物理显示的 CLIENT 合成目标**，即 RenderEngine 把归它合成的若干层画到一起后的结果。两段独立判断：成功导入某个应用层，不保证最终显示也走共享纹理；最终显示命中，也不证明所有输入层都免了拷贝。

1. **保留原 HAL，包装特定入口。** 项目安装 `/vendor/lib64/hw/hwcomposer.waydroid.so`，它再打开原有的 `hwcomposer.vendor-base.so`。只有 mapper 接口和原 HAL 的元数据、建缓冲区、更新共享内存等入口与已知后端匹配时，才接入自己的更新逻辑。对符合条件的物理层组，在 prepare 阶段交给 CLIENT 合成；不合条件仍调用原 HAL 的 prepare。原 HWC 继续处理表面、层次和提交。[HAL 适配](../src/adapter.c) · [层组条件](../src/policy.h)
2. **从 RenderEngine 取得匹配快照。** [目标生产者](../src/producer.h)识别 SurfaceFlinger 的物理目标，检查 usage（当前为 `0x1b00`）、格式、stride、分配大小和 ID，为这一代保留可共享快照及恢复所需引用。[HWC 端](../src/client-hwc.h)确认列表中确有 CLIENT 内容和目标，等待目标 acquire fence，按该分配向生产者取 GPU 快照或已准备好的 CPU 镜像。预先创建目标可以省分配时间，不能省去从当前帧缓冲生成快照的 blit。
3. **只替换 carrier 的像素来源。** 原 HAL 仍会把一个 Wayland 缓冲区（协议称 *carrier*）附到表面。项目的 [Wayland 协议](../weston/texture.xml)让 HWC 先发 `prepare_target`，把纹理 fd 和尺寸交给嵌套 Weston；后续用 `reuse` 将导入结果关联到正确的 carrier。Weston 的 [GL 渲染器](../weston/libweston/renderer-gl/texture.c)以 `GL_HANDLE_TYPE_OPAQUE_FD_EXT` 导入 GPU 资源，[合成器](../weston/libweston/compositor.c)在匹配的 attach 上采用它。表面状态、损伤区域、层序和 commit 仍属于原来的 Wayland 连接。嵌套 Weston 的窗口再通过父级 WSLg 显示到 Windows；不是让 Android 直接向 Windows 窗口提交纹理。
4. **首次确认，成功后才异步。** 对未验证的生产者和尺寸先等 Weston 回报导入结果；相同生产者和尺寸已有成功记录时，才允许异步 prepare，减少 HWC 等待应答的时间。拒绝、释放和退役是不同事件：被拒的缓冲区不会再收到正常 release。代码单独跟踪在途请求，拒绝后请求镜像恢复；恢复不成时可以保留上一次表面内容，而不是贴上没有有效像素的 carrier。[异步状态](../src/async-target.h) · [恢复](../src/recovery.h) · [协议的 hold_carrier](../weston/texture.xml)

## 5. 哪些像素仍走 CPU，为什么要保留这些路径

| 场景 | 处理方式 |
|---|---|
| 窗口或消费者不符合共享条件 | 走原有窗口提交、CPU 读回或上传路径。需要暂存像素时，[暂存槽](../src/scratch.h)在提交或写入完成前不出借，之后可供同尺寸的短命窗口复用。 |
| Android 需要读像素，或 HWC 不能用 GPU 导入 | 从对应快照生成 CPU 镜像，使 gralloc 中有可读的实际像素。[宿主 texture-consumer](../src/consumer.c)是其中一条兼容路径：在获准写入后用宿主 GL 将共享纹理读回对应 plane，处理 stride、方向与同步。它不是共享纹理成功时每帧必经的中转站。HWC 的 [shm 更新路径](../src/adapter.c)在适用时逐行复制并调整通道顺序；其他布局交回原实现。 |
| 壁纸反复绑定 | [壁纸代码](../src/wallpaper.h)仅识别紧密排列、身份匹配、大小不超过 16 MiB 的 RGBA8 BLAST 壁纸分配。在 dma-buf 的 CPU 读同步区间逐字节比较；相同时重绑原图，不同才上传。省的是重复上传，不是 CPU 比较。 |
| 位图与视频不是普通窗口 RGBA 帧 | [位图路径](../src/bitmap.h)在上下文、纹理及上传布局匹配时把完整 RGBA 上传反映到 Android 缓冲区，避免其他读者看到旧图。[视频配置](../services/decoder-output.rc)让本镜像的 FFmpeg Codec2 输出 YUV_420；[YV12 适配](../src/yv12.h)按平面和色域转换并复用线程暂存区。这是解码后像素的兼容处理，不是新增视频硬解码器。 |

## 6. 宿主装了什么，多个进程如何控制内存

WSL 中有三个图形角色：[`input-broker`](../services/systemd/waydroid-acc-input.service)管理输入图层的代次、导入与镜像；[`texture-consumer`](../services/systemd/waydroid-acc-consumer.service)处理受支持的读回请求；[嵌套 Weston](../services/systemd/waydroid-acc-weston.service)负责显示。前两者使用部署目录中的 EGL/Gallium；Weston 使用发行版的 `/usr/bin/weston`，但加载项目的 libweston、GL 渲染器和 Wayland 后端，对 Waydroid 提供 `wd-weston`，对上连接 WSLg 的 `wayland-0`。[部署器](../deploy/wsl-waydroid-acc/deploy.sh)将宿主组件放在 `/usr/local/lib/wsl-waydroid-acc/` 的 release 目录，在 Android 一侧更新 overlay 里的 EGL/GLES、HWC 以及数据目录中的 Gallium。服务运行不从源码树或 NDK 加载文件。窗口尺寸、输入与启动操作另见 [MISC](MISC.md)。

进程内渲染意味着多个 Android 图形进程都会加载驱动和 Mesa。为避免每个进程各复制一套，[ELF 加载器](../src/loader_so.c)对已知 AMD 用户态驱动、Gallium、libcrypto、libstdc++ 核对构建 ID、段布局和页大小。匹配时将不可写段的完整文件页映射到各进程，内核可共享同一份物理页；可写段和不足一页的尾部仍按原方式处理。不匹配或文件映射失败则走复制路径，不盲用其他版本的布局。另行回收不再需要的解析映射和 fd。这与纹理缓存是两件事：前者省跨进程的库内存，后者省同代图像的重复导入。

## 目前的瓶颈

- **还不是端到端零拷贝。** 本机的 Android gralloc 与 WSL D3D12 没有能让两端直接导入同一分配的 dma-buf 通路。gralloc plane fd、D3D12 纹理 fd、GPU 就绪 fd 即便都能经 Unix 套接字传递，也分别表示像素平面、GPU 资源、完成事件，不能互换。共享路径省的是 GPU→CPU 读回和整帧 `wl_shm` 复制，不是所有 GPU 拷贝，也不意味着项目控制 WSLg 到 Windows 的后续合成。[WSL GPU 架构](https://devblogs.microsoft.com/directx/directx-heart-linux/) · [Linux dma-buf](https://docs.kernel.org/6.18/driver-api/dma-buf.html)
- **队列归还和同步仍决定部分尾帧。** 生产者可能等 BufferQueue 归还槽位，即使不用 CPU 拷贝也可能等 GPU 完成、broker 应答或首次导入确认。异步只移走可延后的等待。已测通知栏场景中，broker 在等待查询时穿插处理其他窗口，两个批次的应答 P95 并未一致改善；显示帧 P95 为 16.45 / 17.68 ms，不能将超过 60 Hz 预算的帧全部归因于 broker。
- **覆盖与移植都有条件。** 增大 Weston 窗口不等于更多 Android 格式、分辨率或生产者已经通过验证。更换镜像要重新核对 gralloc 布局、HAL ABI、图形库以及 CPU 读者；更换宿主驱动也要重新验证导入与文件页共享。待验证的扩展单列在 [UNFINISHED](UNFINISHED.md)。

## 已有验证与优化收益

以下是实现这些改动时的对照记录，不是对当前版本重新跑出的游戏基准。**1080p：** 2026-11-21 核对 Weston、broker、consumer 映射到同一份部署 release。设置启动、滚动和返回桌面期间导入了 67 个全尺寸 EGLImage；两批设置滚动显示帧 P50 为 7.50 / 7.81 ms，P95 为 9.31 / 9.67 ms。另一次设置页测量中，目标创建、命令提交与句柄处理合计 P50 从 3.19 降至 1.54 ms。下表各行的场景和统计对象不同，不能累加。

| 当前版本已采用的改动 | 同条件对照结果 |
|---|---|
| broker 全量 BLAKE3 校验 | 保留包括行填充在内的逐字节覆盖；计算器上通知栏展开和收起合计 P95：19.26 → 15.22 ms。单看展开仍是 19.00 ms。 |
| 复用同代 GPU 导入对象 | 仅在 broker 完成本次可写缓冲区校验后复用。展开 P95：19.02 → 16.15 ms；仍有 22/452 帧超出 60 Hz 预算。 |
| AMD 驱动共享只读页 | 整栈工作集：桌面 4862 → 2977 MiB，依次打开设置、Jelly、计算器后 5884 → 4047 MiB；上述检查点均无 swap。 |
| 回收 ELF 解析映射与 fd，共享 Gallium 只读页 | 以 AMD 共享已开启为对照，桌面和多应用的整栈工作集分别再少 178.4、217.8 MiB。 |
| CPU 暂存槽复用 | 多次打开搜索并返回桌面，搜索进程从 8 个槽降至 1 个（63.28 → 7.91 MiB）。四个操作的实测显示帧 P95 未变差。 |
| libcrypto 与 libstdc++ 共享只读页 | 多应用下仅开一项分别省 65.05、28.46 MiB，同时开启省 93.55 MiB（12 个图形进程的目标映射按物理页去重，非整机节省）。未测得可稳定归因于某一库的通知栏展开尾帧退化。 |

**较早的 1920×1048 配置：** 2026-11-20 的构建在两种尺寸、12 代共享纹理上通过离屏检查。两批正反顺序测试中，设置滚动的显示帧 P50 从 11.53 / 11.43 降到 6.97 / 7.08 ms。另一次 Windows 录屏覆盖设置、切换、通知栏、最近任务及返回桌面，12 秒、720 帧中未检出全黑帧或横向黑条。这不能替代 1080p 的逐帧验收。

## English

[Project overview](../README.en.md)

There are two separate problems: **how an Android process renders on the Windows GPU**, and **how the next process gets those pixels without reading them back through the CPU**. An EGL/GLES bridge and an in-process loader address the first. Changes to Mesa, the Waydroid composer HAL and a nested Weston address the second. Access to D3D12 alone does not give Android apps, SurfaceFlinger and Weston a common allocation.

### 1. What the original stack provides and what changes

| Boundary | Kept from the original stack | What this project changes |
|---|---|---|
| Windows and the WSL2 kernel | The Windows display driver, WSL DXCore/D3D12 libraries and `/dev/dxg` provide access to the host GPU. WSLg displays Wayland clients. | The tested kernel provides dxgkrnl, Binder, dma-buf heaps and synchronization. The runtime build produces user-space components. It does not build a Windows driver or patch WSLg. |
| Android app to graphics driver | Apps, HWUI and games use Android EGL/GLES and native-window APIs. | Install 64-bit EGL/GLES shims and load glibc-targeted Mesa D3D12 into the calling process. |
| App to SurfaceFlinger | Gralloc allocates buffers, BufferQueue owns queueing and release, and SurfaceFlinger controls composition. | Publish a GPU texture alongside eligible window buffers and import it through the EGLImage path used by RenderEngine. Maintain a gralloc CPU mirror when needed. |
| SurfaceFlinger to desktop | The image's vendor HWC owns Wayland surface state and commits. WSLg manages the outer window. | Wrap the HWC and add a shared-texture protocol and recovery path. Run a nested Weston with custom GL import support inside the WSL distribution, as a client of WSLg. |

**Prerequisites.** The installer targets an already configured x86_64 Waydroid instance, not pristine WSL or arbitrary Android images. VKMS, VGEM and dma-heap serve the tested Android device and allocation setup; the presence of `card1` or `renderD128` does not mean they drive the Radeon GPU. D3D12 rendering goes through `/dev/dxg`. The installer expects compatible vendor HAL and platform libraries and a prepared container-side D3D12/graphics user-mode driver. Neither Digitalis nor a KernelSU module is required for this graphics path. [Kernel and devices](../README.en.md#51-wsl-kernel-and-devices) · [Installer prerequisites](../deploy/wsl-waydroid-acc/deploy.sh)

### 2. Android process to host GPU

1. **Intercept the Android entry points.** The [build](../build.sh) uses the Android NDK to build `libEGL_angle.so` and `libGLESv2_angle.so` for the image's `/system/lib64/`. The installation name does not mean stock ANGLE renders the accelerated path. GLES calls arrive through exported symbols or `eglGetProcAddress`. Operations the bridge must handle, notably window swaps, return the shim entry point instead of bypassing it. The GLES shim looks up the already loaded EGL shim so the process does not acquire two loaders with separate contexts or thread-local state. [EGL entry points](../src/egl_shim.c) · [GLES dispatch](../src/gles_shim.c)
2. **Cross the Bionic/glibc boundary.** Android uses Bionic; the WSL graphics libraries are Linux user-space ELF objects, not drop-in Android drivers. A [purpose-built ELF loader](../src/loader_so.c) maps loadable segments, loads dependencies, resolves symbols and relocations, and handles TLS and required runtime calls. It redirects driver lookups to the prepared libraries inside the container. This supports a known set of libraries inside the process; it does not replace Android's libc or send every GL call over a socket.
3. **Render first; queue later.** Mesa is built for surfaceless EGL with the Gallium D3D12 backend. On the shim path, `eglCreateWindowSurface` retains the Android `ANativeWindow` but creates a Mesa offscreen surface (pbuffer) for drawing. Mesa submits rendering through the WSL D3D12 stack and host driver. At `eglSwapBuffers` the shim hands the output to the Android window queue. Finishing a GPU draw and delivering a frame to Android are distinct operations. [Surface creation and swap](../src/egl_shim.c) · [Window submission](../src/window.h)

### 3. App window to SurfaceFlinger

Two objects travel in parallel: **the Android buffer identifies the queued allocation; the shared texture supplies GPU pixels for that generation**. A slot ID is not a content version, and a texture handle cannot release an Android queue slot.

1. **Dequeue and qualify the allocation.** The producer dequeues from the original window, waits for its acquire fence, then asks the mapper about the actual allocation. It checks the producer role, dimensions, single-layer RGBA layout, stride, usage, FourCC, modifier and buffer ID. Ordinary apps and system windows negotiate capabilities separately; deferred CPU mirrors require qualifying usage. On failure the producer cancels a slot it still owns rather than queueing incomplete content. [Window eligibility and publishing](../src/window.h)
2. **Export these pixels.** New [Mesa state-tracker entry points](../mesa/src/mesa/state_tracker/st_cb_readpixels.c) take the current read framebuffer, account for orientation, blit to a shareable RGBA8 resource and submit the GPU work. One fd identifies the texture; a second comes from the [D3D12 completion fence](../mesa/src/gallium/drivers/d3d12/d3d12_screen.cpp). Having a handle does not establish that the GPU has finished writing.
3. **Publish before queueing.** Over a Unix socket the producer sends the texture fd, completion fd and Android plane fd to the WSL `input-broker`. “Input” means a compositor input layer, not a mouse event. The broker checks the sending process and associates the allocation with a content generation. Only an acknowledgement for the correct ID and serial allows the producer to queue the original slot. On eligible first-frame paths a GPU query can detect an all-zero texture; it is not a general rule to drop black frames. [Broker](../src/broker.c) · [Peer and package identity](../src/packages.h) · [Visibility query](../src/visibility.h)
4. **Bind during composition.** SurfaceFlinger still runs its own RenderEngine. The project modifies the [EGLImage bridge](../src/egl_image.c) beneath it, not the SurfaceFlinger executable. When Android binds a native buffer, the [input import](../src/input.h) looks up the texture by allocation and generation. A validated generation can reuse the imported object; a CPU-writable allocation must still be revalidated by the broker, including row padding. On import failure the consumer requests a CPU mirror rather than sampling a previous generation. [Mirror digest](../src/mirror-digest.h)

### 4. Composed display to Weston

The previous boundary carries **one app layer**. This one carries the **physical display's CLIENT target**, the output RenderEngine produced for the layers assigned to client composition. They qualify independently: an imported input layer does not guarantee a GPU display handoff, and a successful display handoff says nothing about copies on other input layers.

1. **Wrap rather than replace the vendor implementation.** The installed `/vendor/lib64/hw/hwcomposer.waydroid.so` opens the original `hwcomposer.vendor-base.so`. It installs its update logic only when mapper and vendor buffer metadata, creation and shared-memory update entry points match the expected backend. During prepare it steers eligible physical layer groups into CLIENT composition; other cases retain vendor policy. The original HWC still owns surface state, stacking and commits. [HAL adapter](../src/adapter.c) · [Grouping policy](../src/policy.h)
2. **Match the output snapshot.** The [target producer](../src/producer.h) recognizes SurfaceFlinger's physical target and checks usage (currently `0x1b00`), format, stride, allocation size and ID. It retains the generation's shareable snapshot and the references required for recovery. The [HWC side](../src/client-hwc.h) checks for CLIENT content and target, waits for its acquire fence and requests the matching texture or CPU mirror from the producer. Preallocating the destination saves allocation time, not the blit from the current framebuffer.
3. **Replace the carrier's pixel source, not its surface.** The vendor HWC still attaches a Wayland buffer (the *carrier*) to its surface. With the [private Wayland protocol](../weston/texture.xml), HWC sends `prepare_target` with texture fd and dimensions, then uses `reuse` to associate the import with the correct carrier. Weston's [GL renderer](../weston/libweston/renderer-gl/texture.c) imports the resource as `GL_HANDLE_TYPE_OPAQUE_FD_EXT`; the [compositor](../weston/libweston/compositor.c) uses it on the matching attach. Damage, stacking, other surface state and commit remain on the vendor connection. Nested Weston presents its window to parent WSLg; Android is not submitting a texture directly to a Windows window.
4. **Confirm first, then allow asynchronous prepare.** An unproven producer/size combination waits for Weston's import result. A previously successful combination can prepare asynchronously so HWC need not block on every reply. Rejection, release and retirement are distinct: a rejected buffer will not receive a normal release. The adapter tracks outstanding requests, requests a mirror after rejection and can hold the last surface content if recovery fails, rather than attaching a carrier without valid pixels. [Asynchronous offers](../src/async-target.h) · [Recovery](../src/recovery.h) · [hold_carrier](../weston/texture.xml)

### 5. Where pixels still go through the CPU

| Case | What happens |
|---|---|
| Ineligible window or consumer | Use the existing window presentation, readback or upload path. A [CPU staging slot](../src/scratch.h) remains leased until presentation or writing finishes; another short-lived window can then reuse idle storage of the right size. |
| Android needs CPU pixels, or HWC cannot use an import | Materialize a readable gralloc mirror from the matching snapshot. The host [texture-consumer](../src/consumer.c) is one compatibility path: after the write grant, its GL context reads the shared texture back to the specified plane with the correct stride, orientation and synchronization. It is not a mandatory hop for every displayed texture. Where applicable, [HWC's shm path](../src/adapter.c) copies rows and swaps channels; unrelated layouts go to the original implementation. |
| Repeated wallpaper binds | The [wallpaper cache](../src/wallpaper.h) accepts only identified, tightly packed RGBA8 BLAST wallpaper allocations up to 16 MiB. It compares bytes inside a dma-buf CPU-read synchronization interval. Identical pixels rebind the existing image; changed pixels are uploaded. This avoids an upload, not the CPU comparison. |
| Bitmap and video inputs | The [bitmap path](../src/bitmap.h) reflects matching full RGBA uploads into the Android buffer so other readers do not see stale pixels. The [Codec2 setting](../services/decoder-output.rc) requests YUV_420 from this image's FFmpeg decoder; the [YV12 bridge](../src/yv12.h) converts planes according to their color space and reuses per-thread scratch memory. This handles pixels after decoding; it is not a hardware video decoder. |

### 6. WSL installation and cross-process memory

There are three host graphics roles: the [input broker](../services/systemd/waydroid-acc-input.service) tracks input generations, imports and mirrors; the [texture consumer](../services/systemd/waydroid-acc-consumer.service) handles supported readback requests; [nested Weston](../services/systemd/waydroid-acc-weston.service) displays the result. The first two use the installed EGL/Gallium libraries. Weston runs the distribution's `/usr/bin/weston` with project libweston, GL renderer and Wayland backend modules. Its client socket is `wd-weston`; its parent is WSLg's `wayland-0`. The [installer](../deploy/wsl-waydroid-acc/deploy.sh) puts host components in a release under `/usr/local/lib/wsl-waydroid-acc/`, updates Android's overlay EGL/GLES and HWC libraries, and places Gallium in the container data directory. Running services do not load files from a checkout or NDK. For window sizing and input see [MISC](MISC.md#english).

In-process rendering means several Android graphics processes load the driver and Mesa. For recognized AMD driver, Gallium, libcrypto and libstdc++ builds, the [ELF loader](../src/loader_so.c) checks build ID, segment layout and page size. It file-maps whole pages of non-writable segments so processes can share physical pages. Writable segments and partial-page tails keep their original handling. Unknown builds or failed file mappings use the copy path instead of assuming another build's layout. Parser mappings and fds are released when no longer needed. This saves library memory across processes; it is independent of caching a texture import within a generation.

### Remaining bottlenecks

- **Not end-to-end zero-copy.** On this stack Android gralloc and WSL D3D12 cannot directly import the same allocation through dma-buf. A gralloc plane fd, D3D12 texture fd and GPU readiness fd can all travel over a Unix socket, but identify a pixel plane, GPU resource and completion event respectively. Shared handoffs avoid GPU-to-CPU readback and a full-frame `wl_shm` copy, not all GPU copies or later WSLg/Windows composition. [WSL GPU architecture](https://devblogs.microsoft.com/directx/directx-heart-linux/) · [Linux dma-buf](https://docs.kernel.org/6.18/driver-api/dma-buf.html)
- **Buffer release and synchronization affect tail latency.** Producers can still wait for BufferQueue slots, GPU completion, broker replies and first imports. Asynchronous handling removes only deferrable waits. When the broker serviced other windows during GPU queries, two notification-shade batches did not agree on the change in reply P95. Display-frame P95 was 16.45 / 17.68 ms: the measurements do not isolate the broker as the cause of missed 60 Hz deadlines.
- **Coverage and portability are conditional.** A resizable Weston window does not validate more Android formats, resolutions or producers. A different image requires checking gralloc layout, HAL ABI, graphics libraries and CPU readers. A different host driver requires rechecking import and file-page sharing. See [unfinished directions](UNFINISHED.md#english).

### Existing validation and measured effects

These are comparisons from implementation work, not a new gaming benchmark of the current version. **1080p:** on 2026-11-21 Weston, the broker and consumer were verified to map the same installed release. Settings launch, scrolling and return to Home produced 67 full-size EGLImage imports. Two scrolling batches measured display-frame P50 of 7.50 / 7.81 ms and P95 of 9.31 / 9.67 ms. In a separate Settings measurement, target creation, submission and handle processing together fell from 3.19 to 1.54 ms at P50. The comparisons below measure different objects and scenarios; their savings cannot be added.

| Changes in the current version | Result under matched conditions |
|---|---|
| Full-buffer BLAKE3 validation in the broker | All bytes, including row padding, remain covered. Combined shade-open/close P95 over Calculator: 19.26 → 15.22 ms. Opening alone still measured 19.00 ms. |
| Reuse imports within a generation | Reuse occurs only after the broker validates the writable buffer for that request. Shade-open P95: 19.02 → 16.15 ms; 22/452 frames still exceeded the 60 Hz budget. |
| Share read-only AMD driver pages | Whole-stack working set: 4862 → 2977 MiB at Home; 5884 → 4047 MiB after opening Settings, Jelly and Calculator. No swap at those checkpoints. |
| Release ELF parser mappings and fds; share read-only Gallium pages | Relative to AMD sharing already enabled, the desktop and multi-app working sets fell by a further 178.4 and 217.8 MiB. |
| Reuse CPU staging slots | Repeatedly opening Search and returning Home reduced its pool from 8 slots to 1 (63.28 → 7.91 MiB). None of the four measured actions had a worse display-frame P95. |
| Share read-only libcrypto and libstdc++ pages | In the multi-app test, enabling one library saved 65.05 or 28.46 MiB, respectively; together they saved 93.55 MiB. These are resident physical pages of the target mappings, deduplicated across 12 graphics processes, not whole-machine savings. No repeatable shade-open tail regression could be attributed to one library. |

**Earlier 1920×1048 setup:** the 2026-11-20 build passed offscreen checks across two sizes and twelve texture generations. In two opposite-order batches, Settings scrolling display-frame P50 went from 11.53 / 11.43 to 6.97 / 7.08 ms. A separate 12-second, 720-frame Windows recording covered Settings, switching apps, the notification shade, Recents and Home; no all-black frames or horizontal black bands were found in that recording. It is not frame-by-frame acceptance at 1080p.
