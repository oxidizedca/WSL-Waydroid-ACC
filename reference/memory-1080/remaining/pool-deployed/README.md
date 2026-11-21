# CPU 回退暂存槽复用：单变量部署（2026-11-21）

[English](#english)

1920×1080 Waydroid；A 已启用 AMD/Gallium 只读页共享和 ELF 解析视图回收。B 仅修改 [scratch.h](../../../../src/scratch.h) 的暂存槽选择顺序；两组的池容量相同，均未开启 libcrypto/libstdc++ 共享。

## 内存

两版均由部署脚本重启 Weston 与容器。从桌面开始，在不提交查询的情况下，反复启动 Google 搜索、HOME 返回；随后在计算器上反复展开/收起通知栏。每种动作两组，每组预热 2 轮、记录 6 轮。各检查点回到桌面；页级采样与帧计时分开。各检查点映射此 EGL 的进程数 A/B 都是 10、11、12。A、B 初始搜索进程均只有 1 个 8100 KiB 暂存槽。

| 检查点 | 指标 | A (MiB) | B (MiB) |
|---|---|---:|---:|
| 搜索 → HOME 后 | Google 搜索进程的暂存槽 | 63.28（8 槽） | 7.91（1 槽） |
| 通知栏 → HOME 后 | SystemUI 暂存槽 | 47.71 | 22.22 |
| 通知栏 → HOME 后 | 所有已采图形进程的暂存槽 | 134.75 | 45.98 |

统计只含槽所指向缓冲区的完整驻留页，不是 GPU 显存。最后一点直接观测到暂存池少驻留 88.77 MiB，其中 55.37 MiB 来自搜索。整栈工作集最终为 4185.7 / 3631.3 MiB，但初始桌面就相差 412.6 MiB，两次启动的缓存和后台状态不完全相同，不能把总差归因给此改动。所有检查点均无 swap。[逐槽](slots.csv) · [分阶段汇总](memory.csv) · [cgroup 原始采样（字节）](cgroup.csv)

## 显示帧时与显示核对

两组 SurfaceFlinger 显示帧合并后计算 P95，仅计完整落在动作观察窗内的帧，单位 ms，不是点击到应用可用的耗时。

| 动作 | A → B P95 | 超过 16.67 ms 的帧数 A → B |
|---|---:|---:|
| 打开搜索 | 22.59 → 21.66 | 91/450 → 94/459 |
| 返回桌面 | 23.64 → 23.51 | 97/430 → 84/434 |
| 展开通知栏 | 16.75 → 15.94 | 24/464 → 15/455 |
| 收起通知栏 | 14.21 → 13.53 | 2/473 → 0/468 |

单次 A 启动与单次 B 启动，未观察到这四个动作的 P95 恶化；搜索仍有约两成帧超预算，不能据此推断普遍提速。搜索用 am start -n com.google.android.googlequicksearchbox/.SearchActivity 和 KEYCODE_HOME；通知栏用 [shade.py](../../../../validation/shade.py)。[分组统计](timing.csv) · [逐帧数据](frames.csv)

Android 独立进程 D3D12 着色器和全像素检查、历史帧回放与第九个并发槽检查均通过。Windows 窗口截图抽查启动后的桌面、搜索及键盘、通知栏、返回桌面，抽查画面未见黑屏、图标缺失或应用任务栏滞留；不是逐帧或游戏长期验证。结束时 Android crash 缓冲区为空。

---

<a id="english"></a>

# CPU fallback staging-slot reuse: single-variable deployment (2026-11-21)

At 1920×1080, A already shared AMD/Gallium read-only pages and released ELF parsing views. B changed only the slot-selection order in [scratch.h](../../../../src/scratch.h). Both had the same pool capacity and neither shared libcrypto/libstdc++.

## Memory

The same deploy script restarted Weston and Android for each version. From Home, repeatedly open Google Search and return with HOME without submitting a query; then repeatedly open/close the shade over Calculator. Each action had two groups, two warm-ups and six measured cycles per group. Checkpoints were at Home; page sampling was separate from frame timing. A and B each had 10, 11 and 12 EGL processes at successive checkpoints. Search started with one 8100 KiB slot in both.

| Checkpoint | Metric | A (MiB) | B (MiB) |
|---|---|---:|---:|
| After Search → HOME | Google Search staging slots | 63.28 (8 slots) | 7.91 (1 slot) |
| After shade → HOME | SystemUI staging slots | 47.71 | 22.22 |
| After shade → HOME | Staging slots of all sampled graphics processes | 134.75 | 45.98 |

Only complete resident pages of slot-backed buffers are counted, not GPU memory. At the final checkpoint the pool held 88.77 MiB less, 55.37 MiB of it in Search. Whole-stack working sets ended at 4185.7 / 3631.3 MiB, but initial Home already differed by 412.6 MiB. Cache and background states differed between boots; do not attribute that entire difference to the change. No swap at any checkpoint. [Per-slot data](slots.csv) · [Summary by stage](memory.csv) · [Raw cgroup samples (bytes)](cgroup.csv).

## Display-frame times and visual checks

P95 was calculated from both groups' pooled SurfaceFlinger display frames, counting only complete frames inside action windows. Units are ms; this is not tap-to-ready latency.

| Action | A → B P95 | Frames above 16.67 ms, A → B |
|---|---:|---:|
| Open Search | 22.59 → 21.66 | 91/450 → 94/459 |
| Return Home | 23.64 → 23.51 | 97/430 → 84/434 |
| Open shade | 16.75 → 15.94 | 24/464 → 15/455 |
| Close shade | 14.21 → 13.53 | 2/473 → 0/468 |

With one A boot and one B boot, none of the four actions had a worse P95. About one in five Search frames still exceeded budget; this is not evidence of universal speedup. Search used `am start -n com.google.android.googlequicksearchbox/.SearchActivity` and KEYCODE_HOME; the shade used [shade.py](../../../../validation/shade.py). [Group statistics](timing.csv) · [Per-frame data](frames.csv).

An isolated Android process passed D3D12 shader and pixel checks, history replay and ninth-concurrent-slot checks. Windows screenshots showed Home after startup, Search and keyboard, the shade and return to Home without visible black screens, missing icons or a stuck app taskbar. These were spot checks, not per-frame or long-duration game tests. The Android crash buffer was empty afterward.
