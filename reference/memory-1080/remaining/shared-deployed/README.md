# 两份额外依赖的只读文件映射：Waydroid 实机验证

[English](#english)

2026-11-21，1920×1080、滑动锁屏保持开启，冷启动后正常上滑解锁。A 为已部署的 CPU 暂存槽复用版；B 仅更新 Android EGL 的[自定义 ELF 加载器](../../../../src/loader_so.c)。GL 驱动、broker、桌面、SystemUI、两份被加载的依赖库和八件运行件中的另七件均未改变。未启动游戏。

## 到底省了哪些页

每轮在桌面取样，再连续打开设置、Jelly、计算器。第一对 A/B 在相同阶段读取加载器模块表和 Linux pagemap，只统计 libcrypto.so.3 和 libstdc++.so.6 的**非可写 PT_LOAD 中可映射的完整 4 KiB 页**。对所有入样图形进程的驻留页按物理页号去重；输出只保留计数，不输出地址、页号或内存内容。

| 第一对冷启动检查点 | 同版 EGL 进程数 A / B | A 映射的独立驻留页 (MiB) | B (MiB) | 少映射 (MiB) |
|---|---:|---:|---:|---:|
| 解锁后桌面 | 10 / 10 | 81.05 | 3.51 | 77.55 |
| 设置 → Jelly → 计算器 | 12 / 12 | 97.27 | 3.51 | 93.76 |

A 的目标页均是独占匿名页；B 在上述范围内不再有独占匿名页。多应用场景逐进程核对，12/12 均有两份库各自的文件后备 RX 映射。B 的 3.51 MiB 是当时已建立映射并驻留的不同页，不是库文件大小，也不含纯页缓存或 GPU 显存。变化来自两个方面：多个进程能用同一物理页，未访问的文件页也不必像旧路径那样预先 memcpy。不能把表内差值直接当成整栈工作集降幅。

[逐进程页计数](pages.csv) · [分库去重结果](unique.csv) · [单次 A/B 页数与 cgroup 汇总](summary.csv)。独立复查：在 WSL 仓库根以 root 运行 python3 reference/memory-1080/remaining/shared-deployed/pages.py out/libEGL_angle.so /var/tmp/acc-pages-new，输出目录须不存在，要求已装 EGL 与参数文件逐字节匹配、符号表可用；仅支持本次核对的两种构建。

## 兼容性与边界

- 只放行已核对的确切构建：文件名、大小、GNU Build ID、程序头数量和四个 LOAD 布局均匹配才尝试共享。可写部分和不足一页的尾部保留原实现；不修改 libcrypto 或 libstdc++ 本身，也不把 libd3d12core 加进允许列表。
- 对实际文件运行本版匹配函数，原始样本被接受；在测试副本上分别改变 Build ID 字节或任一 LOAD 的内存大小，都被拒绝。[负例结果](identity.txt)。这是匹配函数检查，不是替换系统库。
- 从当前源码重编对照 EGL，结果与部署前安装文件逐字节一致。候选在 Android 独立进程通过 D3D12 清屏、着色器编译链接及 64×64 全像素检查。A SHA-256 为 52da392d9b6bed1c28a1ce9cd4fe2426adbd6f61e007dbe90181b734e7c679ba，B 为 2e43e41280440386044eb59618069f4b614d4499c9f57eaa91c8d9f20f603b2c。

## 整栈与尾帧

使用[整栈采样](../../../../validation/loader_memory.py)的 --ui-only 场景，每个检查点 5 个采样，工作集含容器和专属 WSL 服务、扣除 inactive_file，不含宿主显存。每版每次冷启动后再执行两组[通知栏测试](../../../../validation/shade.py)，每组预热两轮、记录六轮。帧时是 SurfaceFlinger 显示帧时，不是点击响应时间；汇总 P95 从逐帧样本重新计算，不是各组 P95 的平均。未清系统页缓存。

每版各重启两次，顺序 A、B、A、B。锁屏四次均开启，走同一个非凭据上滑解锁流程，不改系统设置。第一对额外采集上述去重页数，第二对仅采集整栈和逐进程内存，以复核第一对出现的通知栏尾帧差异。除每轮预热外，未专门冷却或固定 GPU/CPU 频率。

[四次启动的阶段汇总](all-boots.csv) · [工作集原始采样](samples.csv) · [图形进程 PSS](processes.csv) · [帧时分组统计](timing.csv) · [逐帧原始数据](frames.csv)

| 两次启动均值 | A | B | A − B |
|---|---:|---:|---:|
| 桌面整栈工作集 | 2738.0 MiB | 2678.6 MiB | 59.4 MiB |
| 多应用整栈工作集 | 3265.8 MiB | 3184.4 MiB | 81.4 MiB |

两对同向，八个阶段检查点的 swap 均为 0。容器匿名内存均值桌面 1862.3 → 1798.4 MiB，多应用 2138.9 → 2052.6 MiB。整栈工作集包含其它进程与内核波动；直接归因请看上面的页级数据。

| 动作 | A / B P50 (ms) | A / B P95 (ms) | A / B 超过 60 Hz 单帧预算 |
|---|---:|---:|---:|
| 展开通知栏 | 8.84 / 8.92 | 15.67 / 17.54 | 30/934 / 54/880 |
| 收起通知栏 | 8.78 / 8.88 | 14.38 / 14.54 | 1/945 / 2/895 |

展开 P95 在两对中均变慢（15.80 → 17.66、15.42 → 17.17 ms），合并后超过 16.67 ms 的帧从 3.2% 升到 6.1%。不能认定“省内存且不影响帧时”，也不能仅据结果认定原因是缺页。四组合拆分的结果见下，不能将这两对样本解释为基线的稳定退化。

Windows 侧抽查了解锁后的桌面、计算器、设置及返回桌面，静态画面未见异常；没有逐帧复核 splash。

后续已完成[四组合单库拆分及缺页对齐](../shared-split/README.md)：多应用中 crypto 和 stdc++ 单开分别少驻留 65.05 和 28.46 MiB。拆分测试未稳定复现本页记录的双开慢帧，不宜由上轮两组数据直接指定某个库为主因。

---

<a id="english"></a>

# Two additional file-backed libraries: Waydroid device results

2026-11-21, 1920×1080. Swipe lock stayed enabled; both boots used a normal swipe to unlock. A had CPU staging reuse; B changed only Android EGL's [Custom ELF loader](../../../../src/loader_so.c). GL driver, broker, launcher, SystemUI, both dependency files and the other seven of the eight runtime components were unchanged. No game was launched.

## Which pages were saved?

Each run sampled Home, then launched Settings, Jelly and Calculator. At matched checkpoints in the first A/B pair, loader tables and Linux pagemap identified **complete mappable 4 KiB pages in non-writable PT_LOAD segments** of libcrypto.so.3 and libstdc++.so.6. Resident pages were deduplicated by physical page number across all sampled graphics processes. Only counts were recorded, not addresses, page numbers or contents.

| First cold-boot pair: checkpoint | Processes with the tested EGL, A / B | Distinct resident pages mapped by A (MiB) | B (MiB) | Reduction (MiB) |
|---|---:|---:|---:|---:|
| Home after unlock | 10 / 10 | 81.05 | 3.51 | 77.55 |
| Settings → Jelly → Calculator | 12 / 12 | 97.27 | 3.51 | 93.76 |

All target pages in A were exclusive anonymous pages; B had none in this range. In the multi-app scene, 12/12 processes had file-backed RX mappings for both libraries. B's 3.51 MiB counts distinct pages actually mapped and resident then, not file sizes, cache-only pages or GPU memory. Processes can share a physical page, and untouched file pages need not be copied eagerly. The difference is not itself a whole-stack working-set reduction.

[Per-process page counts](pages.csv) · [Deduplicated pages by library](unique.csv) · [A/B page counts and cgroups](summary.csv). To reproduce, as root in the WSL repository run `python3 reference/memory-1080/remaining/shared-deployed/pages.py out/libEGL_angle.so /var/tmp/acc-pages-new`. The output directory must not exist; installed EGL must match the file argument byte for byte and have a symbol table. Only the two checked builds are supported.

## Compatibility and scope

- The name, size, GNU Build ID, program-header count and all four LOAD layouts must match. Writable data and partial tails stay on the existing path. Neither dependency is modified, and libd3d12core is not allowlisted.
- The matcher accepted the original files. Changing a Build ID byte or any LOAD memory size in a copy was rejected. [Negative controls](identity.txt). This exercised the matching function; it did not replace system libraries.
- Control EGL rebuilt byte-for-byte identical to the predeployment file. The candidate passed D3D12 clear, shader compile/link and full 64×64 pixel checks in an isolated Android process. A SHA-256: 52da392d9b6bed1c28a1ce9cd4fe2426adbd6f61e007dbe90181b734e7c679ba; B: 2e43e41280440386044eb59618069f4b614d4499c9f57eaa91c8d9f20f603b2c.

## Whole-stack memory and frame-time tails

The [Whole-stack sampler](../../../../validation/loader_memory.py) used --ui-only, 5 samples per checkpoint, container and dedicated WSL services minus inactive_file, excluding host VRAM. Each boot had two groups of the [Shade test](../../../../validation/shade.py), two warm-ups and six measured cycles per group. Timings are SurfaceFlinger display frames, not tap latency. P95 is recomputed from frames, not averaged across groups. System page cache was not cleared.

Two boots per version, A/B/A/B. All four used the same swipe lock, without changing settings. The first pair additionally collected deduplicated page counts; the second took only whole-stack and per-process memory to recheck the shade tail difference. No forced cooldown or fixed GPU/CPU clocks beyond the warm-up.

[Stage summary across four boots](all-boots.csv) · [Raw working-set samples](samples.csv) · [Graphics-process PSS](processes.csv) · [Frame-time groups](timing.csv) · [Raw frames](frames.csv)

| Mean of two boots | A | B | A − B |
|---|---:|---:|---:|
| Home: whole-stack working set | 2738.0 MiB | 2678.6 MiB | 59.4 MiB |
| Multi-app: whole-stack working set | 3265.8 MiB | 3184.4 MiB | 81.4 MiB |

Both pairs moved in the same direction; all eight checkpoints had zero swap. Mean container anonymous memory: Home 1862.3 → 1798.4 MiB, multi-app 2138.9 → 2052.6 MiB. The whole-stack figure includes unrelated process/kernel variation. Use the page-level data for direct attribution.

| Action | A / B P50 (ms) | A / B P95 (ms) | Frames over the 60 Hz budget, A / B |
|---|---:|---:|---:|
| Open shade | 8.84 / 8.92 | 15.67 / 17.54 | 30/934 / 54/880 |
| Close shade | 8.78 / 8.88 | 14.38 / 14.54 | 1/945 / 2/895 |

Opening P95 worsened in both pairs (15.80 → 17.66 and 15.42 → 17.17 ms); pooled frames over 16.67 ms rose from 3.2% to 6.1%. This does not prove unchanged frame times or prove page faults caused the difference. The four-way comparison below explains why two pairs do not establish a stable baseline regression.

Windows spot checks of Home after unlock, Calculator, Settings and return to Home showed no static anomalies; splash was not inspected frame by frame.

In the [Four-way comparison and page-fault timing](../shared-split/README.md#english), crypto and stdc++ alone saved 65.05 and 28.46 MiB respectively in the multi-app case. The combined tail regression did not reproduce consistently. The two pairs above cannot identify one library as its primary cause.
