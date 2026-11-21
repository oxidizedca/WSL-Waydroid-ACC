# 离屏：动态底图、静态侧层与 Planner 缓冲交接

[English](#english)

**结论（2026-11-21）：** 在本机实际 D3D12 Weston 上，只要载体、viewport 和提交的缓冲匹配，在三个小子表面与一个全屏透明缓存层之间切换可以得到完全相同的 RGB 输出。隐藏子表面与释放其最后一个缓冲不是同一件事。没有在生产 UI 重开 Planner，也没有重现或证明上次 HWC 退出的直接原因。

## 实际运行

1. **Android 两进程。** 普通离屏 EGL 进程分配 1920×1080 RGBA8888 AHardwareBuffer，写入蓝底和红块，flush/finish，通过 Unix socket 把同一缓冲交给第二进程，独立导入并校验两处像素。分别使用 Planner 的 usage `0xb00`（RENDER | COMPOSER | TEXTURE）和带 CPU 位的对照 `0xb33`，实际 stride 均为 1920、format 均为 1，写端与读端均得到预期的两个颜色。[输出](android.txt) · [源码](ahb.c)。该项不是 RenderEngine 实例，也不是模拟显示 fence 的时序测试。
2. **WSL 专用 headless Weston。** 使用已安装的定制 libweston 和 GL renderer、只对测试进程可见的 socket；用项目构建的 Gallium 导出真正的 D3D12 RGBA8 纹理，等生产 fence 后走 v3 的 prepare_target / reuse / retire。合成场景为交替更新的蓝/绿底和三个侧边色块（含半透明层）；紫色 SHM 仅作为载体。三组各 12 帧，前两帧建立两种背景的未合并参照，其余 10 帧将整张 1920×1080 输出与对应参照比较，**共 30 张对照帧，RGB 不同像素均为 0**。同时校验背景色和色块位置，不把初始两帧计入比较。这是离屏输出校验，不是抓取 Windows/游戏画面，也不是游戏帧时测量。[逐帧](pixels.csv)
3. **归还时机。** 独立缓存子表面、复用原侧层子表面的两种模式中，做 null attach + 子表面/父表面 commit + retire 后，又观察四次输出：两张底图已归还，最后的缓存纹理尚无 release；销毁对应的子表面后收到一次 release。第三组在同一子表面显示新的实际缓冲替换缓存，此时无需销毁表面便收到 release。三组最终均为 3 次 retire、3 次 release；这证明不能把“已隐藏”当成“可以复写”，并非证明永久泄漏或上次崩溃源自释放过早。[事件计数](release.csv)

运行 WSL 侧三组校验：先按仓库说明构建 ACC、安装同版 Weston，然后执行本目录的 `bash replay.sh`。使用系统 Weston 14 headless 后端、wayland-scanner、wayland-protocols 与 libweston-14 的 output-capture XML。只检查私有内存输出，不保存桌面截图；正常退出时移除临时文件，不连接默认显示。

## 适配边界：缓冲区尺寸与 source crop

- AOSP 的 Flattener 以显示大小分配缓存；OutputLayer 用 override buffer 的实际宽高作 source crop，并用 override 专用槽传给 HWC。这里不是把侧边 27×108 的 buffer 原样再提交。
- Waydroid 的 Android 16 [几何信息补丁](https://github.com/waydroid/android_vendor_waydroid/blob/a09fb7a8759d807bed9016ad7416c14c93903b0e/waydroid-patches/base-patches-36/frameworks/native/0006-surfaceflinger-Provide-layer-names-and-stuffs.patch)和[按帧补发补丁](https://github.com/waydroid/android_vendor_waydroid/blob/a09fb7a8759d807bed9016ad7416c14c93903b0e/waydroid-patches/base-patches-36/frameworks/native/0017-surfaceflinger-Send-layer-buffer-info-to-hwcomposer-.patch)另走 Waydroid display 扩展上报元数据。后者在 setBuffer 旁仍把 **outputIndependentState.buffer** 传给 setLayerHandleInfo，并非条件选中的 override。通用 HWC 路径用上报的宽、高、stride、format 构建 SHM 载体。[上游 HWC 参考代码](https://github.com/waydroid/android_hardware_waydroid/blob/3f8653798de35f534ff6c5ae90368aab432c18b1bacfcd8381336b/hwcomposer/gralloc_handler.cpp)
- 本机 vendor HAL SHA-256 是锁定基线的 `942fad47d8372ea1eb6873a8ccad0954d91592b0fc6137d8eff2f638043f1a29`，导出 setLayerHandleInfo 和 setLayerSize；本机 SF Build ID 是 `54cc179eb5a9c79c64af28c4659b479d`。上游 dev 分支不是已证明的本机源码 checkout；二进制符号和日志字符串不能证明上次那帧的值。**这是一处上游代码可见的不一致，本机故障归因仍待证。**
离屏试验没有捕获故障帧从 mapper 到 HWC 的完整元数据，不能确认实际提交的 buffer、上报尺寸和 source crop 是否一致。该线索不构成修改 Weston 释放策略或启用 Planner 的依据。

---

<a id="english"></a>

# Offscreen: dynamic background, static side layers and Planner buffer handoff

**Result (2026-11-21):** On the actual D3D12 Weston renderer, switching between three small subsurfaces and one full-screen transparent cache layer produced identical RGB output when the carrier, viewport and submitted buffer matched. Hiding a subsurface does not release its last buffer. We did not re-enable Planner in the production UI, reproduce the previous HWC exit, or establish its immediate cause.

## Experiments

1. **Two Android processes.** An offscreen EGL process allocated a 1920×1080 RGBA8888 AHardwareBuffer, drew a blue background and a red block, flushed and finished, and passed the same buffer to a second process over a Unix socket. The second process imported it independently and checked two pixels. Both Planner usage `0xb00` (RENDER | COMPOSER | TEXTURE) and the control with CPU bits `0xb33` produced stride 1920 and format 1; writer and reader saw both expected colors. [Output](android.txt) · [Source](ahb.c). This was not a RenderEngine instance or a test of display-fence timing.
2. **Dedicated headless Weston in WSL.** We used the installed custom libweston and GL renderer with a socket visible only to the test processes. The project build of Gallium exported real D3D12 RGBA8 textures. After waiting for the producer fence, the test used v3 prepare_target / reuse / retire. It composed an alternating blue/green background and three colored side blocks, including a translucent layer; a purple SHM buffer served only as the carrier. Each of three runs produced 12 frames. The first two established unmerged references for the two backgrounds; each of the next 10 full 1920×1080 outputs was compared with its corresponding reference. **All 30 comparison frames had zero differing RGB pixels.** Background colors and block positions were also checked. The first two frames were not counted. This validates offscreen output, not Windows or game screenshots or game frame times. [Per-frame data](pixels.csv)
3. **When buffers are returned.** Two variants used a dedicated cache subsurface or reused a side-layer subsurface. After a null attach, child and parent commits, and retire, we observed four more outputs: both backgrounds had been returned, but the last cached texture had no release. Destroying its subsurface produced one release. In the third variant a real replacement buffer was displayed on the same subsurface, and the cache was released without destroying the surface. All three variants finished with 3 retires and 3 releases. Hidden does not mean safe to overwrite; this does not prove a permanent leak or that premature release caused the previous crash. [Event counts](release.csv)

To run the three WSL checks, build ACC and install the matching Weston as described in the repository, then run `bash replay.sh` in this directory. The test uses the system Weston 14 headless backend, wayland-scanner, wayland-protocols and libweston-14's output-capture XML. It validates private in-memory output only, does not save desktop screenshots or connect to the default display, and removes temporary files on normal exit.

## Integration boundary: buffer dimensions and source crop

- AOSP Flattener allocates its cache at display size. OutputLayer uses the override buffer's actual width and height as its source crop and passes it to HWC in a dedicated override slot. This is not a resubmission of the original 27×108 side buffer.
- Waydroid's Android 16 [Geometry metadata patch](https://github.com/waydroid/android_vendor_waydroid/blob/a09fb7a8759d807bed9016ad7416c14c93903b0e/waydroid-patches/base-patches-36/frameworks/native/0006-surfaceflinger-Provide-layer-names-and-stuffs.patch) and [Per-frame update patch](https://github.com/waydroid/android_vendor_waydroid/blob/a09fb7a8759d807bed9016ad7416c14c93903b0e/waydroid-patches/base-patches-36/frameworks/native/0017-surfaceflinger-Send-layer-buffer-info-to-hwcomposer-.patch) also report metadata through the Waydroid display extension. Alongside setBuffer, the latter still passes **outputIndependentState.buffer** to setLayerHandleInfo rather than the conditionally selected override. The generic HWC path builds the SHM carrier from the reported width, height, stride and format. [Upstream HWC reference](https://github.com/waydroid/android_hardware_waydroid/blob/3f8653798de35f534ff6c5ae90368aab432c18b1bacfcd8381336b/hwcomposer/gralloc_handler.cpp)
- The installed vendor HAL matched the pinned SHA-256 `942fad47d8372ea1eb6873a8ccad0954d91592b0fc6137d8eff2f638043f1a29` and exported setLayerHandleInfo and setLayerSize; the installed SF Build ID was `54cc179eb5a9c79c64af28c4659b479d`. The upstream dev branch is not a verified checkout of the installed source, and binary symbols and log strings cannot establish the values on the failed frame. **The upstream code shows an inconsistency; whether it caused the local failure remains unproven.**
The offscreen test did not capture the failed frame's full mapper-to-HWC metadata, so it cannot show whether the actual submitted buffer, reported dimensions and source crop agreed. This is not grounds to change Weston's release policy or enable Planner.
