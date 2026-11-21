# ELF 清理与 Gallium 共享页：整栈部署对照

[English](#english)

2026-11-20，本机 1920×1080。A 是已接受的 AMD 驱动共享页版本；C 在此基础上释放 ELF 解析输入映射与文件描述符，并共享已核对版本的 Mesa Gallium 只读页。只更换 libEGL_angle.so，broker 保持 64 槽。两组分别通过同一部署脚本重启 Weston 和 Android 容器，未清缓存，未启动游戏。

| 场景 | A 工作集 (MiB) | C 工作集 (MiB) | A−C (MiB) | 容器匿名内存 A / C (MiB) | swap A / C (MiB) |
|---|---:|---:|---:|---:|---:|
| 桌面 | 2960.3 | 2781.9 | 178.4 | 2130.9 / 1963.9 | 0.0 / 0.0 |
| 设置 → Jelly → 计算器 | 3515.0 | 3297.2 | 217.8 | 2432.7 / 2215.5 | 0.0 / 0.0 |

| 场景 | 版本 | 映射项目 EGL 的进程数 | AMD 代码文件映射命中 | Gallium 代码文件映射命中 | 指定库目录下仍打开的 .so 句柄 |
|---|---|---:|---:|---:|---:|
| 桌面 | A | 10 | 10 | 0 | 140 |
| 桌面 | C | 10 | 10 | 10 | 0 |
| 设置 → Jelly → 计算器 | A | 12 | 12 | 0 | 168 |
| 设置 → Jelly → 计算器 | C | 12 | 12 | 12 | 0 |

复测：WSL root 下，在原版和候选各自完成重启后运行 `python3 -B validation/loader_memory.py <新的输出目录> --ui-only`。HOME 后等待 25 秒；依次启动设置、Jelly 和计算器，每次等待 4 秒。各阶段采 5 点取均值；每版仅一次启动，不是跨多次启动的均值。[工作集口径](../../README.md)含容器和专属 WSL 服务，不含 Windows / GPU 专用显存。内核页缓存和后台任务可能在两次启动间不同，不能把单次差值完全归因于改动；本次没有量游戏内存、帧率或长期稳定性。

[整栈原始数据（字节）](samples.csv) · [进程、映射和句柄](processes.csv) · [复测脚本](../../../../validation/loader_memory.py)

Windows 侧核对候选重启后的桌面、设置、计算器和返回桌面四张画面，抽查未见图标缺失或桌面残留应用 taskbar。验证结束时 Android crash 缓冲区为空，容器、Weston、输入及消费服务均 active。静态画面不构成逐帧或长时间验证。

---

<a id="english"></a>

# ELF cleanup and Gallium sharing: on-device whole-stack comparison

2026-11-20, 1920×1080. A already shared AMD driver pages; C also released ELF parser mappings and fds and shared read-only pages of the verified Gallium build. Only libEGL_angle.so changed; the broker retained 64 slots. The same script restarted Weston and Android for both runs. Caches were not cleared and no game was launched.

| Scene | A working set (MiB) | C working set (MiB) | A−C (MiB) | Container anonymous memory A / C (MiB) | swap A / C (MiB) |
|---|---:|---:|---:|---:|---:|
| Home | 2960.3 | 2781.9 | 178.4 | 2130.9 / 1963.9 | 0.0 / 0.0 |
| Settings → Jelly → Calculator | 3515.0 | 3297.2 | 217.8 | 2432.7 / 2215.5 | 0.0 / 0.0 |

| Scene | Version | Processes mapping the project's EGL | AMD file-backed code mappings | Gallium file-backed code mappings | Open .so handles under the specified library directory |
|---|---|---:|---:|---:|---:|
| Home | A | 10 | 10 | 0 | 140 |
| Home | C | 10 | 10 | 10 | 0 |
| Settings → Jelly → Calculator | A | 12 | 12 | 0 | 168 |
| Settings → Jelly → Calculator | C | 12 | 12 | 12 | 0 |

To reproduce, as WSL root run `python3 -B validation/loader_memory.py NEW_OUTPUT_DIR --ui-only` after each version restarts. Wait 25 s after HOME, then launch Settings, Jelly and Calculator in order, waiting 4 s each. Each stage is the mean of 5 points from **one boot per version**, not a mean across boots. The [Working-set definition](../../README.md#english) includes Android and dedicated WSL services, not Windows or dedicated GPU memory. Page cache and background tasks can vary between boots; the whole difference is not necessarily causal. Game memory, FPS and long-term stability were not measured.

[Raw whole-stack data (bytes)](samples.csv) · [Processes, mappings and handles](processes.csv) · [Reproduction script](../../../../validation/loader_memory.py)

Windows screenshots of Home, Settings, Calculator and return to Home showed no missing icons or app taskbar left on Home. The Android crash buffer was empty at the end; container, Weston, input and consumer services were active. Static inspection is not frame-by-frame or long-duration verification.
