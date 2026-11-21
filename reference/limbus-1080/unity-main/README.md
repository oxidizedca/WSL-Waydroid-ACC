# UnityMain 是在计算还是等待？

[English](#english)
2026-11-20，Limbus Company 1.119.1，同一运行中的游戏进程；未重启、未换库、未改线程数。两轮使用不同周期的 Linux 用户态 CPU 时钟采样，选取实际最忙的 UnityMain，而非只按线程名取第一条。同取 schedstat；第二轮并行读 SurfaceFlinger 图层，新缓冲区就绪 559 帧 / 17.017 秒（32.85 FPS）。

- 两轮主线程分别有 98.55% / 98.52% 的墙钟时间在 CPU 上，已就绪但等调度约 0.30% / 0.26%。这排除了长时间睡眠为常态，但单看这一项不能排除自旋。
- 使用与运行库 Build ID 一致的 Digitalis 调试符号，对翻译缓存中的 host 起点与长度、guest PC 和转译层级做只读匹配。只接受前后映射一致、范围唯一且再次读取有效的表项。归属率见[两轮统计](summary.csv)，分母是全部用户态 CPU 样本，而不是帧数。
- 两轮约 79% 的样本来自 libunity.so，约 17–18% 来自 libil2cpp.so；各命中 2583 / 1863 个不同的翻译块。占比最多的块在两轮中一致：从表中读取元素、做范围比较，每次前进 16 字节并递减剩余长度。不是反复读取同一个标志，不需要另一个线程改值才能退出。其他高频块包含该查找的调用点、折半查找、向量算术。另见涉及原子访问的块，不能据此判定游戏完全没有锁竞争。[热点前 20（文件偏移，不是符号名）](hot-blocks.csv)
- 判断限于当前场景：主线程以执行 Unity 引擎工作为主，未见单个自旋循环主导。不能还原所有等待、确定 Unity 子系统名称，也不能估算原生 ARM 与转译执行的时间差。针对 futex/yield 的额外事件计数未通过已知事件对照，因此不使用其零值作证据。

**结论边界：** 这些样本可定位 UnityMain 的热点，但没有同场景的转译模式或线程数 A/B，不能据此量化增加线程的收益。基线未调整系统线程数或全局转译阈值。

---

<a id="english"></a>

# Is UnityMain computing or waiting?
2026-11-20, Limbus Company 1.119.1, same live process; no restart, library change or thread-count change. Two Linux userspace CPU-clock samples at different periods targeted the busiest UnityMain, not the first thread of that name. Both included schedstat. Concurrent SurfaceFlinger layer sampling in the second run counted 559 new ready buffers / 17.017 s (32.85 FPS).

- The main thread ran on CPU for 98.55% / 98.52% of wall time in the two runs and was runnable but waiting for about 0.30% / 0.26%. Long sleeps were not typical, though that fact alone cannot exclude spinning.
- Matching Digitalis debug symbols were used to read translation-cache host starts and lengths, guest PCs and translation tiers. An entry was accepted only if mappings were stable, the range was unique and a reread still validated it. Attribution percentages in the [Statistics for both runs](summary.csv) use all userspace CPU samples, not frames, as the denominator.
- About 79% of samples landed in libunity.so and 17–18% in libil2cpp.so, hitting 2583 / 1863 distinct translated blocks across the two runs. Both had the same hottest block: read a table element, compare a range, advance 16 bytes and decrement remaining length. It does not repeatedly poll a flag or need another thread to let it exit. Other hot blocks contained callers of that lookup, binary search and vector arithmetic. Atomic accesses also appeared, so contention cannot be ruled out. [Top 20 hotspots (file offsets, not symbols)](hot-blocks.csv).
- In this scene the main thread mostly did Unity engine work; no single spin loop dominated. The samples cannot reconstruct all waits, name the Unity subsystem or measure native ARM versus translated execution time. Extra futex/yield counters failed a known-event control; their zero values are not evidence.

**Scope:** these samples locate UnityMain hotspots. Without a same-scene A/B for translation mode or thread count, they cannot quantify what more threads would gain. The baseline changes neither system thread count nor global translation thresholds.
