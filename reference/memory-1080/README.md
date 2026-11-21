# 1080p 内存：加载器、暂存池与 broker 槽位

[English](#english)

基线包含三组可分别核对的改动：[AMD 驱动只读页共享](loader-system/README.md)、[释放 ELF 解析视图与 fd 并共享 Gallium 只读页](loader-lifetime/deployed/README.md)、[CPU 暂存槽复用与两个依赖库的只读页共享](remaining/README.md)。本页保留最初定位驱动重复驻留的依据，以及 broker 保留 64 槽的原因。单进程 PSS、整栈工作集和目标页去重驻留是不同指标。

## 驱动重复驻留：离屏对照（2026-11-20）
对照组的 [ELF 加载器](../../src/loader_so.c) 把 PT_LOAD 段复制到匿名内存。通过比对活动进程 VMA 长度和代码字节，确认至少 8 个进程有独立的 amdxc64.so 副本，合计 1358.14 MiB Private_Dirty；只计能明确归属的映射，不含可能与邻接映射合并的区间。这是已占用的页，不等于全部能省掉的内存。[逐进程记录](loader-live.csv)

[候选补丁](amd-file-pages-pilot.patch)仅对本机 AMD 驱动试验：整页对齐的只读数据和可执行代码改用文件映射，使内核可按需调页，并在多个进程间共享；可写数据、尾端不完整页和 BSS 仍沿用原处理。核对的驱动 SHA-256 为 `33cae131c20fd1bfb74844988f53e5363b101fb26016c14e03515201d9afad98`，其 392178 项重定位的写入目标均在可写段。原型只按文件名及页布局选择，**不是可以直接对其他驱动部署的通用补丁**。基线仅对通过文件身份与段布局校验的指定构建启用；不匹配时仍按原路径复制。

同一容器内使用独立临时库，按原版、候选、候选、原版顺序，每组启动两个新进程并发绘制。D3D12 后端，64×64 EGL pbuffer，红绿蓝三次清屏，每次核对全部 4096 个像素，两个进程均通过后读取 smaps_rollup。原版四个进程平均 PSS 为 **248.16 MiB**，候选为 **80.37 MiB**，差 **167.79 MiB**。另用顶点/片元着色器绘制三角形并检查全部像素，两版均在容器 root 和 shell UID 下通过。没有验证应用沙箱、冷启动动画、游戏或完整桌面候选版，也没有这项改动的整栈节省值。[进程采样](loader-offscreen.csv) · [清屏与内存探针](loader-memory-probe.c) · [着色器探针](loader-shader-probe.c)

## broker 槽位：内存与帧时的取舍
对照为 64 个活跃槽，实验版只将循环替换的活跃槽限制到 32 个，不改持久化布局。[实验差异](broker32.patch)。A64 / B32 / B32 / A64 四轮，每轮重启 broker，从桌面打开已预热的计算器，展开/收起通知栏 12 次，返回桌面静置约 30 秒。各阶段采 5 点，表为各方案两轮共 10 点均值。未清缓存或杀应用。

| 阶段 | 64 槽工作集 (MiB) | 32 槽工作集 (MiB) |
|---|---:|---:|
| 起始桌面 | 5420.13 | 5410.24 |
| 打开计算器 | 5464.97 | 5447.71 |
| 12 次通知栏开合后 | 5695.44 | 5523.29 |
| 回桌面静置 | 5727.74 | 5538.51 |

工作集取 Waydroid payload/monitor、容器管理服务、Weston、输入/消费服务各自不重叠的 cgroup memory.current 之和，加组外 Waydroid session 与 dnsmasq 的 PSS，减各组 inactive_file。不计 Windows、外层 WSLg、独立显存和无关 WSL 服务；swap 单列。broker 持有的 dma-buf 按设备和 inode 去重，其标称大小是解释性指标，**不再累加**。回桌面后它从 332.81 降到 167.81 MiB；扣除各自起始桌面后，整栈工作集增长量少 179.34 MiB。这是固定场景下的留存，不是泄漏证明。

单独复跑通知栏帧时：展开 P95 为 15.88 → 17.30 ms，超 16.67 ms 的帧为 19/463 → 26/454；收起 P95 为 13.96 → 14.18 ms。基线因此保留 64 槽。[采样工具](../../validation/memory.py) · [原始内存数据（字节）](samples.csv) · [进程排名](process-top.csv) · [逐帧时长（纳秒）](frames.csv)

单进程生命周期与整栈工作集的区别见[加载器离屏测量](loader-lifetime/README.md)。

---

<a id="english"></a>

# 1080p memory: loader, staging pool and broker slots

Baseline changes have separate measurements: [AMD driver read-only sharing](loader-system/README.md#english), [Release ELF parser views and fds; share Gallium read-only pages](loader-lifetime/deployed/README.md#english), and [Reuse CPU slots and share read-only pages of two dependencies](remaining/README.md#english). This page records how duplicate residency was identified and why broker capacity remains 64. Single-process PSS, whole-stack working set and deduplicated resident target pages are distinct metrics.

## Duplicate driver pages: offscreen comparison (2026-11-20)
The control [ELF loader](../../src/loader_so.c) copied PT_LOAD segments into anonymous memory. VMA lengths and code bytes identified separate amdxc64.so copies in at least 8 active processes, totaling 1358.14 MiB Private_Dirty. Only unambiguously identified mappings were counted, not possible merged neighbors. This is occupied memory, not guaranteed savings. [Per-process records](loader-live.csv).

The [Experimental patch](amd-file-pages-pilot.patch) tested only this AMD driver: complete aligned read-only and executable pages were file-mapped for demand paging and cross-process sharing. Writable data, partial tails and BSS kept the old handling. Driver SHA-256 `33cae131c20fd1bfb74844988f53e5363b101fb26016c14e03515201d9afad98`; all 392178 relocation writes targeted writable segments. The prototype matched only filename and page layout and is **not a general patch for other drivers**. The baseline also verifies file identity and segment layout before enabling sharing; mismatches are copied.

Isolated libraries in the same container, order original/candidate/candidate/original; each group launched two concurrent drawing processes. Each used a 64×64 D3D12 EGL pbuffer, cleared red/green/blue and checked all 4096 pixels each time. smaps_rollup was read after both passed. Mean PSS over four processes per version: **248.16 → 80.37 MiB**, a **167.79 MiB** difference. Vertex/fragment shaders, a triangle and full pixel checks also passed under root and shell UID on both versions. This experiment did not test app sandboxes, cold-start animations, games, the full candidate desktop or whole-stack savings. [Process samples](loader-offscreen.csv) · [Clear and memory probe](loader-memory-probe.c) · [Shader probe](loader-shader-probe.c).

## Broker slots: memory versus frame time
A used 64 active slots; B limited rotation to 32 without changing the persisted layout. [Experimental diff](broker32.patch). Order A64/B32/B32/A64, restarting only the broker per run. From Home, open prewarmed Calculator, open/close the shade 12 times, then idle at Home about 30 s. Five readings per stage per run; table means use 10 points across two runs per version. No cache clearing or app killing.

| Stage | 64-slot working set (MiB) | 32-slot working set (MiB) |
|---|---:|---:|
| Initial Home screen | 5420.13 | 5410.24 |
| Open Calculator | 5464.97 | 5447.71 |
| After 12 shade cycles | 5695.44 | 5523.29 |
| Idle after returning Home | 5727.74 | 5538.51 |

Working set sums non-overlapping cgroup memory.current for Waydroid payload/monitor, container manager, Weston and input/consumer services, plus PSS of the out-of-group session and dnsmasq, minus each group's inactive_file. It excludes Windows, outer WSLg, dedicated VRAM and unrelated WSL services; swap is separate. Broker dma-bufs are deduplicated by device/inode. Their nominal size explains retention and is **not added again**. At Home after the test it fell from 332.81 to 167.81 MiB; relative to each run's initial Home checkpoint, working-set growth was 179.34 MiB lower. This is retention in a fixed scenario, not proof of a leak.

In separate shade timing, open P95 was 15.88 → 17.30 ms, frames above 16.67 ms were 19/463 → 26/454, and close P95 was 13.96 → 14.18 ms. The baseline therefore keeps 64 slots. [Sampling tool](../../validation/memory.py) · [Raw memory data (bytes)](samples.csv) · [Process rankings](process-top.csv) · [Per-frame durations (ns)](frames.csv).

See [Offscreen loader measurements](loader-lifetime/README.md#english) for the distinction between single-process lifetime and whole-stack working set.
