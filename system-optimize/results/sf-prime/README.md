# SurfaceFlinger 启动着色器预热：内存与首轮转场

[English](#english)

环境：1920×1080、60 Hz、Waydroid x86_64 / Android 16、Skia GLES → Mesa D3D12；设置和计算器均为镜像内应用。只改变 `service.sf.prime_shader_cache`。不更改磁盘着色器缓存、不关闭着色器编译，也不干预应用或游戏自身的管线预热。

## 对照和结论

- [原会话](warm1.csv)：从同一桌面开始，每轮依次进入设置和计算器，每个应用进最近任务后返回桌面，做四轮。Scudo 主分配区私有脏页从 199292 KiB 到 200336 KiB（静置 30 秒）。另取 35 秒 heapprofd，16 KiB 采样间隔，覆盖三轮热切换；记录区间内估算累计正向分配 185.17 MiB、净增 0.98 MiB。附加到运行中进程不能还原它在启动时的所有分配；部分栈未完整展开。
- [A 默认](default1.csv) → [B 关闭预热](no_prime.csv) → [A 默认复测](default2.csv)：每次重启 SurfaceFlinger（该镜像的 init 也重启 Zygote）；同一容器、同一桌面、未清磁盘缓存。进入桌面并等待后做同样四轮操作，回桌面静置 30 秒。逐次核对前台包名和 SF PID。
- 最后一点 SF PSS：265.35 / 95.51 / 265.14 MiB。主分配区私有脏页：193.15 / 23.84 / 193.10 MiB；次分配区均为 27.94 MiB。三组最后均为 19 个 RenderEngine 跟踪缓冲、19 个导入缓冲。启动预热是这一场景中可避免的约 170 MiB 常驻量的主因；仍不能细分为存活对象、碎片和分配器未归还页。这不是 GPU 显存计数，也不是整台 WSL 的内存承诺。
- [显示帧统计](frames.csv)：Android Perfetto 的 `actual_frame_timeline_slice`，只取本轮 SF PID 的 DisplayFrame，完整落在每轮动作窗口中的有效帧，非 Windows 呈现间隔。首轮 P99 32.42 / 40.49 / 31.78 ms，四轮合计 P95 17.89 / 18.05 / 17.61 ms。提示首次使用着色器可能有尾部代价；每个条件只有有限的独立启动，不能证明所有应用或 GPU 无回退。尚未测游戏和清空磁盘缓存后的第一次启动。
- 本轮不改后台保活，也不做全屏图层直通。否则内存和帧时差异难以归因。

## KernelSU 模块

[模块源码、构建、启用与回退方法](../../README.md)。

### 整容器冷启动验收

已使用本机 KernelSU 3.3.1 的模块安装器安装 ZIP，不是临时写属性代替验收。分别在模块启用、禁用后以原有 ratio / 1920×1080 配置重启整个容器，两次均核对 Android init 已换新、模块状态和属性。进入桌面后，启用 / 禁用的 SF PSS 分别为 **93.99 / 264.31 MiB**；启动设置和计算器、进最近任务并返回后分别为 **96.22 / 264.29 MiB**。两组都报告原 D3D12 GPU renderer。在 Windows 侧检查了桌面和应用窗口的多张截图，未发现静态显示异常；这不等于逐帧消除了首次编译卡顿。[启用](ksu_on.csv) · [禁用](ksu_off.csv)

这是独立的可选模块。禁用时，本镜像未设置预热属性，SurfaceFlinger 使用镜像默认逻辑。它不更改后台进程策略或 Weston。

---

<a id="english"></a>

# SurfaceFlinger startup shader warm-up: memory and first transitions

Environment: 1920×1080, 60 Hz, Waydroid x86_64 / Android 16, Skia GLES → Mesa D3D12. Settings and Calculator came with the image. Only `service.sf.prime_shader_cache` changed. We did not modify the on-disk shader cache, disable shader compilation or interfere with apps' or games' own pipeline warm-up.

## Comparison and findings

- [Original session](warm1.csv): starting from the same Home screen, each round opened Settings and then Calculator; from each app we opened Recents and returned Home. After four rounds, Scudo primary private dirty pages had gone from 199292 KiB to 200336 KiB (after 30 seconds idle). A separate 35-second heapprofd capture with a 16 KiB sampling interval covered three rounds of warm switching. Estimated cumulative positive allocations were 185.17 MiB and net growth 0.98 MiB. Attaching to a running process cannot reconstruct all startup allocations; some stacks were incompletely unwound.
- [A: default](default1.csv) → [B: prewarm off](no_prime.csv) → [A: repeat default](default2.csv): each run restarted SurfaceFlinger (init also restarts Zygote on this image). The container and Home screen were the same and the disk cache was not cleared. After reaching Home and waiting, we repeated the same four rounds, returned Home and waited 30 seconds. The foreground package and SF PID were checked each time.
- Final SF PSS: 265.35 / 95.51 / 265.14 MiB. Primary private dirty pages: 193.15 / 23.84 / 193.10 MiB; the secondary was 27.94 MiB in all runs. Each ended with 19 buffers tracked by RenderEngine and 19 imported buffers. Startup warm-up accounts for most of the roughly 170 MiB of avoidable resident memory in this scenario; the measurements do not distinguish live objects, fragmentation and pages retained by the allocator. This is neither a VRAM figure nor a memory commitment for all of WSL.
- [Display-frame statistics](frames.csv): from Android Perfetto's `actual_frame_timeline_slice`, we selected valid DisplayFrames from that run's SF PID falling entirely within each action window, not Windows presentation intervals. First-round P99 was 32.42 / 40.49 / 31.78 ms; P95 across four rounds was 17.89 / 18.05 / 17.61 ms. This suggests a possible tail cost on first shader use. With few independent starts per condition, it cannot rule out regressions in other apps or on other GPUs. Games and the first launch after clearing the disk cache remain untested.
- This experiment did not change background-process retention or bypass composition of full-screen layers; changing either would confound memory and timing comparisons.

## KernelSU module

[Module sources, build, enable and disable](../../README.md#english).

### Full container cold-start verification

We installed the ZIP using the local KernelSU 3.3.1 module installer, not by temporarily setting a property. With the module enabled and then disabled, we restarted the full container in the existing ratio / 1920×1080 configuration. Both times we confirmed a new Android init process, the module state and the property. After reaching Home, SF PSS was **93.99 / 264.31 MiB** enabled / disabled. After launching Settings and Calculator, opening Recents and returning, it was **96.22 / 264.29 MiB**. Both reported the original D3D12 GPU renderer. Multiple Windows screenshots of Home and app windows showed no static display anomalies; this does not rule out first-use compilation stutter frame by frame. [Enabled](ksu_on.csv) · [Disabled](ksu_off.csv)

This module is independent and optional. When disabled, the tested image leaves the warm-up property unset and SurfaceFlinger uses the image default. It changes neither background-process policy nor Weston.
