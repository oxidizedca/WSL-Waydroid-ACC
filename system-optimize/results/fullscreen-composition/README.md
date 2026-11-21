# 游戏主图层直通与静态图层缓存（仅调查，未发布模块）

[English](#english)

2026-11-21，Waydroid 1920×1080，现有 Weston 和 Android HWC，未启用 SF 着色器模块。目的：确认能否不丢失界面地用一张游戏纹理代替整张 CLIENT 合成结果。

## 静态采样不等于逐帧覆盖率

每次读取 `dumpsys SurfaceFlinger` 的物理显示 HWC 图层表，间隔约 0.8 秒加 dump 耗时。另从 composition list 对照游戏和附加图层的 buffer 和 frame 编号；DEVICE 光标与 CLIENT 图层分开计数。[逐次采样](snapshots.csv)。

- Blue Archive：45 个启动至游戏画面样本，43 个有游戏 SurfaceView；这 43 个中每次都有 3 个侧边 CLIENT 小层，还可能有全屏 Activity 层或 DEVICE 光标。游戏帧号从 17 到 1818，两个侧层的 buffer/frame 始终不变，另一个侧层在部分时刻更新。0/43 满足“仅有一个 CLIENT 游戏层”的必要条件。采样不能证明这些层的每个像素是否透明，更不能据此删掉它们。
- Limbus Company：36 个样本只观察到启动、提示窗口和回桌面，未取得稳定游戏场景，不作为游戏命中率样本。
- 实现上的区别：现有 HWC 的 `group_physical` 在全 UI 模式下把合格图层组合为 CLIENT 目标，Weston 导入的是其合成输出。普通应用输入纹理已有发布路径，但现有直接呈现特例受 1222 像素宽和票据校验限制。放宽宽度不能解决图层顺序、alpha、颜色空间、同步和失败时保留画面的问题。见 [组合策略](../../../src/policy.h)、[输入发布](../../../src/window.h)、[HWC 呈现](../../../src/present.h)。

## Planner 对照：开启组无有效性能数据

在 Blue Archive 同一会话先采默认段，再通过 SurfaceFlinger 的运行时诊断开关请求启用 layer caching / Planner，不写系统属性。开启段未完成。Weston 在 07:47:06.372 记录客户端通信错误；内核 init 日志随后先报告 `vendor.hwcomposer-2-1`（PID 245）以状态 134 退出，然后重启 SF（300 → 4877）。镜像 HWC 服务明确声明 `onrestart restart surfaceflinger`。容器没有 OOM kill 记录，未取得这次 HWC 的新 tombstone；未知具体断言或 Wayland 请求。不能据此给出开启后的帧时、内存收益或断定错误发生在 Android Planner 本体。

基线使用镜像默认的 Planner 配置，不设开机属性，也不提供该方向的 KernelSU 模块。

## 离屏验证

[实际像素、归还事件与 HWC 元数据线索](offscreen/README.md)。

---

<a id="english"></a>

# Direct game-layer presentation and static layer caching (investigation only; no module)

2026-11-21, Waydroid at 1920×1080 with the existing Weston and Android HWC; the SF shader module was disabled. The question was whether a single game texture could replace the full CLIENT composition target without losing UI elements.

## Snapshot counts do not measure per-frame coverage

We read the physical display's HWC layer table from `dumpsys SurfaceFlinger` at intervals of about 0.8 seconds plus dump time. We cross-checked buffer and frame numbers of the game and additional layers in the composition list, counting the DEVICE cursor separately from CLIENT layers. [Individual samples](snapshots.csv).

- Blue Archive: 45 samples from startup to gameplay; 43 contained the game SurfaceView. All 43 also contained three small side CLIENT layers; some had a full-screen Activity layer or a DEVICE cursor. The game frame number advanced from 17 to 1818. Two side layers kept the same buffer/frame throughout; the third changed on occasion. In 0/43 samples was the game the only CLIENT layer, a necessary condition for the shortcut. Sampling cannot prove that every pixel in those layers is transparent or justify dropping them.
- Limbus Company: 36 samples captured only startup, prompts and a return Home. No stable gameplay scene was captured, so these are not game hit-rate samples.
- The current HWC `group_physical` groups eligible layers into a CLIENT target in full-UI mode; Weston imports that composited output. Ordinary app input textures already have a publication path, but the existing direct-presentation exception is restricted to a width of 1222 pixels and validated tickets. Relaxing the width check would not address layer order, alpha, color space, synchronization, or preserving the displayed frame on failure. See [Composition strategy](../../../src/policy.h), [Input publication](../../../src/window.h) and [HWC presentation](../../../src/present.h).

## Planner comparison: no valid performance data with Planner enabled

In one Blue Archive session we sampled the default first, then requested layer caching / Planner via SurfaceFlinger's runtime diagnostic switch without setting a system property. The enabled run did not finish. Weston logged a client communication error at 07:47:06.372. The kernel init log then reported `vendor.hwcomposer-2-1` (PID 245) exiting with status 134, followed by an SF restart (300 → 4877). The image's HWC service explicitly declares `onrestart restart surfaceflinger`. There was no container OOM kill or new HWC tombstone for the incident; the specific assertion or Wayland request is unknown. We cannot report frame times or memory gains for the enabled case, or attribute the failure to Android Planner itself.

The baseline keeps the image's default Planner configuration. It sets no boot property and does not provide a KernelSU module for this experiment.

## Offscreen validation

[Pixel results, release events and HWC metadata](offscreen/README.md#english).
