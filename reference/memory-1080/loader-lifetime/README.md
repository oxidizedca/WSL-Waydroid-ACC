# ELF 临时映射回收与 Mesa Gallium 共享页（2026-11-20）

[English](#english)

以已开启 AMD 共享页的 EGL 为 A，对照 B（回收解析视图与 fd）和 C（在 B 上共享 Gallium 只读页）。使用相同 NDK 和其余对象，在独立 Android shell UID 进程测试[加载器](../../../src/loader_so.c)。C 的两项改动均纳入基线；整栈影响见[部署对照](deployed/README.md)。

**生命周期。** load_elf 的整文件只读视图用于解析动态段、节表和递归载入依赖。返回的符号表、字符串表、TLS、展开信息指向运行时映射；程序头已有独立副本。旧实现在成功路径未释放输入视图，也未关闭其 fd。加载器在依赖与重定位完成后解除视图、关闭 fd，不解除运行时映射。这是每加载一份库的固定留存，不是每帧增长。

**文件页。** 保留原 AMD 白名单，增列本项目已核对的 libgallium-26.0.8.so。该文件 SHA-256 为 `e3493819cd3ace11dbe4815cd12a2717432e09051ee11a10b8b13a48ccf4c448`，GNU Build ID 为 `c7224630093249c7213125e590de8743d2231d18`。用 readelf 审核 20346 个重定位目标及写入宽度（含 16 字节 TLSDESC），均落在可写段。仅完整的只读/可执行页使用文件映射，非整页尾部与可写部分仍私有；文件大小、Build ID 或段布局不匹配时沿用拷贝。没有泛化到所有 Mesa 构建。

| 独立进程，4 次均值 | PSS (MiB) | 匿名 PSS (MiB) | 整文件临时视图 | 仍打开的装载输入 fd |
|---|---:|---:|---:|---:|
| A：对照 EGL（仅共享 AMD 页） | 160.25 | 78.86 | 14 | 14 |
| B：仅释放输入视图与 fd | 111.20 | 78.85 | 0 | 0 |
| C：B + Gallium 共享页 | 101.71 | 59.24 | 0 | 0 |

A/B/C 顺序为 ABCCBABCACBA；同一容器，串行启动新进程。各次用同一份[64×64 像素探针](../loader-shader-probe.c)，成功输出后多停留 2 秒供采样：三次清屏、编译/链接顶点和片元着色器、绘制三角形、核对全部像素与 GL 状态。所有测试均命中 D3D12。C 的 AMD、Gallium 均有独立可执行文件映射；另在临时目录只改变 Gallium Build ID 的一个字节，C 对它退回匿名拷贝，AMD 仍命中共享路径，像素检查同样通过。

**如何解读。** A 的 14 个输入视图合计约 210.12 MiB **虚拟地址**。测试在初始化和首批绘制后立即取样，并不是桌面稳态：只读现场快照中，7 个图形进程仍持有 98 个装载输入 fd，但这 98 份整文件视图当时的 PSS 合计仅 4.31 MiB。文件页可以共享和回收，进程 PSS 的改变也会改变其他进程分摊的比例；不能用探针的 58.54 MiB 差乘以进程数来预测整机节省。相较 B，C 的匿名 PSS 另降 19.61 MiB，这是 Gallium 私有复制减少的直接证据。整栈实测见[部署对照](deployed/README.md)；离屏数据不用于推算帧率或长期稳定性。

[12 次离屏原始采样](offscreen.csv) · [对照组安装快照](installed-snapshot.csv) · [不匹配构建回退](unknown-build.csv)

---

<a id="english"></a>

# Releasing ELF parser mappings and sharing Mesa Gallium pages (2026-11-20)

A had AMD sharing enabled; B released parser views and fds; C added Gallium read-only sharing on top of B. The NDK and remaining objects were identical. The [Loader](../../../src/loader_so.c) was tested in independent Android shell-UID processes. Both C changes are in the baseline; see the [On-device comparison](deployed/README.md#english) for whole-stack impact.

**Lifetime.** load_elf uses a read-only view of the whole file to parse dynamic and section tables and load dependencies recursively. Returned symbol/string tables, TLS and unwind information refer to runtime mappings; program headers have their own copy. The old success path retained both the input view and its fd. The loader now drops both after dependencies and relocations finish, without unmapping runtime data. This was fixed retention per loaded library, not growth per frame.

**File-backed pages.** The verified libgallium-26.0.8.so was added alongside AMD: SHA-256 `e3493819cd3ace11dbe4815cd12a2717432e09051ee11a10b8b13a48ccf4c448`, GNU Build ID `c7224630093249c7213125e590de8743d2231d18`. readelf inspection of all 20346 relocation targets and write widths (including 16-byte TLSDESC) placed every write in a writable segment. Only complete read-only/executable pages use file mappings; partial tails and writable regions remain private. Unknown sizes, Build IDs or layouts keep the copy path. This is not enabled for arbitrary Mesa builds.

| Independent process, mean of 4 runs | PSS (MiB) | Anonymous PSS (MiB) | Temporary whole-file views | Open loader input fds |
|---|---:|---:|---:|---:|
| A: control EGL (AMD sharing only) | 160.25 | 78.86 | 14 | 14 |
| B: release input views and fds only | 111.20 | 78.85 | 0 | 0 |
| C: B plus shared Gallium pages | 101.71 | 59.24 | 0 | 0 |

Order ABCCBABCACBA, fresh processes started serially in one container. The same [64×64 pixel probe](../loader-shader-probe.c) cleared three times, compiled/linked vertex and fragment shaders, drew a triangle and checked every pixel and GL state, then waited 2 s for sampling. All used D3D12. C had separate file-backed executable mappings for AMD and Gallium. Changing one byte of Gallium's Build ID in a temporary copy made C fall back to anonymous copying for Gallium alone; AMD remained shared and pixels still passed.

**Interpretation.** A's 14 input views covered about 210.12 MiB of **virtual address space**. The probe sampled just after initialization and first draws, not steady-state Home. In a live snapshot, 7 graphics processes still held 98 input fds but the 98 whole-file views had only 4.31 MiB combined PSS. File pages are shared and reclaimable, and PSS attribution changes across processes. Do not multiply the probe's 58.54 MiB difference by the process count. C reduced anonymous PSS another 19.61 MiB versus B, direct evidence of fewer private Gallium copies. See [On-device comparison](deployed/README.md#english); offscreen results do not predict FPS or long-term stability.

[12 raw offscreen samples](offscreen.csv) · [Control installation snapshot](installed-snapshot.csv) · [Fallback for unknown builds](unknown-build.csv)
