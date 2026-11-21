# CPU 暂存槽与依赖库共享页：内存归因

[English](#english)

在 1920×1080 桌面、后台保留设置和搜索的条件下，分别定位暂存槽留存与 ELF 只读页重复驻留。两项优化均已合入。实机对照分别见[暂存槽](pool-deployed/README.md)、[两库共享](shared-deployed/README.md)及[单库收益拆分](shared-split/README.md)；下文保留选槽和页级归因的依据。

## 1. CPU 回退暂存池：无历史槽被不同窗口逐个占满

在 10 个加载本版 EGL 的 Android 进程中读取项目自己的表头和页驻留位，不读帧像素。[逐进程](cache.csv) · [逐槽](slots.csv)。Google 搜索进程的八个槽各有 8100 KiB（1920×1080 RGBA 的大小）的驻留写缓冲，全都空闲且两侧历史标志都为 0；仅这个进程就留了 63.28 MiB。SystemUI 的两个 8100 KiB 槽也是无历史的，但还有一个有效的 14400 KiB 历史，不能整个清池。这里计数的是完整驻留页，不是将 malloc 容量当成 PSS；这是一个时点，不是持续泄漏的证据。

[源码](../../../src/scratch.h)原先按“同一窗口 → 可回放历史 → 最久未使用”选槽。当同尺寸空窗口不断换身份时，未用槽 tick=0，优先于已分配但无历史的空闲槽，最终把高水位撑到八份。[选槽差异](pool.patch)只在找不到原窗口及可回放历史时，优先借用同尺寸、空闲且两侧均无历史的写槽；不缩并发上限，不主动清历史，不逐帧 free。不会立即回收旧进程之前积下的存量。

[WSL 原生小程序](pool.c)直接包含两个版本的头文件，用 12 个不同窗口身份依次写满同尺寸帧：A 留存 64800 KiB、B 留存 8100 KiB（差 55.37 MiB）；两版均通过历史帧标记和九个并发持有槽互不重叠的检查。[A](pool-A.log) · [B](pool-B.log)。此差值只代表选槽探针；真实搜索进程的驻留量与四个动作的帧时见[实机对照](pool-deployed/README.md)。

## 2. ELF 加载器：两份额外依赖的已验证只读页

[页级归因](loader.csv)按已装 EGL 的库表、文件的 PT_LOAD 及 pagemap 统计，限定非可写段中完整、驻留、匿名且独占的页。此现场 10 个进程的 libcrypto.so.3 合计 57120 KiB，libstdc++.so.6 合计 25880 KiB，共 81.05 MiB；实际系统收益须扣除共享页本身，不能把这个数当作节省量。libd3d12core.so 另有 63800 KiB，但其 RX 段起点不对齐且没有现行规则所需的 GNU Build ID，这次没有把它加入允许列表。

[共享页差异](shared.patch)仍按文件名、大小、GNU Build ID、程序头数量和 LOAD 布局精确匹配；只让完整非可写页来自文件映射，其余保持原有拷贝。核对两文件的 25147 / 6294 个重定位，涉及的 8 字节写入全部在可写段内。libcrypto 的 SHA-256 为 5385f0436ac2e284ba8c5fb1488874057c29c04be72dea32add832694f381e28，libstdc++ 的为 5bb0d21308f123b6ad46c6f35b42cedfcb8d6d439a53aa3dae04d880aaffdde3。只改变了实验加载器，不修改这两个库。

| Android 独立离屏进程（每版四次均值） | A 已安装版本 | B 额外共享两库 |
|---|---:|---:|
| PSS | 66.61 MiB | 62.00 MiB |
| 匿名 PSS | 59.24 MiB | 51.13 MiB |

用项目 NDK 重新编出的 A 与已安装版本 SHA-256 一致；顺序 ABBABAAB，每次用新进程。八次均通过 D3D12 离屏清屏、编译链接着色器和 64×64 全像素校验，并逐次确认两份新增库只在 B 出现文件后备的可执行映射。[原始数据](ab.csv) · [像素探针](../loader-shader-probe.c)。此表是当时的离屏结果，不与暂存槽收益相加；后续实机 A/B 单列在[部署记录](shared-deployed/README.md)，游戏不在本轮范围内。

复查现场可在 WSL 仓库根目录以 root 运行 `python3 reference/memory-1080/remaining/scan.py <已有的空输出目录>`。[只读采样脚本](scan.py)限定采样时的 EGL 的 SHA-256、架构和数据结构布局，输出不含像素或地址。

---

<a id="english"></a>

# Memory attribution: CPU staging slots and shared library pages

On a 1920×1080 desktop with Settings and Search retained in the background, staging retention and duplicate ELF read-only pages were investigated separately. Both optimizations are in the baseline. See [Staging slots](pool-deployed/README.md#english), [Sharing both libraries](shared-deployed/README.md#english) and [Per-library savings](shared-split/README.md#english) for device comparisons. This page records how the slots and pages were identified.

## 1. CPU fallback staging: changing windows fill history-free slots

In 10 Android processes loading this EGL, only project table headers and page residency bits were read, not pixels. [Per-process data](cache.csv) · [Per-slot data](slots.csv). Google Search had eight idle slots, each with an 8100 KiB resident write buffer (1920×1080 RGBA) and both history flags clear: 63.28 MiB in that process alone. SystemUI had two history-free slots of the same size but also valid 14400 KiB history; clearing the whole pool would be wrong. These are complete resident pages, not malloc capacity counted as PSS. A snapshot does not establish a persistent leak.

The [Source](../../../src/scratch.h) originally preferred the same window, then replayable history, then the least recently used slot. As same-size windows changed identity, unused slots at tick=0 outranked allocated idle slots without history, raising the high-water mark to eight buffers. The [Slot-selection diff](pool.patch) prefers an idle same-size write slot with neither history flag only after the first two checks miss. Capacity is unchanged, history is not purged and there is no per-frame free. It does not immediately reclaim allocations retained by old processes.

A [Native WSL probe](pool.c) included both versions of the header and sequentially filled same-size frames for 12 distinct window identities. A retained 64800 KiB and B 8100 KiB (55.37 MiB difference). Both preserved history markers and kept all nine simultaneously held slots disjoint. [A](pool-A.log) · [B](pool-B.log). These are probe figures; see the [On-device comparison](pool-deployed/README.md#english) for real Search residency and frame times for four actions.

## 2. ELF loader: validated read-only pages in two more dependencies

The [Page-level attribution](loader.csv) combined the installed EGL module table, PT_LOAD data and pagemap, counting only complete, resident, exclusively owned anonymous pages of non-writable segments. Across 10 processes libcrypto.so.3 totaled 57120 KiB, libstdc++.so.6 25880 KiB, or 81.05 MiB together. Shared pages themselves still cost memory: this is not a savings figure. libd3d12core.so had another 63800 KiB but its RX segment start was unaligned and it lacked the GNU Build ID required by the rule; it was not allowlisted.

The [Shared-page diff](shared.patch) matches exact filename, size, GNU Build ID, program-header count and LOAD layout. Only whole non-writable pages become file-backed; other bytes retain the copy path. Across 25147 / 6294 relocations respectively, all 8-byte writes were in writable segments. SHA-256: libcrypto 5385f0436ac2e284ba8c5fb1488874057c29c04be72dea32add832694f381e28; libstdc++ 5bb0d21308f123b6ad46c6f35b42cedfcb8d6d439a53aa3dae04d880aaffdde3. Only the experimental loader changed.

| Independent offscreen Android process (four-run mean per version) | A: installed control | B: also share both libraries |
|---|---:|---:|
| PSS | 66.61 MiB | 62.00 MiB |
| Anonymous PSS | 59.24 MiB | 51.13 MiB |

Rebuilding A with the project NDK matched the installed hash. Order ABBABAAB, new process each time. All eight passed D3D12 clear, shader compilation/linking and all 64×64 pixels; executable mappings for the two libraries were file-backed only in B. [Raw data](ab.csv) · [Pixel probe](../loader-shader-probe.c). Do not add these offscreen findings to staging-slot savings. The [Deployment results](shared-deployed/README.md#english) measures device-level A/B separately; this test did not cover games.

From the WSL repository root, as root run `python3 reference/memory-1080/remaining/scan.py EXISTING_EMPTY_OUTPUT_DIR`. The [Read-only sampler](scan.py) requires the tested EGL hash, architecture and table layout. No pixels or addresses are emitted.
