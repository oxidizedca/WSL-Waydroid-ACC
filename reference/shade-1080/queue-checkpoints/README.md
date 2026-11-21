# 输入 broker：导入或 CPU 回填前先处理一个就绪 GET

[English](#english)

**仅保留实验补丁。** 队首 GET 等待缩短，但目标场景（展开通知栏）的尾帧没有改善。A 为现行 broker；B 在 GPU 导入、新帧 CPU 回填、淘汰旧帧回填之前各最多尝试处理一个队首连接，不主动等待。只接收另一个窗口已提交的 lazy 代次，排除当前发布 ID 和正在替换的槽，沿用原处理函数验证对端及平面 fd。非 GET、包尚未就绪或需校验 CPU 镜像的连接留在队首。没有调整淘汰策略或跳过回填。

## 对照结果

同一次 Waydroid 启动、同一 SurfaceFlinger，1920×1080、60 Hz、计算器前台。仅切换宿主输入 broker。顺序 A1、B1、B2、A2、B3、A3；每次重启 broker 后先预热 8 轮，最后一对预热 16 轮。用 [shade.py](../../../validation/shade.py) 再预热 2 轮，计时 6 轮。显示帧测试不挂函数探针；每组之后再独立做 6 轮双端函数采样。表中按版本合并逐帧或逐请求样本重算 P95，不能将某一次 GET 直接归因于另一轮里的某个显示帧。

| 指标 | A | B |
|---|---:|---:|
| 展开 GET 等待分派 P95 | 3.710 ms | 2.848 ms |
| 展开 GET 端到端 P95 | 3.847 ms | 3.027 ms |
| 展开时发布请求往返 P95 | 7.437 ms | 7.606 ms |
| 展开显示帧 P95 | 15.646 ms | 16.015 ms |
| 展开超过 1000/60 ms | 23/693 | 29/688 |
| 收起显示帧 P95 | 14.313 ms | 13.375 ms |
| 收起超过 1000/60 ms | 2/693 | 1/698 |

按展开、收起合计，GET 等待分派 P95 为 3.086 / 2.484 ms，发布往返 P95 为 5.432 / 5.476 ms。后者从生产者发送完整发布包前计至收到回复，不包括 dequeue、导出或后续 queue。加长预热的末对展开 P95 为 15.970 / 16.135 ms，两版都出现 8 个超预算帧。本次没有测整容器 CPU 或内存。

新增检查点实际提前处理了 158 个成功的 GET：131 个在导入前、17 个在新帧回填前、10 个在淘汰回填前。均核对为嵌套在不同 buffer ID 的发布之内、返回 lazy 纹理。包括原有可见性查询期间的处理，A 提前处理 86 次，B 为 210 次；158 次并非相对原版的净增量。

## 慢帧中的图层关联

在未挂探针的 Perfetto 记录中，按同一条 trace 内的 display_frame_token 把各应用 surface 帧与 SurfaceFlinger 显示帧关联。展开的 52 个超预算帧中，41 个关联了 **SystemUI StatusBar** 的 surface 帧（79%）；1,329 个未超预算帧中只有 178 个关联（13%）。分版本也有同样方向。相比之下，Taskbar 在两组都经常出现。这里统计的是同一次显示提交，不是图层是否可见，也不是因果证明。这些数据不能区分状态栏出帧晚、等待缓冲区和多图层同时合成。

Windows 侧抽查了试验组的通知栏和计算器。

[候选源码差异](candidate.patch) · [每组结果](summary.csv) · [显示帧](frames.csv) · [双端成功 GET 分段，µs](gets.csv) · [生产者往返，µs](pubs.csv) · [检查点命中及回退计数](counts.csv) · [展开帧的图层关联](layers.csv)

---

<a id="english"></a>

# Input broker: service one ready GET before import or CPU backfill

**Experimental patch only.** Head-of-queue GET wait decreased but the target shade-opening tail did not improve. A was the existing broker. B made one nonblocking attempt at the front connection before GPU import, new-frame backfill and eviction backfill. Only a committed lazy generation from another window qualified, excluding the active publish ID and slot being replaced. The normal handler still checked the peer and plane fd. Non-GET, incomplete or CPU-validation requests remained at the head. Eviction and mandatory backfill were unchanged.

## Comparison

Same Android boot and SurfaceFlinger, Calculator in foreground at 1920×1080/60 Hz. Only the host broker was switched: A1/B1/B2/A2/B3/A3. Eight warm-ups after each restart (16 for the final pair), then two more via [shade.py](../../../validation/shade.py) and six measured cycles. No function probes during display timing; six separate cycles sampled both endpoints after each group. P95s are recomputed from pooled frames or requests by version. A particular GET cannot be attributed to a display frame recorded in a different run.

| Metric | A | B |
|---|---:|---:|
| Open: GET dispatch wait P95 | 3.710 ms | 2.848 ms |
| Open: end-to-end GET P95 | 3.847 ms | 3.027 ms |
| Open: publish round trip P95 | 7.437 ms | 7.606 ms |
| Open: display-frame P95 | 15.646 ms | 16.015 ms |
| Open: over 1000/60 ms | 23/693 | 29/688 |
| Close: display-frame P95 | 14.313 ms | 13.375 ms |
| Close: over 1000/60 ms | 2/693 | 1/698 |

Combined open and close: GET dispatch-wait P95 3.086 / 2.484 ms, publish round-trip P95 5.432 / 5.476 ms. The latter runs from just before sending the full publish request to its reply, excluding dequeue, export and later queue. In the longer-warmed final pair, open P95 was 15.970 / 16.135 ms; both versions had eight over-budget frames. Whole-container CPU and memory were not measured.

The new checkpoints serviced 158 successful GETs early: 131 before import, 17 before new-frame backfill, 10 before eviction backfill. Each returned a lazy texture inside a publish for a different buffer ID. Including the original visibility-query path, A serviced 86 early and B 210; 158 is not the net increase over A.

## Layer association in slow frames

In unprobed Perfetto traces, display_frame_token associated app surface frames with SurfaceFlinger display frames within the same trace. Of 52 over-budget opening frames, 41 (79%) had a **SystemUI StatusBar** surface frame; only 178/1,329 on-budget frames did (13%). Both versions showed the same direction. Taskbar appeared frequently in both groups. This is co-submission, not layer visibility or causation. It cannot distinguish late status-bar production, waiting on buffers and multilayer composition.

Windows screenshots spot-checked the test shade and Calculator.

[Experimental diff](candidate.patch) · [Results by group](summary.csv) · [Display frames](frames.csv) · [Successful GET timings at both ends, µs](gets.csv) · [Producer round trips, µs](pubs.csv) · [Checkpoint hits and fallback counts](counts.csv) · [Layers associated with opening frames](layers.csv)
