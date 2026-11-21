# 1920×1080 通知栏：CPU 镜像的全量校验

[English](#english)

仅替换宿主输入 broker 进程内的摘要实现。CPU 镜像写完后保留 32 字节 BLAKE3；再次借出 GPU 纹理前仍按实际 stride×height×4 校验。不可变代次的快速路径、恢复 fd、GPU fence、可见性、Android 和 Weston 二进制没有改变。摘要不进入协议或恢复存储。没有添加工作线程或额外整帧缓存。

## 计时

同一 Android 启动、60 Hz、计算器前台、三项共享纹理优化全开。顺序 B1、A2、A3、B2（A 为上一版 SHA-256，B 为 BLAKE3）。中途只替换/重启输入 broker，每组先预热，再执行 6 次展开和收起。Perfetto 记录 SurfaceFlinger 的实际显示帧。计时不运行函数探针或录屏。A1 是部署前单独的一组，保留在原始记录中，不参与下表。按帧合并计算百分位，而非平均两批 P95。

| 动作 | A / B 帧数 | P50 ms，A / B | P95 ms，A / B | 超过 1000/60 ms，A / B |
|---|---:|---:|---:|---:|
| 展开 | 432 / 441 | 9.47 / 9.37 | 25.78 / 19.00 | 52 / 31 |
| 收起 | 471 / 458 | 9.34 / 9.24 | 14.41 / 13.90 | 7 / 1 |
| 合计 | 903 / 899 | 9.40 / 9.29 | 19.26 / 15.22 | 59 / 32 |

展开和收起不是同一负载；合计 P95 不能解释为展开已稳定达到 60 Hz。原始 A 的展开 P95 在 22.66 至 29.41 ms 间波动，以上数值不是跨设备保证。[逐组结果及 broker 哈希](runs.csv) · [逐帧样本](frames.csv) · [单组采集脚本](../../validation/shade.py)。以 WSL root 运行 `python3 validation/shade.py /tmp/未使用的目录`；需已安装 trace processor，脚本不会切换版本。

## 分段归因（独立于帧时采样）

4 次展开/收起，在 broker 线程对摘要函数入口和返回做 uprobe 计时，两次均遇到 37 次 8,294,400 字节的全屏校验。P50 / P95 从 3.24 / 3.55 ms 降至 0.81 / 0.97 ms；完整镜像回填 P50 从 4.54 降至 2.84 ms。未统计整个容器 CPU 或 RSS。probe 文件中列依次为阶段、次数、总耗时、P50、P95、最大值，耗时单位 ms。[旧版](sha256-probe.txt) · [新版](blake3-probe.txt)

已构建并通过 D3D12 离屏测试；摘要与 Ubuntu 独立库在七种输入长度下一致，更改单个可见字节、行尾 padding 或分配的最后一个字节均改变摘要。部署后 Windows 侧分别核看通知栏展开、收起后的计算器及返回桌面；静态截图不用于排除单帧异常。

---

<a id="english"></a>

# 1920×1080 notification shade: validating the complete CPU mirror

Only the host broker's in-process digest changed. It saves a 32-byte BLAKE3 digest after mirror writes, then validates the actual stride×height×4 bytes before lending the GPU texture again. Immutable-generation fast path, recovery fd, GPU fence, visibility, Android and Weston binaries are unchanged. No digest enters protocol or persisted recovery data. No extra worker thread or full-frame cache.

## Timing

Same Android boot, Calculator at 60 Hz, all three shared-texture optimizations enabled. Order B1/A2/A3/B2 (A used SHA-256, B BLAKE3). Only the broker was replaced/restarted between groups; after warm-up each ran six shade cycles. Perfetto recorded actual SurfaceFlinger display frames. No function probes or video during timing. Predeployment A1 remains in the raw data but not the table. Percentiles are pooled by frame, not averaged from batch P95s.

| Action | Frame count, A / B | P50 ms, A / B | P95 ms, A / B | Over 1000/60 ms, A / B |
|---|---:|---:|---:|---:|
| Open | 432 / 441 | 9.47 / 9.37 | 25.78 / 19.00 | 52 / 31 |
| Close | 471 / 458 | 9.34 / 9.24 | 14.41 / 13.90 | 7 / 1 |
| Total | 903 / 899 | 9.40 / 9.29 | 19.26 / 15.22 | 59 / 32 |

Open and close are different workloads; their combined P95 is not proof of consistent 60 Hz opening. Original A opening P95 ranged from 22.66 to 29.41 ms. Results are device-specific. [Group results and broker hashes](runs.csv) · [Per-frame samples](frames.csv) · [Single-group collector](../../validation/shade.py). Run `python3 validation/shade.py /tmp/UNUSED_OUTPUT_DIR` as WSL root with trace processor installed. The script does not switch builds.

## Stage attribution (separate from frame timing)

Over four open/close cycles, uprobes on broker digest function entry/return observed 37 full-screen checks of 8,294,400 bytes in each version. P50 / P95 fell from 3.24 / 3.55 ms to 0.81 / 0.97 ms; complete mirror backfill P50 from 4.54 to 2.84 ms. No whole-container CPU or RSS measurement. Probe output columns: stage, count, total, P50, P95 and maximum, with times in ms. [Before](sha256-probe.txt) · [After](blake3-probe.txt).

Build and D3D12 offscreen tests passed. Digests matched an independent Ubuntu library at seven input lengths. Changing one visible byte, row padding or the final allocation byte changed the digest. Windows screenshots after deployment checked the open shade, Calculator after closing and return to Home; static screenshots cannot rule out a one-frame glitch.
