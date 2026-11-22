# 未实现与未纳入当前版本的方向

[English](#english)

这里集中记录项目文档中尚未解决的问题、没有打包的 KernelSU 模块方向，以及**已经做过实验但没有采用**的改动。三者并不等价：有补丁或离屏结果，不代表有可安装版本；测不到收益，也不是承诺继续开发。原始测量和实验补丁仅在本地 `out/` 保存。

## Android 系统侧：哪些方向没有打成模块？

| 方向 | 现状与再次评估的条件 |
| --- | --- |
| 全屏游戏主图层直通 | **未实现通用直通。** Blue Archive 有游戏 SurfaceView 的 43 次采样，每次都还有 3 个侧边 CLIENT 图层；不能只交出游戏纹理就假定画面完整。需要逐帧保留附加层的顺序、透明度、颜色空间和同步，失败时仍能显示完整的合成结果。 |
| SurfaceFlinger Planner / 静态图层缓存 | **已做离屏对照，未打包。** 30 张离屏对照帧的 RGB 一致，但实机启用组在 HWC 退出后中断，没有有效的收益数据。尚缺故障帧实际提交的 buffer、上报尺寸和 source crop 的对应关系；不能认定是 Planner 本身或 Weston 过早释放导致。当前版本使用镜像默认配置。 |
| CPU 绑核、调度优先级及 GPU 队列提权 | **只采样，未打包。** 明日方舟样本中，16 个超过 25 ms 的帧间隔内，主线程累计等 CPU 最多 0.212 ms，图形线程最多 0.312 ms。另一次对齐采样中，52 个长间隔里有 46 个在游戏两次交换缓冲之间就已用去超过 20 ms。设备支持请求 HIGH 不等于提高 SurfaceFlinger 队列优先级有收益。除非新场景显示持续的就绪队列或 GPU 队列积压，否则保留原策略。 |

SurfaceFlinger 按需编译着色器 **已有可安装模块**，不属于上述三项。未验证的是游戏和清空磁盘缓存后的首次启动，尤其是首轮转场的尾帧代价。后台进程保留、冻结和回收则是有意沿用镜像策略，不是遗漏的模块。

## 图形路径与内存

- **端到端零拷贝与共享纹理覆盖面｜结构限制。** 当前 gralloc 分配与宿主 D3D12 资源不能通过同一个 dma-buf 直接互导。已实现的共享纹理路径能省掉整帧 CPU 读回，但物理 CLIENT 目标仍有 GPU 快照拷贝和就绪同步；CPU 消费者或不匹配的格式、尺寸、usage 仍需镜像或回退。不能靠放宽窗口筛选就移除这些步骤。[现有实现与瓶颈](GPU-ACC.md#目前的瓶颈)
- **通知栏展开尾帧｜归因未完成。** broker 优先淘汰已有 CPU 镜像的冷槽，未稳定减少总读回；在导入和回填前优先处理队首 GET，虽缩短分派等待，却没有改善展开尾帧。两份实验补丁均未合入。慢帧与 SystemUI StatusBar 同次提交存在关联，但尚未区分是状态栏出帧晚、等槽，还是多层合成。
- **broker 从 64 槽减为 32 槽｜实测后不采用。** 固定场景中留存内存下降，但通知栏展开 P95 从 15.88 升至 17.30 ms，超过 60 Hz 的单帧预算。当前版本保留 64 槽；评价其他容量策略时要同时看整栈工作集和展开尾帧。
- **扩大 ELF 只读页共享｜未覆盖其他构建。** 指定版本的 AMD 驱动、Gallium、libcrypto 和 libstdc++ 已有共享策略。`libd3d12core.so` 仍未加入：当前样本的可执行段未按页对齐，也没有现有规则要求的 GNU Build ID。不能把观测到的匿名页总量直接算成可节省内存，更不能仅凭库名放宽校验。

## 游戏、转译与宿主工具

- **Limbus Company 普通低帧｜进一步的转译收益未量化。** UnityMain 热点能归属到 libunity 和 libil2cpp；在转译代码里运行不等于在编译，也不能据此算出增加线程的收益。当前版本已采用 [CC1 + 短叶函数](../system-optimize/digitails/README.md)。扩大覆盖之前，应把同类战斗的普通慢帧和长停顿分开，对齐热点与帧间隔，而不是用整场平均 FPS。
- **游戏加载与 ASTC｜暂缓。** GPU 解码和预热试验没有缩短已测 Limbus 关卡的端到端进入时间，未纳入当前版本。冷加载与热重进不是同一负载；继续调查时需区分文件 I/O 等待、Unity 线程工作和纹理上传，不能把离屏上传耗时当作整关收益。
- **ADB 截图的剩余耗时｜未采用额外中转服务。** 当前版本已有无损 PNG 快速路径和 NoDelay 转发。固定 PNG 重放显示传输与命令往返仍有开销，但不能把整段时间算给 Android 编码器，也没有理由仅为实验里的改善新增常驻 Python 代理。非目标格式继续回退平台编码，真实 HDR 屏幕不在现有验证范围。

关闭锁屏后的透明 splash 是[已定位的镜像问题](KNOWN-ISSUE.md)，不是加速层待实现的修复。

---

<a id="english"></a>

# Unfinished work and experiments outside the current version

This index distinguishes open problems, investigations with no installable KernelSU module, and prototypes tested but not adopted. None is a release commitment. Raw measurements and experimental patches are retained locally under `out/`.

## Android system: investigations without a module

| Direction | Status and what would justify revisiting it |
| --- | --- |
| Present the main fullscreen game layer directly | **No general implementation.** All 43 Blue Archive samples with a game SurfaceView also had three side CLIENT layers. A direct path must preserve their order, alpha, color space and synchronization, and recover the full composition on failure. |
| SurfaceFlinger Planner / static layer caching | **Tested offscreen; not packaged.** RGB matched across 30 offscreen comparison frames. The on-device enabled run ended when HWC exited, so it yielded no valid performance comparison. The submitted buffer, reported size and source crop from the failing frame are still missing. Neither Planner itself nor an early Weston release is established as the cause. The image default remains in use. |
| CPU affinity, scheduling priority and GPU queue priority | **Measurement only; not packaged.** In 16 Arknights frame intervals longer than 25 ms, the main and graphics threads spent at most 0.212 and 0.312 ms respectively waiting for CPU while runnable. In a separate aligned trace, 46 of 52 long intervals had already spent over 20 ms between game buffer swaps. Support for HIGH priority does not establish a benefit for SurfaceFlinger. Retain stock scheduling unless another workload shows sustained CPU or GPU queue contention. |

The on-demand SurfaceFlinger shader module **is already packaged**. Its remaining validation gap is first-use latency in games and after clearing the disk shader cache. Background process retention, freezing and reclamation deliberately follow image policy.

## Graphics and memory

- **End-to-end zero-copy and shared-texture coverage — structural limit.** The tested stack cannot import the same allocation into gralloc and host D3D12 via dma-buf. Shared textures already avoid full-frame CPU readback on eligible paths, but the physical CLIENT target still needs a GPU snapshot and completion synchronization. CPU consumers and unsupported layouts still need mirrors or fallback. Relaxing the window filter alone cannot remove these steps. [Current limits](GPU-ACC.md#remaining-bottlenecks)
- **Notification shade tail latency — cause not isolated.** Preferring cold slots with CPU mirrors did not consistently reduce total readback. Serving a queued GET before import or backfill reduced dispatch wait but did not improve opening tail frames. Neither patch was adopted. StatusBar submissions correlate with slow frames; late production, waiting for a slot and multilayer composition have not been separated.
- **32 vs. 64 broker slots — tested and rejected.** Fewer slots retained less memory in the measured workload, but shade-opening P95 rose from 15.88 to 17.30 ms, exceeding the 60 Hz frame budget. The current version keeps 64 slots. Compare both stack working set and tail latency before revisiting capacity.
- **ELF read-only page sharing — not generalized.** Specific AMD driver, Gallium, libcrypto and libstdc++ builds already share eligible pages. The observed `libd3d12core.so` executable segment is not page-aligned and lacks the GNU Build ID required by the current rules. Private anonymous residency is not a savings estimate or a reason to match by filename alone.

## Games, translation and host tools

- **Ordinary low frame rates in Limbus Company — further gains unquantified.** UnityMain hotspots map to libunity and libil2cpp. Running translated code is not the same as compiling it, and these samples do not quantify the benefit of additional threads. The current version already includes [CC1 and short leaf functions](../system-optimize/digitails/README.md#english). To evaluate wider coverage, align hotspots with comparable battle frames and separate ordinary slow frames from long stalls.
- **Game loading and ASTC — deferred.** GPU decoding and warm-up experiments did not shorten end-to-end entry to the measured Limbus stage and were not included in the current version. Cold loading and warm re-entry are different workloads. Separate I/O waits, Unity thread work and texture upload before attributing stage time to the offscreen upload path.
- **Remaining ADB screenshot latency — no extra relay service.** Lossless fast PNG and NoDelay forwarding are already deployed. Replaying identical PNG bytes exposes transfer and command overhead that cannot all be attributed to Android encoding. The relay experiment did not justify a resident Python proxy. Unsupported formats use the platform encoder; a physical HDR display has not been validated.

The transparent splash with the lock screen disabled is a [known image issue](KNOWN-ISSUE.md#english), not a pending acceleration-layer fix.
