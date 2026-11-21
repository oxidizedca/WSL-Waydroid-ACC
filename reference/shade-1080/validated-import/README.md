# 1080p 通知栏：通过校验后复用 GPU 导入对象

[English](#english)

对照 A 是已启用 BLAKE3 的版本，B 仅改变 Android EGL shim。对于已经回填到 CPU 的缓冲区，SurfaceFlinger 每次仍要向 broker 请求纹理：设备/inode、代次、序号和整块 CPU 镜像校验通过，才允许使用 GPU 快照。如果同一 EGLImage、GL 上下文、分配、代次、尺寸已经导入过，B 复用 Mesa 的资源引用，不重复打开 D3D12 资源。未通过 GET 不进入此缓存。不会为可写平面保存返回的 fd；销毁 EGLImage 时清理其缓存，沿用原有 16 项上限。

## 分段验证

- 改前，计算器上做 3 次展开/收起，用 uprobe 记录 RenderEngine 的请求：500 次成功 GET 对应 464 个不同的（分配、nonce），GET 中位数 0.12 ms、P95 3.63 ms；11 次成功 BOUND 共 9.91 ms。这次优先处理重复导入，不改确认协议。
- 另以现有短时绑定日志分别观察 A、B 的 3 次展开/收起。A 的 515 次「经过 GET、导入未命中」绑定 P50 为 0.798 ms。B 有 39 次「经过 GET、导入命中」，GET P50 0.320 ms、绑定 P50 0.0044 ms；473 次未命中的绑定 P50 为 0.779 ms。这里的绑定耗时不含 GET，不是整帧节省量。

## 端到端测量

RX 7900 XTX，1920×1080、60 Hz，计算器前台。每组预热 2 轮，再用 [shade.py](../../../validation/shade.py) 采 6 次展开和收起，统计 SurfaceFlinger 实际显示帧。顺序 A1、A2、部署并重启容器、B1、B2。每个版本合并两组逐帧样本重新求百分位；不是同一次启动交叉对照。函数探针和绑定日志不在帧时计时阶段启用。

| 动作 | A / B 帧数 | P50 ms，A / B | P95 ms，A / B | 超过 1000/60 ms，A / B |
|---|---:|---:|---:|---:|
| 展开 | 451 / 452 | 9.70 / 9.57 | 19.02 / 16.15 | 30 / 22 |
| 收起 | 468 / 467 | 9.44 / 9.42 | 14.12 / 14.16 | 6 / 2 |
| 合计 | 919 / 919 | 9.56 / 9.49 | 15.85 / 15.19 | 36 / 24 |

收起 P95 基本不变；B1 单组展开 P95 为 16.90 ms，仍有超过预算的帧。不据此推断游戏帧率、CPU 或内存变化。

A / B 的 libEGL_angle.so SHA-256 分别为 `c1f9599d2b12a60a9cecad7f186b7b4471ba75997cad4b21cbe8cefa03e1654b` / `87397532a9ab86439fbf40c1d28e53e8ad312a0ee8feeecb4f5ff43087429daa`。D3D12 离屏检查通过；Windows 侧抽查了计算器启动、通知栏及收起后的应用画面，不是 60 FPS 逐帧视觉检查。

[每组结果](runs.csv) · [逐帧原始时间戳和耗时](frames.csv)

后续已对齐 GET 的两侧时间线：[请求排队、broker 服务与旧槽淘汰归因](../get-queue/README.md)。

---

<a id="english"></a>

# 1080p shade: reuse GPU imports after validation

A already used BLAKE3. B changed only the Android EGL shim. For a buffer backfilled to CPU, SurfaceFlinger must still request the texture each time; device/inode, generation, serial and the whole CPU mirror must validate before using the snapshot. For a matching previously imported EGLImage, GL context, allocation, generation and dimensions, B reuses the Mesa resource reference rather than reopening the D3D12 resource. Failed GETs never enter the cache. Returned fds are not cached for writable planes. EGLImage destruction clears entries; the limit remains 16.

## Stage validation

- Before this change, uprobes during three Calculator shade cycles saw 500 successful RenderEngine GETs for 464 unique (allocation, nonce) pairs. GET median 0.12 ms, P95 3.63 ms; 11 successful BOUND calls totaled 9.91 ms. This change addresses repeated imports, not the acknowledgement protocol.
- Separate brief bind logs covered three cycles for A and B. A had 515 post-GET import misses with bind P50 0.798 ms. B had 39 post-GET import hits (GET P50 0.320 ms, bind P50 0.0044 ms); 473 misses had bind P50 0.779 ms. Bind time excludes GET and is not the whole-frame saving.

## End-to-end measurement

RX 7900 XTX, 1920×1080/60 Hz, Calculator foreground. Each group warmed twice then [shade.py](../../../validation/shade.py) measured six open/close cycles using actual SurfaceFlinger display frames. Order A1/A2, deploy and restart Android, B1/B2. Each build's two groups were pooled before computing percentiles; not an interleaved same-boot comparison. Probes and bind logs were off during timing.

| Action | Frame count, A / B | P50 ms, A / B | P95 ms, A / B | Over 1000/60 ms, A / B |
|---|---:|---:|---:|---:|
| Open | 451 / 452 | 9.70 / 9.57 | 19.02 / 16.15 | 30 / 22 |
| Close | 468 / 467 | 9.44 / 9.42 | 14.12 / 14.16 | 6 / 2 |
| Total | 919 / 919 | 9.56 / 9.49 | 15.85 / 15.19 | 36 / 24 |

Close P95 hardly changed; B1 alone had open P95 16.90 ms, so some frames still exceeded budget. No inference about game FPS, CPU or memory.

A / B libEGL_angle.so hashes: `c1f9599d2b12a60a9cecad7f186b7b4471ba75997cad4b21cbe8cefa03e1654b` / `87397532a9ab86439fbf40c1d28e53e8ad312a0ee8feeecb4f5ff43087429daa`. D3D12 offscreen checks passed. Windows spot checks covered Calculator launch, the shade and the app after closing; this was not a 60 FPS per-frame visual inspection.

[Results by group](runs.csv) · [Raw frame timestamps and durations](frames.csv)

The two-sided GET timelines are aligned in [Request queueing, broker service and slot eviction](../get-queue/README.md#english).
