# Android / WSL 调度：前台游戏的等待来自哪里？

[English](#english)

2026-11-21，1920×1080 / 60 Hz，明日方舟前台。没有切换画面、重启、启用模块或调整线程/队列优先级。以下为自然运行中的分段采样，不是固定场景的性能对比。

## CPU：关键线程不是长时间处于 runnable

- WSL 看到 Ryzen 7 9800X3D 的 8 核 / 16 线程，游戏线程可用 0–15。容器、apps、游戏 UID、PID 四层 cgroup 的 cpu.max 均为 max 100000、cpu.weight 均为 100，容器 nr_throttled=0。两个 cpuset 属性 default / efficient 都是 0–15。镜像 task_profiles 中 HighPerformance / MaxPerformance 的 Actions 为空（聚合 profile 仍含 IO 优先级、timer slack 等，不能说所有动作都无效）。未编译 CONFIG_UCLAMP_TASK，WSL 没有 cpufreq policy 或 GPU devfreq 设备。
- [12.07 秒线程计数器](cpu-sample.csv)：按 /proc/.../schedstat 前后差取运行和就绪等待，读取实际策略。游戏主线程和图形工作线程均为普通策略，SF 主线程和 RenderEngine 已是 FIFO / 2。
- [单次排队分布](runqueue-summary.csv)：独立 tracefs 实例只采 5 条关键线程的 sched_wakeup、sched_switch，使用 boot 时钟。队列开始取实际 wakeup 或 runnable 状态被切出，结束取下一次 switch-in；丢弃边界未配对事件。使用内核事件中的 TID 和线程名，不把 Android PID 直接当成内核 TID。此次实例的 16 个 CPU 统计均无 overrun/dropped events；先导的较小 buffer 有覆盖，其数据不参与结论。
- [与游戏就绪帧对齐](game-intervals.csv)：按完整落在 trace 时域内的连续 BLAST 帧就绪时间去重，计算每个间隔内两条游戏线程累计处于 runnable 但未获 CPU 的时间。371 个间隔，57.20 就绪帧/秒；P50 / P95 / 最大间隔为 16.95 / 22.51 / 30.61 ms。16 个间隔超过 25 ms，各长间隔内主线程累计排队均不超过 0.212 ms，图形线程不超过 0.312 ms。这不含等锁、等 fence、主动限帧，以及宿主暂停整个 vCPU 等无法从 guest 运行队列独立度量的时间。也不代表 Windows 实际呈现 FPS。
- 另采 20 秒 Android FrameTimeline：1133 个完成的 SF DisplayFrame，时长 P50/P95/P99 为 4.63/5.35/6.25 ms。HAL 标记 PresentFenceIsNotReliable，本次没有可靠的屏幕呈现时刻，不能用全标为 Unknown/Display HAL jank 的记录算掉帧率。SF 配置 app/SF phase 为 2/6 ms、标称 SF work duration 10.67 ms；它们不是单帧实际 CPU/GPU 耗时。

## GPU：队列优先级可请求，但不等于应该调高

- [当前 Mesa 队列创建](../../../mesa/src/gallium/drivers/d3d12/d3d12_screen.cpp)的 D3D12_COMMAND_QUEUE_DESC 零初始化，没有设置 Priority，因此该路径使用 NORMAL（0）。[微软枚举定义](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_command_queue_priority)。
- [只读能力查询源码](queue-capability.cpp)及[输出](queue-capability.csv)：在 WSL 用 DXCore 枚举硬件适配器，RX 7900 XTX 的 D3D12 DIRECT 队列分别查询 NORMAL 和 HIGH（100），均返回 S_OK / supported=1。没有创建队列或提交 GPU 工作。此结果不是 Windows 会按某个比例分配 GPU 时间的保证，也不是提优先级的收益测试。[CheckFeatureSupport 对应结构](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_command_queue_priority)
- Windows 相邻观察窗取 16 个 1 秒样本，每次按适配器及引擎合计各进程 3D 计数后取最高者；范围 8.30%–9.66%。未能从该计数器独立归属给游戏或 WSL。这不能排除毫秒级队列竞争，也不能据此判断所有 GPU 引擎。在 Android 注册的是 example Power HAL；报告支持 hint session，但不支持 CPU/GPU headroom。不要把已注册 ADPF 等同于可调宿主 GPU 时钟。
- **基线选择：** 保留镜像 CPU/GPU 调度策略，不绑核、不修改 governor 或 GPU 队列优先级，也不发布调度模块。

**同一时间窗复核：** [Windows GPU 队列与游戏慢帧逐帧对照](gpu-timeline/README.md)。这次能区分游戏、SurfaceFlinger 和 Weston 的包；52 个长就绪间隔中，46 个在游戏两次交换缓冲之间就已用去超过 20 ms。不支持为了这个场景提高 SF 队列优先级。

复核 WSL 能力查询：安装 DirectX-Headers 后，运行
```sh
g++ $(pkg-config --cflags DirectX-Headers) queue-capability.cpp -L/usr/lib/wsl/lib -ld3d12 -ldxcore -lDirectX-Guids -o /tmp/acc-queue-capability
LD_LIBRARY_PATH=/usr/lib/wsl/lib /tmp/acc-queue-capability
```

---

<a id="english"></a>

# Android / WSL scheduling: where does a foreground game spend time waiting?

2026-11-21, 1920×1080 / 60 Hz, Arknights in the foreground. No scene switches, restarts, modules or thread/queue priority changes. These are samples from different periods of natural gameplay, not a controlled comparison of a fixed scene.

## CPU: key threads do not spend long runnable but off-CPU

- WSL saw 8 cores / 16 threads of the Ryzen 7 9800X3D; game threads could use CPUs 0–15. In the container, apps, game UID and PID cgroups, cpu.max was max 100000 and cpu.weight was 100 at all four levels; container nr_throttled=0. Both default and efficient cpusets were 0–15. HighPerformance / MaxPerformance had empty Actions in the image's task_profiles; aggregate profiles still include I/O priority, timer slack and other actions. CONFIG_UCLAMP_TASK was not built in. WSL exposed neither a cpufreq policy nor a GPU devfreq device.
- [Thread counters over 12.07 s](cpu-sample.csv): runtime and runnable wait are differences between /proc/.../schedstat readings; scheduling policies were read directly. The game main and graphics worker threads had ordinary policy; the SF main and RenderEngine threads already used FIFO / 2.
- [Queue waits by submission](runqueue-summary.csv): an isolated tracefs instance captured only sched_wakeup and sched_switch for five key threads on the boot clock. Queue time started at a real wakeup or a runnable switch-out and ended at the next switch-in. Unpaired boundary events were dropped. We used TIDs and thread names from kernel events, not Android PIDs as kernel TIDs. All 16 CPUs reported no overruns or dropped events in this instance. An earlier, smaller pilot buffer wrapped and was excluded.
- [Aligned to game ready frames](game-intervals.csv): we deduplicated consecutive BLAST ready timestamps fully inside the trace and summed runnable-but-off-CPU time for each of the two game threads in every interval. The 371 intervals yielded 57.20 ready frames/s; P50 / P95 / maximum interval: 16.95 / 22.51 / 30.61 ms. In the 16 intervals above 25 ms, cumulative queuing never exceeded 0.212 ms for the main thread or 0.312 ms for the graphics thread. This excludes lock waits, fence waits, deliberate pacing and time such as a host pause of an entire vCPU that cannot be measured independently from the guest run queue. It is not actual Windows presentation FPS.
- A separate 20-second Android FrameTimeline captured 1133 completed SF DisplayFrames; duration P50/P95/P99 was 4.63/5.35/6.25 ms. The HAL reported PresentFenceIsNotReliable. Without reliable presentation timestamps, frames all marked Unknown/Display HAL jank cannot give a dropped-frame rate. SF's app/SF phase settings were 2/6 ms and nominal SF work duration 10.67 ms; these are not actual per-frame CPU/GPU times.

## GPU: queue priority can be requested; that does not mean it should be raised

- The D3D12_COMMAND_QUEUE_DESC in [Mesa queue creation](../../../mesa/src/gallium/drivers/d3d12/d3d12_screen.cpp) is zero-initialized and does not set Priority, so the path uses NORMAL (0). [Microsoft enum definitions](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_command_queue_priority).
- [Read-only capability probe](queue-capability.cpp) and [Output](queue-capability.csv): a WSL DXCore hardware-adapter query on the RX 7900 XTX returned S_OK / supported=1 for both NORMAL and HIGH (100) on D3D12 DIRECT queues. No queue was created and no GPU work submitted. This neither guarantees a share of GPU time on Windows nor demonstrates a benefit from a higher priority. [CheckFeatureSupport structure](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_command_queue_priority)
- In an adjacent Windows observation window we collected 16 one-second samples, summed each process's 3D counters by adapter and engine, then took the highest total. Values ranged from 8.30% to 9.66%. The counter alone could not attribute this to the game or WSL. It cannot rule out millisecond-scale queue contention or characterize all GPU engines. Android registered the example Power HAL, which reported hint-session support but no CPU/GPU headroom support. Registered ADPF does not mean the host GPU clock is controllable.
- **Baseline decision:** Keep the image's CPU/GPU scheduling policy. No CPU pinning, governor change, GPU queue priority override, or scheduling module.

**Same-window follow-up:** [Windows GPU queues aligned with slow game frames](gpu-timeline/README.md#english). Packets from the game, SurfaceFlinger and Weston could be distinguished. In 46 of the 52 long ready intervals, over 20 ms had already elapsed between game buffer swaps. This does not support raising SF queue priority for this scene.

To repeat the WSL capability query, install DirectX-Headers and run:
```sh
g++ $(pkg-config --cflags DirectX-Headers) queue-capability.cpp -L/usr/lib/wsl/lib -ld3d12 -ldxcore -lDirectX-Guids -o /tmp/acc-queue-capability
LD_LIBRARY_PATH=/usr/lib/wsl/lib /tmp/acc-queue-capability
```
