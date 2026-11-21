# 明日方舟：Windows 合成队列是否拖慢 Android 帧就绪？

[English](#english)

2026-11-21，沿用 1920×1080 / 60 Hz、RX 7900 XTX 的现场。明日方舟前台自然运行，采集前后焦点相同；未操控游戏、重启、改队列优先级或部署模块。以下 Windows ETW、Android Perfetto 和游戏图层就绪时间来自**同一次采集**。

## 结果

- 16 秒公共采样窗内有 907 个连续游戏图层就绪间隔，P50 / P95 / 最大为 16.87 / 27.32 / 44.04 ms，52 个超过 25 ms。这是 BLAST 的就绪间隔，不是显示器帧时。
- Windows 侧 SF 有 1818 个完整的普通硬件队列提交包。入队→硬件提交 P95 为 0.054 ms；入队→完成通知 P95 / 最大为 0.591 / 1.194 ms。52 个长间隔中，51 个与 SF 提交包重叠，另 1 个没有；重叠包从入队到完成最长 0.851 ms。这里比较的是单包而非整帧总 GPU 时间，未将没有包解释成零耗时的包。
- 896 个游戏图形线程的完整 `eglSwapBuffers` 调用与就绪帧一对一匹配，得到 895 个相邻间隔，覆盖全部 52 个长间隔。**上一 swap 返回到下一 swap 进入**：长间隔中位 23.64 ms（46/52 超过 20 ms），其余 843 个间隔中位 11.63 ms（1/843 超过 20 ms）。长间隔内 `waitForBufferRelease` 的重叠时长仅 4/52 超过 5 ms，但最重达 25.10 ms，故缓冲归还可解释部分尖峰，不是多数长间隔的共同点。
- 最长的 44.04 ms 间隔里，39.16 ms 在两次 swap 之间，后一次 swap 自身 4.94 ms；没有观察到等待缓冲归还，重叠的两个 SF 包最长 0.410 ms。另一个 31.86 ms 的间隔则确实有 25.10 ms 落在等待缓冲归还里。不要用单一原因解释两类样本。
- 结论限于当前前台场景：没有看到 SF 的宿主 GPU 队列积压足以解释长间隔，**不据此改 HIGH**。优先诊断两次 swap 之间游戏图形线程在做什么：主动限帧、等待主线程/同步、实际运行应区分，当前不能据此认定 ARM 转译或 GPU 执行慢。

## 复核口径

1. Windows 使用独立的 DxgKrnl ETW 会话和 capture-state。用 Process/DC_Start 的虚机进程身份 → Device.DxgProcess → Context.hDevice → HwQueue.hContext / ParentDxgHwQueue 连接宿主硬件队列。实际识别到 UnityMain、surfaceflinger、weston；不是按 Windows 调用线程或相似帧率猜归属。仅分析这些进程有活动的硬件队列，不把其他引擎、信号包等都算进去。
2. 用队列及 ProgressFenceValue 配对 178（PacketType 0）、450（成功）、451；采样窗内完整三联才进[包耗时分布](gpu-summary.csv)。450→451 包含可能的硬件队列等待、执行和完成通知延迟，不能叫作纯 GPU 执行时间。调度日志的 context 状态没有建立到每个提交包首次实际执行的一对一映射。也没有关联 Weston 的内容帧到 Windows 实际呈现。[微软 GPUView 队列说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-hardware-queue)
3. Android 取游戏 SurfaceView BLAST 的 `--latency` 第三列，重复快照按就绪时间去重，只保留 Perfetto 窗内的相邻间隔。Android 端以 boot 时钟对齐；Windows UTC 在窗前后各与 WSL 的 CLOCK_BOOTTIME 握手 12 次，分别取最短 RTT 的中点做线性映射。两端最短 RTT 约 0.210 / 0.208 ms，半 RTT 约 0.105 / 0.104 ms；不要解释跨系统几十微秒的先后关系。包内部耗时来自同一宿主时钟。
4. 游戏就绪点取距 swap 返回最近且不超过 1 ms 的记录，检查没有重复使用，且相邻调用连续；匹配绝对误差 P95 0.037 ms。另有 12 个间隔缺少完整 swap 对，应用列留空，不按零等待计入分组。[逐间隔明细](ready-intervals.csv)以相对 trace 起点的毫秒数表示；重叠等待列是区间交集，swap、dequeue 和 release wait 可嵌套，不要相加。显示帧列是与间隔相交的最长 SF DisplayFrame，不是与游戏一对一的 present。
5. ETW 查询的 EventsLost 为 0；Perfetto 报告 1 个 traced_chunks_discarded，不宣称整条 trace 无损。未以不可依赖的 HAL present fence 推算物理显示帧率。仓库只保存汇总及相对时间，不保存整机 ETL 和进程/设备句柄。

---

<a id="english"></a>

# Arknights: does the Windows composition queue delay Android frame readiness?

2026-11-21, 1920×1080 / 60 Hz, RX 7900 XTX. Arknights ran normally in the foreground, with the same focus before and after capture. We did not control the game, restart it, change queue priorities or deploy a module. The Windows ETW, Android Perfetto and game-layer ready timestamps below come from **the same capture**.

## Results

- The common 16-second window contained 907 consecutive game-layer ready intervals. P50 / P95 / maximum: 16.87 / 27.32 / 44.04 ms; 52 exceeded 25 ms. These are BLAST ready intervals, not monitor frame times.
- We identified 1818 complete ordinary SF hardware-queue packets on Windows. Enqueue to hardware submission had a P95 of 0.054 ms; enqueue to completion notification had a P95 / maximum of 0.591 / 1.194 ms. Of the 52 long intervals, 51 overlapped SF packets and one did not. The longest overlapping packet took 0.851 ms from enqueue to completion. These are single-packet timings, not total GPU time per frame. Absence of a packet is not a zero-duration packet.
- We matched 896 complete `eglSwapBuffers` calls from the game graphics thread one-to-one with ready frames. The resulting 895 adjacent intervals covered all 52 long intervals. **From the previous swap's return to entry into the next swap**, the median was 23.64 ms in long intervals (46/52 above 20 ms), versus 11.63 ms in the other 843 intervals (1/843 above 20 ms). Overlap with `waitForBufferRelease` exceeded 5 ms in only 4/52 long intervals but reached 25.10 ms. Buffer return accounts for some spikes, not the common pattern.
- In the longest interval (44.04 ms), 39.16 ms elapsed between swaps and the second swap took 4.94 ms. There was no observed buffer-release wait, and the longer of two overlapping SF packets took 0.410 ms. By contrast, a 31.86 ms interval included 25.10 ms of buffer-release wait. A single cause does not explain both cases.
- For this foreground scene, no backlog in SF's host GPU queue was large enough to account for the long intervals. **These results do not justify switching to HIGH.** The next question is what the game graphics thread does between swaps: deliberate frame pacing, waiting on the main thread or synchronization, and active execution must be distinguished. The data do not establish slow ARM translation or GPU execution.

## How to reproduce the analysis

1. Windows used a dedicated DxgKrnl ETW session and capture-state. Host hardware queues were attributed through the VM process identity in Process/DC_Start → Device.DxgProcess → Context.hDevice → HwQueue.hContext / ParentDxgHwQueue. UnityMain, surfaceflinger and weston were identified this way, not guessed from Windows caller threads or similar frame rates. We analyzed their active hardware queues, not every engine or signal packet.
2. We paired events 178 (PacketType 0), 450 (success) and 451 by queue and ProgressFenceValue. Only complete triples within the window enter [Packet timing distribution](gpu-summary.csv). The 450→451 interval may include hardware-queue waiting, execution and notification latency; it is not pure GPU execution time. Scheduler context states were not matched one-to-one to the first actual execution of each packet. Weston content frames were not correlated with actual Windows presentation. [Microsoft GPUView queue documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-hardware-queue)
3. On Android, we used the third `--latency` column of the game SurfaceView BLAST, deduplicated snapshots by ready timestamp, and retained adjacent intervals within the Perfetto window. Android data use the boot clock. Before and after the window, Windows UTC was paired with WSL CLOCK_BOOTTIME in 12 handshakes each; the midpoint of the shortest RTT at each end defined a linear mapping. Minimum RTTs were about 0.210 / 0.208 ms, half-RTTs 0.105 / 0.104 ms. Do not infer cross-system ordering tens of microseconds apart. Within-packet timings use the same host clock.
4. Each ready timestamp was matched to the nearest swap return within 1 ms; we checked that records were not reused and adjacent calls were consecutive. Absolute matching error P95 was 0.037 ms. A further 12 intervals lacked complete swap pairs; their app columns are blank, not counted as zero wait. Times in [Per-interval details](ready-intervals.csv) are milliseconds from trace start. Overlapping waits are interval intersections: swap, dequeue and release wait can nest and must not be summed. The display-frame column is the longest SF DisplayFrame intersecting the interval, not a one-to-one game present.
5. ETW reported EventsLost=0; Perfetto reported one traced_chunks_discarded, so we do not claim lossless capture. We did not use the unreliable HAL present fence to infer physical display FPS. Only aggregates and relative times are retained, not the full machine ETL or process/device handles.
