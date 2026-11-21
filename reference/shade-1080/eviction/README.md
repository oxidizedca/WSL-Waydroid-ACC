# 64 槽淘汰实验：优先淘汰已有 CPU 镜像的冷槽

[English](#english)

**结果：不纳入默认构建。** 变更仅针对输入 broker。未改 Android、Weston、协议、恢复或像素校验。A 为原轮转策略；B 只在轮到一个尚未回填的槽时，向后查看 7 个槽，优先选空槽或自最近一次发布/成功 GET 以来至少经过 8 次新槽选择、且已有 CPU 镜像的最冷槽。一个代次最多跳过一次轮转；无合适候选仍执行原回填。实验补丁保存在 [candidate.patch](candidate.patch)；基线采用 A。

## 显示帧

同一次 Waydroid 启动，计算器前台、1920×1080、60 Hz。仅重启输入 broker；每组先做 8 轮展开/收起使缓存进入工作状态，然后运行 [shade.py](../../../validation/shade.py) 自带的 2 轮预热、6 轮计时。顺序 A1、B1、B2、A2；追加的 B3、A3 将初始预热加倍至 16 轮。分支样本合并后重新计算 P95。

| 对照 | 展开 P95，A / B | 展开超 16.67 ms，A / B | 收起 P95，A / B |
|---|---:|---:|---:|
| 前四组 | 19.04 / 16.98 ms | 39/456 / 24/457 | 13.90 / 14.33 ms |
| 加长预热，末两组 | 17.41 / 17.03 ms | 14/228 / 14/231 | 13.91 / 14.41 ms |

A2 期间存在额外的停机读回观察探针。保守剔除整对 A2/B2，只计未挂函数探针的 A1/B1、B3/A3，展开 P95 为 18.19 / 16.80 ms，收起为 13.97 / 14.41 ms。不是所有动作都变快。Windows 侧截图抽查了通知栏、计算器和返回桌面；不是逐帧视觉检查。

## 读回、回退与停机清点

每组帧时测试结束后另做 6 轮函数采样。读回以实际进入 glReadPixels 的可见像素数 × 4 统计，不用“选中 lazy 槽”推算，也不是总线带宽测量。

| 三组诊断采样合计 | A | B |
|---|---:|---:|
| 淘汰触发的 GPU 读回 | 106 次 / 530.36 MiB | 109 次 / 491.95 MiB |
| 共享输入绑定成功 / 尝试 | 4,626 / 7,268 | 4,626 / 7,274 |
| RenderEngine 外部纹理 CPU 镜像上传成功 | 2,641 | 2,647 |
| RenderEngine GET 成功 / 请求 | 3,075 / 3,076 | 3,080 / 3,080 |

最后一对 A3/B3 单看，淘汰读回为 36 次 / 187.18 MiB 与 37 次 / 186.68 MiB；处理发布请求的 broker 服务 P95 为 3.319 / 3.522 ms。绑定、CPU 上传和 GET 是不同层级的计数，不可混作一个命中率。

另外，在 B2 和 A2 测完并返回桌面后，观察各自进程正常停机时的最后一次回填：A 为 40 次 / 195.79 MiB，B 为 49 次 / 251.63 MiB。这只是一对停机样本，不是 RSS，也不与三组诊断采样直接相加。它表明部分成本可能移出动画窗口，不能把动画期间的字节差当作已消除的工作。

A、B 均通过 `bash build.sh --offscreen` 检查。

[每组帧时](runs.csv) · [逐帧时间戳](frames.csv) · [独立函数采样](counts.csv) · [停机清点](drain.csv)

---

<a id="english"></a>

# 64-slot eviction experiment: prefer cold slots with CPU mirrors

**Not in the default build.** Only the input broker changed; Android, Weston, protocol, recovery and pixel checks were unchanged. A used round-robin replacement. When the next slot needed backfill, B looked ahead seven slots, preferring an empty slot or the coldest already-mirrored slot untouched by publication or successful GET for at least eight new-slot selections. Each generation could skip rotation once; absent a suitable slot, backfill proceeded normally. See [candidate.patch](candidate.patch). Baseline uses A.

## Display frames

Same Waydroid boot and foreground Calculator at 1920×1080/60 Hz. Only the input broker restarted. Eight open/close cycles populated caches per group, followed by [shade.py](../../../validation/shade.py) with its own two warm-ups and six timed cycles. Order A1/B1/B2/A2, then B3/A3 with initial warm-up doubled to 16 cycles. Branch P95s were recomputed from pooled frames.

| Comparison | Open P95, A / B | Open >16.67 ms, A / B | Close P95, A / B |
|---|---:|---:|---:|
| First four groups | 19.04 / 16.98 ms | 39/456 / 24/457 | 13.90 / 14.33 ms |
| Longer warm-up, final two groups | 17.41 / 17.03 ms | 14/228 / 14/231 | 13.91 / 14.41 ms |

A2 had an additional probe observing shutdown readback. Conservatively excluding the whole A2/B2 pair, A1/B1 and B3/A3 without function probes yielded opening P95 18.19 / 16.80 ms and closing P95 13.97 / 14.41 ms. Not every action improved. Windows screenshots spot-checked the shade, Calculator and return to Home, not every frame.

## Readbacks, fallback and shutdown drain

After each timing group, six separate cycles sampled functions. Readback bytes count visible pixels actually entering glReadPixels × 4, not estimates from lazy slot selections or measured bus bandwidth.

| Sum over three diagnostic groups | A | B |
|---|---:|---:|
| GPU readback on eviction | 106 calls / 530.36 MiB | 109 calls / 491.95 MiB |
| Successful shared input binds / attempts | 4,626 / 7,268 | 4,626 / 7,274 |
| Successful RenderEngine external-texture CPU mirror uploads | 2,641 | 2,647 |
| Successful RenderEngine GETs / requests | 3,075 / 3,076 | 3,080 / 3,080 |

In A3/B3 alone, eviction readback was 36 calls / 187.18 MiB vs 37 calls / 186.68 MiB; broker publish service P95 was 3.319 / 3.522 ms. Bind, CPU upload and GET counts are at different levels and do not form a single hit rate.

After B2 and A2 returned Home, final backfills on normal broker shutdown were counted: A 40 calls / 195.79 MiB, B 49 calls / 251.63 MiB. This one shutdown pair is neither RSS nor additive to the three diagnostic groups. Work may have shifted outside the animation window; bytes not read during the animation are not necessarily work eliminated.

Both versions passed `bash build.sh --offscreen`.

[Frame times by group](runs.csv) · [Frame timestamps](frames.csv) · [Separate function samples](counts.csv) · [Shutdown drain](drain.csv)
