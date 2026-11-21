# 共享纹理的显示帧耗时

[English](#english)

测量覆盖 Android EGL、Gallium、输入 broker 和物理显示 HWC。验证设备为 RX 7900 XTX，Android 物理分辨率为 1920×1048。三项可独立切换。下文的三位数字依次代表同代输入缓存、broker 查询期间响应 GET、物理目标预分配。000 和 111 使用同一套二进制。

## 从应用到显示

| SurfaceFlinger 实际显示帧耗时，ms | 全关 P50，两批 | 全开 P50，两批 | 全关 P95，两批 | 全开 P95，两批 |
|---|---:|---:|---:|---:|
| 设置滚动 | 11.53 / 11.43 | 6.97 / 7.08 | 12.59 / 12.71 | 8.62 / 8.84 |
| 计算器上展开、收起通知栏 | 10.98 / 11.08 | 9.16 / 9.33 | 18.16 / 19.31 | 16.45 / 17.68 |

两批以相反顺序比较 000、101、111。每个组合、每个场景分别取三组往返，独立截取 SurfaceFlinger 的 FrameTimeline 显示帧及物理屏 drawLayersInternal。设置场景的物理合成 P50 从 10.59 / 10.50 ms 降到 6.09 / 6.16 ms。通知栏全开后，显示帧 P95 在第二批仍达到 17.68 ms。Perfetto 使用完整保留的轨迹，每格有 217–235 个显示帧或合成样本。[两轮逐组数据](display.csv) · [采集与筛选](../validation/display.py)

另有六种组合的正反顺序测试，计时期间关闭诊断日志和录屏。分开看应用 HWUI 的 FrameCompleted − IntendedVsync，设置中位数保持在约 5.8–6.0 ms；SystemUI 通知栏帧时波动更大，未见稳定的全程降幅。这轮能确认的改善集中在 SurfaceFlinger。[HWUI 独立采样](hwui.csv)

## 三项分别改了什么

1. **RenderEngine 复用不可变的同代输入。** broker 已验证的延迟镜像输入按 EGLImage、Mesa context、分配 ID、nonce 和 serial 匹配，同时校验 screen 和尺寸。命中仍把资源绑定到调用者当前 GL 纹理对象。进程最多持有 16 个引用，通过代次变化、容量淘汰、EGLImage 销毁及 context 销毁释放。CPU 可写镜像沿用内容校验。第一批设置中，仅开这一项就把 fd 缓存命中后的绑定 P50 从 0.757 ms 降到 0.0014 ms。三项全开时，654 次设置命中和 228 次通知栏命中均使用资源缓存；首次取到新代仍需导入。[资源管理](../mesa/src/mesa/state_tracker/resources.h) · [Mesa 接入](../mesa/src/mesa/state_tracker/st_cb_readpixels.c) · [Android 与 broker 接入](../src/broker.c)
2. **broker 在 GPU 查询期间服务其他窗口。** 已协商的系统窗口发出可见性查询后，单个 GL 线程检查结果，间隙可响应另一个窗口、不同分配上已经提交的延迟镜像 GET。每次查询最多四轮，单次监听等待预算 100 µs。遇到发布、CPU 镜像请求、同窗口或尚未收到完整报文的连接，保留队首，交回原处理顺序。发布方等可见性及恢复条件满足才收到 ACK。实际采样的首 6,496 次查询间隙共分派了 254 个 GET。保留其他两项，仅切换 broker，第二批设置 GET 从发送到 broker 收到完整请求头的 P95 为 1.933 → 1.926 ms，通知栏为 3.007 → 2.979 ms。通知栏发布 ACK 的 P95 在这批为 6.034 → 6.134 ms，另一批为 6.453 → 5.107 ms。这个场景证实了并行服务，尚不足以给 broker 单项归因稳定的尾延迟收益。[计数](broker-counts.txt) · [首批分段](stages.csv) · [反序分段及请求排队](stages-reverse.csv)
3. **物理 CLIENT 快照预备一个独立目标。** 每个参与导出的 Mesa context 至多准备一个未发布的 RGBA8 目标。工作线程创建目标并导出共享句柄，RenderEngine 持有原 GL context，从当前 FBO 发出 blit、flush 并返回本次完成信号。尺寸改变、预备尚未完成或资源不可用时走同步分配或原有镜像回退。已经交给消费者的目标不会回到预备槽。仅开这项时，设置场景目标取得 P50 从 0.619 ms 降到 0.019 ms，提交后的句柄与就绪信号处理从 0.085 ms 降到 0.0008 ms。三项全开时设置物理导出的创建、提交、句柄三段合计 P50 为 1.54 ms，全关为 3.19 ms；通知栏为 1.15 ms 和 1.91 ms。GPU 拷贝和完成同步仍有成本。[实测分段](stages.csv) · [独立目标实现](../mesa/src/mesa/state_tracker/resources.h)

## 正确性与画面

- 离屏验证在两个尺寸间切换，保留 12 代不同纯色快照，等待各代真实 ready fd 后反序读取，逐像素核对了每份内容。重复绑定命中、切换代次首次失配、显式销毁后重新导入以及 context 退出均走过。采样期间启用了首帧恢复和 Weston 导入失败恢复。[离屏用例](../validation/check.c) · [本机离屏计时](offscreen.csv)
- Windows 侧对 Weston 单独录了 12 秒、720 帧的 60 Hz 视频。操作覆盖设置双向滚动、切换计算器、通知栏展开收起、进入最近任务和返回桌面。对每一帧缩小采样后的 Android 内容区检查近黑覆盖率和跨屏黑条，并核看动作切换及变化最大的画面。这段录像未发现全黑或横向闪黑。少量像素的瞬态瑕疵、冷启动 splash 和游戏场景没有据此验收。[原始录屏](111-60fps.mp4) · [画面索引](111-60fps.jpg) · [逐帧记录](111-60fps.csv) · [桌面](home.jpg) · [设置](settings.jpg) · [通知栏](shade.jpg)

## 测量口径

[开关与分段计时脚本](../validation/measure.py)和[Windows 录屏脚本](../validation/record.py)对应本次 1920×1048 的采样。探针偏移与特定二进制哈希绑定；这组数字不是对任意构建的性能保证。当前的构建、部署及窗口参数见[项目 README](../README.md)。

---

<a id="english"></a>

# Shared textures: display-frame timings

Measurements cover Android EGL, Gallium, the input broker and physical-display HWC on an RX 7900 XTX at 1920×1048. Three switches were independently controllable. From left to right the digits denote same-generation input caching, serving GETs during broker queries and physical-target preallocation. Configurations 000 and 111 used the same binaries.

## From app to display

| SurfaceFlinger display-frame time, ms | All off P50, two batches | All on P50, two batches | All off P95, two batches | All on P95, two batches |
|---|---:|---:|---:|---:|
| Settings scrolling | 11.53 / 11.43 | 6.97 / 7.08 | 12.59 / 12.71 | 8.62 / 8.84 |
| Open/close shade over Calculator | 10.98 / 11.08 | 9.16 / 9.33 | 18.16 / 19.31 | 16.45 / 17.68 |

Two batches compared 000, 101 and 111 in opposite orders. Three round-trip groups per configuration/scene independently captured SurfaceFlinger FrameTimeline display frames and physical-display drawLayersInternal. Settings physical composition P50 fell from 10.59 / 10.50 to 6.09 / 6.16 ms. Shade display-frame P95 with all switches enabled still reached 17.68 ms in batch two. Perfetto traces were retained intact, with 217–235 display-frame or composition samples per cell. [Groups in both batches](display.csv) · [Collection and filtering](../validation/display.py).

A separate six-configuration forward/reverse test disabled diagnostic logs and recording while timing. In app HWUI (FrameCompleted − IntendedVsync), Settings stayed around 5.8–6.0 ms median. SystemUI shade timings varied more, without a stable end-to-end gain. The confirmed improvement was concentrated in SurfaceFlinger. [Independent HWUI samples](hwui.csv).

## What the three optimizations change

1. **RenderEngine reuses immutable inputs within a generation.** Broker-validated deferred-mirror inputs match on EGLImage, Mesa context, allocation ID, nonce and serial, plus screen and dimensions. A hit still binds the resource to the caller's current GL texture. Each process holds at most 16 references, released on generation change, eviction, EGLImage destruction or context destruction. CPU-writable mirrors still undergo content validation. In the first Settings batch, this change alone cut bind P50 after an fd-cache hit from 0.757 to 0.0014 ms. With all switches on, 654 Settings hits and 228 shade hits used the resource cache; a new generation still required its first import. [Resource management](../mesa/src/mesa/state_tracker/resources.h) · [Mesa integration](../mesa/src/mesa/state_tracker/st_cb_readpixels.c) · [Android and broker integration](../src/broker.c).
2. **The broker services other windows during GPU queries.** After a negotiated system window starts a visibility query, the single GL thread can serve a committed deferred-mirror GET for a different window and allocation between polls. Up to four rounds per query, with at most 100 µs of listener wait per round. Publish, CPU-mirror, same-window or incomplete requests stay at the queue head for normal processing. The publisher receives ACK only after visibility and recovery conditions are met. The first 6,496 query gaps dispatched 254 GETs. Holding the other two switches on, batch-two P95 from GET send to receipt of the full header changed 1.933 → 1.926 ms in Settings and 3.007 → 2.979 ms for the shade. Shade publish ACK P95 was 6.034 → 6.134 ms in that batch and 6.453 → 5.107 ms in the other. Concurrent service was demonstrated, not a consistent tail-latency gain attributable to the broker alone. [Counts](broker-counts.txt) · [First batch by stage](stages.csv) · [Reverse-order stages and queueing](stages-reverse.csv).
3. **Prepare an independent physical CLIENT snapshot target.** Each exporting Mesa context prepares at most one unpublished RGBA8 target. A worker allocates it and exports its handle; RenderEngine retains the original GL context, blits from the current FBO, flushes and returns that submission's completion signal. Size changes, unfinished preparation or unavailable resources fall back to synchronous allocation or the existing mirror. Targets handed to consumers never return to the prepared slot. With only this switch on in Settings, target acquisition P50 fell 0.619 → 0.019 ms and post-submit handle/readiness processing 0.085 → 0.0008 ms. P50 for combined create/submit/handle stages, all on vs off: Settings 1.54 vs 3.19 ms; shade 1.15 vs 1.91 ms. GPU copy and synchronization remain. [Stage measurements](stages.csv) · [Independent target implementation](../mesa/src/mesa/state_tracker/resources.h).

## Correctness and visual checks

- The offscreen test alternated two sizes, retained 12 differently colored generations, waited on each real ready fd and read them back in reverse order, checking every pixel. It covered repeat hits, first misses on a new generation, reimport after explicit destruction and context exit. First-frame and Weston import-failure recovery were enabled during sampling. [Offscreen test](../validation/check.c) · [Local offscreen timings](offscreen.csv).
- A Windows recording isolated Weston for 12 s at 60 Hz (720 frames), covering bidirectional Settings scrolling, switching to Calculator, shade open/close, Recents and Home. Each downsampled Android content area was screened for near-black coverage and horizontal bands; transitions and the largest changes were also inspected. No full-black frame or horizontal flash appeared in this recording. It does not validate small transient artifacts, cold-launch splash or games. [Original recording](111-60fps.mp4) · [Frame index](111-60fps.jpg) · [Per-frame log](111-60fps.csv) · [Home](home.jpg) · [Settings](settings.jpg) · [Notification shade](shade.jpg).

## Measurement scope

The [Switches and stage timing](../validation/measure.py) and [Windows recording script](../validation/record.py) correspond to the 1920×1048 samples. Probe offsets are pinned to particular binary hashes; the measurements are not a performance guarantee for other builds. See the [Project README](../README.en.md) for current build, deployment and window options.
