# libcrypto / libstdc++ 只读页拆分测试（2026-11-21）

[English](#english)

在 1920×1080、60 Hz 的同一台 Waydroid 上测四种 EGL：N 两项都不开；C 只共享 libcrypto.so.3；S 只共享 libstdc++.so.6；X 同时共享。它们都保留 AMD/Gallium 的既有共享页和已部署的 CPU 暂存槽优化。“都不开”只指本轮这两份库，不是禁用全部共享页。未改这两份库、桌面、SystemUI 或渲染服务。

## 如何对照

- N、X 分别使用与[上轮](../shared-deployed/README.md)相同的两份实际二进制；C、S 从同一份加载器源码在临时目录移除另一项白名单条目，复用同一批 EGL 对象，与正式安装和仓库 out 隔离构建。[二进制 SHA-256](variants.sha256) · [仅 crypto 的源码差异](crypto-only.patch) · [仅 stdc++ 的源码差异](stdcpp-only.patch)。两种单库版均先经过 Android 独立进程的 D3D12 着色器、清屏和整面像素检查。
- 启动顺序为 X、C、N、S，再反序 S、N、C、X；中间相邻的两个 S 也完整重启 Weston 和容器。每种组合两次冷启动，用对称顺序减弱单向时间漂移。滑动锁屏始终打开，正常上滑解锁，不更改安全设置，不清缓存。
- 回桌面等待 18 秒；每个检查点按约 0.65 秒间隔采 5 个工作集样本。依次启动设置、Jelly、计算器，每次等 3 秒后测多应用场景。按照[上轮页级方法](../shared-deployed/pages.py)在每个实测 EGL 的动态模块表取目标完整只读页，全部入样进程按物理页号去重，仅保存计数，不保存物理地址、用户数据。核对每个进程各库的独占匿名页是否与对应开关一致。该上轮脚本固定了旧哈希；复测单库版本须明确核对并认可新构建的哈希和表布局。
- 计算器上每次冷启动运行两组[通知栏脚本](../../../../validation/shade.py)，每组预热 2 轮再测 6 轮展开及收起。只计动作窗口内完整的 SurfaceFlinger 显示帧。主结果的 P95 从原始帧合并后计算，不是四个组 P95 的平均。每个版本只有两个独立的启动样本，不能把几百帧当作几百次独立冷启动。Windows 侧桌面和计算器截图在帧计时结束后采集。

## 实测结果

目标库驻留量按相同图形进程中的物理页去重，桌面 10 个进程，多应用 12 个进程。下表节省量以 N 为对照，是两库映射的净差，不是 Windows 或整栈内存。帧时单位为毫秒，预算为 1000/60 ms；尾部指所有展开显示帧的 P95，不特指动画最后一张。

| 配置 | 桌面节省 (MiB) | 多应用节省 (MiB) | 展开 P95（两次分别） | 展开合并 P95 | 超预算帧 | 收起合并 P95 |
|---|---:|---:|---:|---:|---:|---:|
| 都不开 | 0.00 | 0.00 | 16.16 / 16.77 | 16.50 | 43/918 (4.68%) | 14.62 |
| 仅 libcrypto | 54.01 | 65.05 | 15.78 / 18.91 | 17.28 | 51/914 (5.58%) | 14.83 |
| 仅 libstdc++ | 23.53 | 28.46 | 15.73 / 16.36 | 16.26 | 40/924 (4.33%) | 14.77 |
| 两项都开 | 77.46 | 93.55 | 16.71 / 15.84 | 16.06 | 43/924 (4.65%) | 14.82 |

[每次启动的内存](memory.csv) · [逐库去重页](physical.csv) · [逐进程计数](per-process.csv) · [整栈工作集原始样本](cgroups.csv) · [帧时汇总](timing.csv) · [逐组帧时](groups.csv) · [逐帧数据](frames.csv) · [四组差值](contrasts.csv)

### 能归因于哪一项

- **内存：**多应用下，仅 crypto 省 65.05 MiB；在 stdc++ 已开启时再加 crypto，仍省 65.09 MiB。仅 stdc++ 省 28.46 MiB，在 crypto 已开启时再加它，仍省 28.49 MiB。两项相加与实测双开的差距不足 0.04 MiB；约七成收益来自 crypto。本轮双开多应用目标映射为 97.14 → 3.59 MiB，与上轮 97.27 → 3.51 MiB 接近。
- **整栈不能照此拆账：**例如同为 N，多应用工作集两次为 3355.93、3936.18 MiB；后者容器的 file 减去整栈 inactive_file 的差额也明显更大，且出现少量 swap。这些都是检查点观测值，不能据此证明慢帧来自换页或把几百 MiB 的波动算进库收益。
- **展开尾部没有稳定的单库主因。**S 对 N 的两轮 P95 差都约为 −0.4 ms；C 对 N 分别为 −0.37、+2.14 ms；在 S 上增加 C 又分别为 +0.97、−0.52 ms。C 第二轮值得留意，但双开合并 P95 并未复现上轮的退化，不能判定关闭 C 有益。收起 P95 四组只差 0.21 ms。
- **缺轮敏感性：**N 第二次首次执行在第二组一个收起窗口没有完整显示帧，校验中止；该次未完整的样本单独列出，N 另取一次完整冷启动。主表只取完整启动，没有悄悄删除慢帧：[首次未完成采集到的帧](incomplete-N.csv)另存。[敏感性](baseline-sensitivity.csv)：若用这批已观测展开帧替代重测，N 的合并 P95 为 18.45 ms；若纳入三次 N 的全部已观测展开帧，则为 17.71 ms。两者均高于 C 的 17.28 ms；不能稳定地将上轮慢帧归因于 libcrypto。
- **额外检查热态缺页：**在双开配置的一次独立启动中，单独用私有 tracefs 实例按目标库的设备号和 inode 同时采 mm_filemap_fault 与 mm_filemap_map_pages，不混入主表。只采前一个事件无法通过热映射阳性对照，故不用那次结果。双事件检查中，新进程首次读取每库分别命中一次 map_pages；整段通知栏测试两库均为零事件，且 trace 无覆盖。228 个展开帧中有 12 个超过 60 Hz 预算，均无这两份文件的上述事件。这不支持“热态慢帧是反复对这两库按需映射导致”的解释，不排除首触、匿名缺页、调度或 GPU 等因素，也不等于磁盘读数。[事件计数](filemap-events.csv)

**基线选择：两项共享均开启。** 各配置在 Windows 侧采集的桌面、计算器静态截图未见显示异常。保留双开的依据是可复现的目标页节省，而不是通知栏帧时改善。

---

<a id="english"></a>

# Separating libcrypto and libstdc++ read-only sharing (2026-11-21)

Four EGL configurations on the same Waydroid at 1920×1080/60 Hz: N neither, C libcrypto.so.3 only, S libstdc++.so.6 only, X both. AMD/Gallium sharing and CPU staging reuse remained enabled throughout. “Neither” refers only to these two libraries. No dependency files, launcher, SystemUI or rendering service were modified.

## Comparison design

- N and X used the same binaries as the [Two-library comparison](../shared-deployed/README.md#english). C and S removed the other allowlist entry in isolated builds using the same loader source and EGL objects, separate from the installation and repository out. [Binary SHA-256](variants.sha256) · [Crypto-only diff](crypto-only.patch) · [stdc++-only diff](stdcpp-only.patch). Both single-library builds passed D3D12 shader, clear and full-surface pixel checks in isolated Android processes.
- Boot order X/C/N/S then S/N/C/X, including a full Weston/container restart between adjacent S runs. Two cold boots per configuration, in symmetric order to reduce one-way time drift. Swipe lock remained enabled; no security setting or cache clearing.
- Wait 18 s at Home; collect 5 working-set samples about 0.65 s apart at each checkpoint. Launch Settings, Jelly, Calculator with 3 s waits, then measure multi-app. The [Page-level method](../shared-deployed/pages.py) locates complete read-only pages in each EGL's dynamic module table and deduplicates by physical page across processes. Only counts, not addresses or user data, were saved. Each library's per-process exclusive anonymous pages were checked against its switch. The earlier script pins old hashes: verify the new builds' hashes and layouts before testing a single-library variant.
- Over Calculator, each boot had two groups of the [Shade script](../../../../validation/shade.py), two warm-ups then six open/close cycles per group. Only complete SurfaceFlinger display frames inside action windows count. Pooled P95 uses raw frames, not the mean of four group P95s. Hundreds of frames are still only two independent boots per build. Windows screenshots of Home and Calculator were taken after timing.

## Results

Resident target pages were physically deduplicated across the same 10 graphics processes at Home and 12 in the multi-app case. Savings are net differences for both libraries against N, not whole-stack or Windows memory. Times are ms, budget 1000/60 ms. “Tail” means P95 over **all** opening frames, not the final animation frame.

| Configuration | Home savings (MiB) | Multi-app savings (MiB) | Open P95 (each boot) | Pooled open P95 | Over-budget frames | Pooled close P95 |
|---|---:|---:|---:|---:|---:|---:|
| Neither | 0.00 | 0.00 | 16.16 / 16.77 | 16.50 | 43/918 (4.68%) | 14.62 |
| libcrypto only | 54.01 | 65.05 | 15.78 / 18.91 | 17.28 | 51/914 (5.58%) | 14.83 |
| libstdc++ only | 23.53 | 28.46 | 15.73 / 16.36 | 16.26 | 40/924 (4.33%) | 14.77 |
| Both | 77.46 | 93.55 | 16.71 / 15.84 | 16.06 | 43/924 (4.65%) | 14.82 |

[Memory by boot](memory.csv) · [Deduplicated pages by library](physical.csv) · [Per-process counts](per-process.csv) · [Raw whole-stack working set](cgroups.csv) · [Frame-time summary](timing.csv) · [Frame time by group](groups.csv) · [Per-frame data](frames.csv) · [Four-way contrasts](contrasts.csv)

### What can be attributed to each library?

- **Memory:** in the multi-app scenario crypto alone saved 65.05 MiB; adding it on top of stdc++ saved 65.09 MiB. stdc++ alone saved 28.46 MiB, or 28.49 MiB on top of crypto. The sum was within 0.04 MiB of measured combined savings; about 70% came from crypto. Both enabled: 97.14 → 3.59 MiB for the target mappings, close to the previous 97.27 → 3.51 MiB.
- **Do not split whole-stack memory this way.** Even in N, the multi-app working set measured 3355.93 and 3936.18 MiB across boots. In the latter, container file minus whole-stack inactive_file was substantially larger and some swap was used. These checkpoints neither prove paging caused slow frames nor attribute hundreds of MiB of variability to the libraries.
- **Neither library consistently explains the opening tail.** S vs N changed P95 about −0.4 ms in both comparisons; C vs N changed it −0.37 and +2.14 ms. Adding C to S changed it +0.97 and −0.52 ms. C's second run merits attention, but pooled X did not reproduce the earlier regression. Disabling C is not justified. Pooled closing P95 differed by only 0.21 ms across configurations.
- **Incomplete-run sensitivity:** the initial attempt at N's second boot had a closing window with no complete display frame in its second group; validation stopped and a new complete cold boot was measured. The main table uses only complete boots. The [Frames from incomplete sample](incomplete-N.csv) are preserved, not silently removed. In the [Sensitivity analysis](baseline-sensitivity.csv), substituting the available opening frames for the retest gives pooled N P95 18.45 ms; including all observed opening frames from all three N boots gives 17.71 ms. Both exceed C's 17.28 ms, so the earlier regression cannot reliably be assigned to libcrypto.
- **Warm faults checked separately:** in an independent X boot, a private tracefs instance filtered mm_filemap_fault and mm_filemap_map_pages by device and inode for both libraries. Fault alone failed a positive hot-mapping control, so that run was discarded. With both events, the first read of each library by a new process hit map_pages once; the entire shade test recorded zero events for either library with no trace overwrite. Of 228 opening frames, 12 exceeded the 60 Hz budget and none had either target-file event. This argues against repeated demand mapping of these libraries as the explanation for warm slow frames. It does not exclude first touch, anonymous faults, scheduling or GPU effects, and the events are not disk-read counts. [Event counts](filemap-events.csv).

**Baseline: enable both.** Windows screenshots of Home and Calculator showed no static display issues for any configuration. Both are kept for reproducible page savings, not an improvement in shade frame time.
