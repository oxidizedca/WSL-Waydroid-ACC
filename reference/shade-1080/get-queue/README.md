# 通知栏：SurfaceFlinger 的 GET 慢在哪里

[English](#english)

**结论：慢在等正在处理其他请求的输入 broker，不是 GET 自身通常需要几毫秒。** 这是对 broker 和 RenderEngine 的双端采样，不是新增优化。场景为 1920×1080、60 Hz、计算器前台；同一次启动中按 thin1、detail1、detail2、thin2、slots1、slots2 顺序测量。每组预热 2 轮、记录 6 轮展开/收起，短时 uprobe 用完即卸载。没有用这些采样推断显示帧时。

## 两侧时间线

只配对 RenderEngine 返回成功和 broker 返回成功的 GET，按分配 ID、nonce、serial 和时间区间匹配。每组内第一次观察到该键记为 first，不代表分配生命周期的第一次。两组轻量采样合得 2,047 个请求，其中 1,864 个是 first。

| 首次观察到的 GET | 个数 | 端到端 P95 | 等待分派 P95 | broker 服务 P95 |
|---|---:|---:|---:|---:|
| 展开 | 947 | 3.662 ms | 3.543 ms | 0.059 ms |
| 收起 | 917 | 2.988 ms | 2.894 ms | 0.047 ms |
| 合计 | 1,864 | 3.387 ms | 3.190 ms | 0.050 ms |

其中 178 个 GET 耗时至少 2 ms。按这些请求的耗时求和：总计 713.0 ms，等待分派 684.7 ms（96.0%），broker 服务 17.3 ms，余下为客户端准备和收包。等待分派定义为客户端调用 sendmsg 至 broker 进入对应 serve_input；若 broker 先接入并等待客户端发送，该段记零。这是墙钟时间，也可包括调度、套接字和分派开销，不是内核 socket 队列的精确驻留时间。服务段截止 broker 开始发送回复。四段逐请求相加等于总耗时，**百分位不可相加**。

## 哪些发布工作挡住了请求

两组 detail 里，耗时至少 2 ms 的 GET 在通知栏**展开**期间累计等待分派 448.9 ms。把等待窗口与 broker 的函数执行时间取交集，嵌套区间只计一次：

| 正在执行的工作 | 与等待重叠 |
|---|---:|
| 新发布帧的 CPU 镜像，包括摘要 | 92.0 ms |
| 发布端导入 GPU 纹理（不是 RenderEngine 的 GET） | 86.3 ms |
| 淘汰旧槽前补写其 CPU 镜像，包括摘要 | 82.9 ms |
| 释放旧资源 | 49.1 ms |
| 等待生产者 fence 的 poll | 1.5 ms |

只列部分阶段；不能把交集直接算成优化收益。两组独立 slots 采样把回填写入进一步拆到 ReadPixels，发现展开时淘汰回填的重叠为 102.2 ms，其中 ReadPixels 79.3 ms。这不是说 CPU 消耗了 79.3 ms；该调用也可以等待 GPU。

## 64 槽满时的队首等待

当前没有同分配槽可复用时，broker 按固定顺序选择 64 槽之一。若旧槽只有 GPU 快照（lazy），需先回填 CPU 平面才能释放。两组 slots 共观察 115 次选槽，均已占满；其中 74 次选中 lazy 槽。在这些选择的当时状态下，向后看最多 3 个槽，64/74 次能找到已有 CPU 镜像的槽；向后最多 7 个，74/74 次能找到。这是**对原策略的现场快照**：改用另一个槽会改变之后的状态，不代表 74 次都可长期消除。

两种针对排队的变体均未纳入基线：[优先淘汰已有 CPU 镜像的冷槽](../eviction/README.md)没有稳定减少总读回；[导入和回填前处理队首 GET](../queue-checkpoints/README.md)缩短了分派等待，但没有改善展开尾帧。基线保留 64 槽轮转和原有查询间隙调度。

## 复测

[逐请求分段（µs）](requests.csv) · [分阶段、不重复计时的重叠（µs）](overlaps.csv) · [每次选槽快照摘要](slots.csv)。

在此目录用 WSL root 运行。采集器要求从桌面开始，以 SHA-256 绑定本次 broker 和 shim，避免不同构建的指令地址被误用。以下输出目录应尚不存在：

```sh
root=$(mktemp -d)
for g in thin1 detail1 detail2 thin2 slots1 slots2; do
    case "$g" in
        thin*)   python3 capture.py "$root/$g" ;;
        detail*) python3 capture.py "$root/$g" --detail ;;
        slots*)  python3 capture.py "$root/$g" --detail --slots ;;
    esac
    sleep 1
done
python3 analyze.py "$root" "$root/thin1" "$root/detail1" "$root/detail2" "$root/thin2" "$root/slots1" "$root/slots2"
```

---

<a id="english"></a>

# Where SurfaceFlinger's GET waits during notification-shade animation

**Most long GETs waited for a broker busy with other requests; GET service itself did not normally take milliseconds.** This was two-sided sampling, not a new optimization. At 1920×1080/60 Hz over Calculator, one boot ran thin1, detail1, detail2, thin2, slots1 and slots2. Each group warmed twice then recorded six open/close cycles. Short-lived uprobes were detached afterward. Display-frame times were not inferred from these samples.

## Timing both endpoints

Only GETs succeeding in both RenderEngine and broker were paired, by allocation ID, nonce, serial and time window. The first occurrence of a key per group was marked first, not necessarily the first in that allocation's lifetime. The two lightweight groups had 2,047 requests, 1,864 marked first.

| First observed GET | Count | End-to-end P95 | Wait for dispatch P95 | Broker service P95 |
|---|---:|---:|---:|---:|
| Open | 947 | 3.662 ms | 3.543 ms | 0.059 ms |
| Close | 917 | 2.988 ms | 2.894 ms | 0.047 ms |
| Total | 1,864 | 3.387 ms | 3.190 ms | 0.050 ms |

Across 178 GETs lasting at least 2 ms, summed duration was 713.0 ms: 684.7 ms (96.0%) waiting for dispatch, 17.3 ms broker service, remainder client preparation and receive. Dispatch wait starts at client sendmsg and ends at matching serve_input entry; if the broker was already waiting for the client's send, wait counts as zero. Wall-clock time can include scheduling, socket and dispatch overhead; it is not exact kernel socket queue residence. Service ends when the broker begins replying. The four intervals sum for each request; **percentiles do not add**.

## Which publish work blocks GET?

In the two detailed groups, GETs of at least 2 ms accumulated 448.9 ms of dispatch wait during **opening**. The table intersects wait windows with broker functions, counting nested intervals once.

| Work in progress | Overlap with wait |
|---|---:|
| CPU mirror of new frame, including digest | 92.0 ms |
| GPU texture import during publish (not a RenderEngine GET) | 86.3 ms |
| CPU mirror backfill before eviction, including digest | 82.9 ms |
| Release old resources | 49.1 ms |
| Poll on producer fence | 1.5 ms |

Only selected stages are shown. Overlaps are not recoverable time by definition. Two separate slot samples traced backfills into ReadPixels: 102.2 ms of overlap with eviction backfill on opening, 79.3 ms inside ReadPixels. The call may wait on the GPU; this is not 79.3 ms of CPU execution.

## Head-of-line waits with all 64 slots occupied

If the allocation has no reusable slot, the broker selects one of 64 in fixed order. A GPU-only (lazy) generation requires CPU backfill before release. Across two slot samples all 115 selections saw full occupancy; 74 selected a lazy slot. Looking ahead three slots in those snapshots found an existing CPU mirror in 64/74 cases; looking ahead seven found one in 74/74. These are **snapshots of the original policy**. Choosing another slot changes later states, so this does not promise 74 permanently avoided readbacks.

Neither queueing variant was adopted: [Prefer cold slots with existing CPU mirrors](../eviction/README.md#english) did not consistently reduce total readback; [Service the front GET before import and backfill](../queue-checkpoints/README.md#english) shortened dispatch wait without improving opening tail frames. The baseline keeps 64-slot rotation and its existing query-gap dispatch.

## Reproduction

[Request stages (µs)](requests.csv) · [Non-overlapping time attribution by stage (µs)](overlaps.csv) · [Snapshot at each slot selection](slots.csv).

Run from this directory as WSL root, starting at Home. The collector pins the broker and shim by SHA-256 to avoid using instruction offsets from the wrong build. Its output directories must not already exist:

```sh
root=$(mktemp -d)
for g in thin1 detail1 detail2 thin2 slots1 slots2; do
    case "$g" in
        thin*)   python3 capture.py "$root/$g" ;;
        detail*) python3 capture.py "$root/$g" --detail ;;
        slots*)  python3 capture.py "$root/$g" --detail --slots ;;
    esac
    sleep 1
done
python3 analyze.py "$root" "$root/thin1" "$root/detail1" "$root/detail2" "$root/thin2" "$root/slots1" "$root/slots2"
```
