# Limbus Company：1080p 低帧率现场（2026-11-20）

[English](#english)

运行中只读采样，无重启、无配置或图形库更换。游戏 1.119.1，arm64-v8a；设备显示 1920×1080 / 60 Hz，UID 刷新率覆盖 60 Hz，未发现 Android 游戏模式限帧干预。游戏内部帧率设置未核实。

| 采样 | 游戏 BLAST 图层的新就绪帧 / 时间 | FPS | 最忙 UnityMain 的 CPU（100% = 一个逻辑核） |
|---|---|---:|---:|
| 带 Perfetto | 1009 / 30.567 秒 | 33.01 | 97.9% |
| 不开 Perfetto，仅采计数器 | 449 / 12.759 秒 | 35.19 | 97.1% |

首段就绪间隔中位数 28.71 ms、P95 40.03 ms、最长 612.51 ms。实际呈现字段为 -1，不能当成 Windows 最终呈现 FPS。第一次采样中 swap 已用量增加约 20 MiB；第二次保持不变，低帧率仍在。两段是现场连续观察，不保证场景动作一致。

同次约 32 秒 Perfetto 中，游戏 1048 次 eglSwapBuffers 中位 5.49 ms、P95 6.33 ms。一次返回到下次进入的间隔中位 23.17 ms、P95 33.96 ms，最大 606.71 ms。游戏 dequeueBuffer P95 小于 0.01 ms，未见普遍等槽。游戏的产帧间隔在进入 Weston 之前已经偏长；没有测 Windows 显卡利用率或证明 GPU 没有其他开销。

另取约 12 秒最忙 UnityMain 用户态指令地址样本，按采样前后均稳定的进程映射归属：1926 个样本中 1883 个落在 /memfd:exec (deleted)；Berberis 源码以 CreateMemfdOrDie("exec") 创建可执行代码缓存。运行时 native bridge 为 libberberis_arm64.so。这支持“主要在运行经转译的游戏代码”，不是“97.8% 的时间花在编译”，也不能由此判定 IL2CPP、Unity 中哪个函数最慢或转译引入了多少额外开销。

[带 trace 的就绪帧](ready.csv) · [无 trace 的就绪帧](ready-no-trace.csv) · [最忙线程](threads.csv) · [交换阶段计时](timings.csv)

热点归因：[两轮 UnityMain 热点归属与自旋排查](unity-main/README.md)。

---

<a id="english"></a>

# Limbus Company: low frame rate at 1080p (2026-11-20)

Read-only sampling of the running game; no restart, configuration or graphics-library changes. Game 1.119.1, arm64-v8a, 1920×1080 / 60 Hz, UID refresh override 60 Hz. No Android game-mode frame cap was found. The game's own frame-rate setting was not verified.

| Sample | New ready frames on the game's BLAST layer / duration | FPS | Busiest UnityMain CPU (100% = one logical core) |
|---|---|---:|---:|
| With Perfetto | 1009 / 30.567 s | 33.01 | 97.9% |
| Counters only, no Perfetto | 449 / 12.759 s | 35.19 | 97.1% |

In the first interval the frame-ready gap had median 28.71 ms, P95 40.03 ms and maximum 612.51 ms. Actual present timestamps were -1: these are not final Windows presentation FPS. Swap use rose about 20 MiB in the first sample but stayed constant in the second while the low FPS persisted. The samples were consecutive, not guaranteed identical actions.

In the roughly 32-second Perfetto trace, 1048 game eglSwapBuffers calls had median 5.49 ms and P95 6.33 ms. From return to next entry, median was 23.17 ms, P95 33.96 ms and maximum 606.71 ms. Game dequeueBuffer P95 was below 0.01 ms; slot waits were not pervasive. Frame production was already slow before Weston. Windows GPU utilization was not measured; other GPU costs are not ruled out.

A separate roughly 12-second userspace instruction-address sample targeted the busiest UnityMain. With process mappings stable before and after, 1883 of 1926 samples fell in /memfd:exec (deleted). Berberis creates its executable code cache with CreateMemfdOrDie("exec"); the active bridge was libberberis_arm64.so. This supports “mostly executing translated game code,” not “97.8% of time spent compiling.” It does not identify the slowest IL2CPP or Unity function or quantify translation overhead.

[Ready frames with tracing](ready.csv) · [Ready frames without tracing](ready-no-trace.csv) · [Busiest threads](threads.csv) · [Swap-stage timings](timings.csv)

Hotspot attribution: [Two UnityMain hotspot and spin-loop samples](unity-main/README.md#english).
