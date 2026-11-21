# 已部署的 AMD 驱动共享页：系统内存对照

[English](#english)

2026-11-20，RX 7900 XTX，Android 物理显示 1920×1080。A 为原加载器，B 为按 GNU Build ID、文件大小、目标架构、页大小和四个 PT_LOAD 布局限定的共享页加载器。两次均用同一部署入口重启宿主图形服务与 Waydroid；除 Android 的 libEGL_angle.so 外，两组部署产物的摘要相同（broker 保持 64 槽）。A/B 共两次容器启动，不是重复启动后的置信区间。

| 场景 | A 工作集 (MiB) | B 工作集 (MiB) | A−B (MiB) | A / B swap (MiB) | B 中发现驱动代码文件映射的进程数 |
|---|---:|---:|---:|---:|---:|
| 冷启动后的桌面 | 4862 | 2977 | 1885 | 0 / 0 | 10 |
| 设置 → Jelly → 计算器 | 5884 | 4047 | 1837 | 0 / 0 | 12 |
| Blue Archive 启动后约 35 秒 | 7059 | 5691 | 1368 | 676 / 130 | 14 |
| Blue Archive 启动后约 65 秒 | 5983 | 5189 | 794 | 1709 / 585 | 14 |

**口径：** 按[整栈内存口径](../README.md)统计，含 Android 容器及专属 WSL 服务，不含独立 GPU 显存、Windows 或无关 WSL 程序。每个阶段连续采 5 点、间隔 1 秒取均值；swap 单列，不算作驻留内存。每组从桌面依次打开设置、Jelly 和计算器，再启动游戏，不清应用数据、不清页缓存、不点击进入账号。两组相邻但不是同时：文件缓存、Android 后台进程、回收压力及游戏加载进度可能不同。游戏工作集差不能当作游戏进程自身节省，也不能直接外推到战斗场景。映射命中只计加载了项目 EGL 且内有 amdxc64.so 的 r-xp 映射的 Android 进程。

| 进程 | 阶段 | A 进程 PSS (MiB) | B 进程 PSS (MiB) | B 驱动代码映射命中 |
|---|---|---:|---:|---:|
| surfaceflinger | multi | 461 | 294 | 1 |
| com.android.systemui | multi | 434 | 253 | 1 |
| org.lineageos.jelly | multi | 380 | 213 | 1 |
| com.android.calculator2 | multi | 290 | 124 | 1 |
| com.nexon.bluearchive | game35 | 1588 | 1432 | 1 |

[整栈原始采样（字节）](samples.csv) · [进程采样（字节）](processes.csv) · [复测脚本](../../../validation/loader_memory.py) · [加载器实现](../../../src/loader_so.c)

部署前用新库在 Android shell UID 离屏编译着色器、绘制三角形并逐像素校验通过；版本标识筛选测试中，错误文件名、被改动的 Build ID 或不同段布局均未启用文件映射。该驱动共享策略已纳入图形基线。

游戏运行时，对 14 个命中进程另取了一次 smaps；SurfaceFlinger 和游戏的驱动代码段分别有 21732 / 36848 KiB Shared_Clean，两者 Private_Dirty 均为 0。[驱动代码段页统计](shared-code-kib.csv)。游戏约 65 秒时，含可回收文件页的整栈总计 A / B 为 7724 / 7989 MiB；尽管工作集和 swap 都较低，**不能说含缓存的总驻留在每个阶段都下降**。

Windows 侧截图确认了候选版桌面、游戏标题动画以及大厅中的人物、文字和图标。游戏刚启动时也截到了一张黑画面，随后进入动画和大厅；静态截图不能验证有无偶发闪帧。没有复测战斗或长期稳定性。原版 EGL 的 SHA-256 为 `87397532a9ab86439fbf40c1d28e53e8ad312a0ee8feeecb4f5ff43087429daa`，候选为 `055c0421d5e5d04319e505c4e33dd0010369f22bab50b9f15d0072bcfbd57688`，两组输入 broker 均为 `f080048f14bb000e2312ec1b6b2edd6657871f58612ac3261923744e967e6faa`。

**基线配置：** 保留限定驱动版本的共享页实现，broker 继续使用 64 槽。用户在所测版本手动从游戏切回桌面，未观察到显示异常；这次反馈不作为长期稳定性测试，也没有采集切回桌面后的内存数据。

---

<a id="english"></a>

# Sharing read-only AMD driver pages: system memory on device

2026-11-20, RX 7900 XTX, physical output 1920×1080. A was the original loader; B gated file-backed pages on GNU Build ID, file size, architecture, page size and four PT_LOAD layouts. Both used the same deployment and restart for host graphics services and Waydroid. All deployed hashes except Android libEGL_angle.so were identical; broker capacity was 64. Two container boots in total do not establish a confidence interval.

| Scene | A working set (MiB) | B working set (MiB) | A−B (MiB) | A / B swap (MiB) | Processes with file-backed driver code in B |
|---|---:|---:|---:|---:|---:|
| Home after cold boot | 4862 | 2977 | 1885 | 0 / 0 | 10 |
| Settings → Jelly → Calculator | 5884 | 4047 | 1837 | 0 / 0 | 12 |
| About 35 s after Blue Archive launch | 7059 | 5691 | 1368 | 676 / 130 | 14 |
| About 65 s after Blue Archive launch | 5983 | 5189 | 794 | 1709 / 585 | 14 |

**Scope:** the [Whole-stack memory definition](../README.md#english) includes Android and its dedicated WSL services, excluding Windows, dedicated VRAM and unrelated WSL processes. Each stage averaged 5 samples 1 s apart; swap is separate, not resident memory. Each run opened Settings, Jelly and Calculator from Home, then the game, without clearing app data/page cache or logging in. Runs were adjacent, not simultaneous. Cache, background processes, reclaim pressure and game loading progress could differ. A game-stage working-set difference is neither the game's own savings nor a prediction for combat. Mapping hits count Android processes that loaded project EGL and had an amdxc64.so r-xp mapping.

| Process | Stage | A process PSS (MiB) | B process PSS (MiB) | File-backed driver code in B |
|---|---|---:|---:|---:|
| surfaceflinger | multi | 461 | 294 | 1 |
| com.android.systemui | multi | 434 | 253 | 1 |
| org.lineageos.jelly | multi | 380 | 213 | 1 |
| com.android.calculator2 | multi | 290 | 124 | 1 |
| com.nexon.bluearchive | game35 | 1588 | 1432 | 1 |

[Raw whole-stack samples (bytes)](samples.csv) · [Process samples (bytes)](processes.csv) · [Reproduction script](../../../validation/loader_memory.py) · [Loader implementation](../../../src/loader_so.c)

Before deployment the new library passed offscreen shader compilation, triangle drawing and per-pixel validation as Android shell UID. Wrong filename, modified Build ID and mismatched segment layout all rejected file mapping. The gated sharing policy is in the graphics baseline.

A separate smaps snapshot covered 14 processes using the mapping while the game ran. SurfaceFlinger and the game had 21732 / 36848 KiB Shared_Clean respectively in driver code and both had zero Private_Dirty there. [Driver code-page counts](shared-code-kib.csv). At about 65 s the whole-stack total **including reclaimable file pages** was A / B = 7724 / 7989 MiB: lower working set and swap do not imply lower total resident memory at every stage.

Windows screenshots confirmed Home, the game title animation, and characters, text and icons in the lobby. One early launch screenshot was black before the animation and lobby appeared; static captures cannot rule out occasional flashes. Combat and long-term stability were not retested. Original EGL SHA-256: `87397532a9ab86439fbf40c1d28e53e8ad312a0ee8feeecb4f5ff43087429daa`; candidate: `055c0421d5e5d04319e505c4e33dd0010369f22bab50b9f15d0072bcfbd57688`; broker in both: `f080048f14bb000e2312ec1b6b2edd6657871f58612ac3261923744e967e6faa`.

**Baseline:** keep exact-build driver sharing and 64 broker slots. The user manually switched from the game to Home without seeing a display issue. No memory reading at that point and no long-duration validation.
