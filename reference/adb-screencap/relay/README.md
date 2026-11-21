# WSL 回环 ADB 转发：固定字节对照与正式端口实测（2026-11-21）

[English](#english)

**已合入：仅给回环监听套接字增加 `NoDelay=yes`。** 不改变代理程序、截图内容或 PNG 过滤方式。Limbus Company 前台的三次交错测试均观察到改善。

## 1. 只换转发，固定所有 PNG 字节

保持游戏在前台，取一张 1920×1080 原始帧，经正式编码器得到 **2,093,574 字节**的 PNG。在独立的 Windows ADB server 上同时连到两个回环端点，下游始终是同一个已授权的 Waydroid adbd。用相同 Android 程序以相同的 256 KiB 写块重放，按对轮换先后顺序，每种负载各 36 对，另有预热。完整文件和两个前缀每次都在 Windows 核验 SHA-256。此处不重新抓帧，不解码。

| 现行 systemd → 同程序开启 NoDelay | 首个 4 KiB 中位（ms） | Windows 收完并关闭中位（ms） | 收完 P95（ms） |
|---|---:|---:|---:|
| 无输出命令 | — | 91.7 → 44.2 | 107.1 → 63.3 |
| 固定前 256 KiB | 51.0 → 41.0 | 61.7 → 51.8 | 106.5 → 70.4 |
| 固定前 1 MiB | 54.5 → 50.3 | 75.2 → 69.8 | 164.5 → 87.2 |
| 完整 PNG | 59.5 → 44.2 | 132.0 → 116.8 | 187.3 → 137.3 |

[固定字节 CSV](nodelay.csv)。作为独立对照，[另一种用户态中转](../../../validation/adb_loopback_relay.py)也比当次默认端点快（[CSV](path.csv)），但不需要为此增加 Python 常驻进程。在固定整图上，原代理开关前后首个 4 KiB 到 EOF 的中位为 64.4 → 61.6 ms；主要获益是降低短消息等待及慢请求，不是链路带宽翻倍。没有用数据包追踪把等待进一步归因于某一端的 ACK 定时器。

## 2. 实时截图：排除端口偏差

使用正式 `screencap -p`，按 adbutils 2.10.2 + 客户端自带 OpenCV 4.11 读完整图并转成 RGB。均不含游戏识别和任务调度。同一行内按对比较；实时抓取的是相邻时刻，不是同一张图。数值单位 ms。

| 关 → 开 | 对数 | 请求到 RGB 中位 | 请求到 RGB P95 |
|---|---:|---:|---:|
| [15555 → 15557](live-nodelay.csv) | 48 | 272.6 → 226.6 | 333.6 → 270.4 |
| [临时端口 15556 → 15557](ports-ab.csv) | 24 | 263.4 → 229.2 | 308.5 → 277.0 |
| [互换配置：15557 → 15556](ports-ba.csv) | 24 | 249.2 → 229.9 | 309.9 → 282.9 |

正式[启动脚本](../../../services/waydroid-acc-start)为回环监听设置 `--socket-property=NoDelay=yes`，保持同一代理和 Android ADB 认证。[重放配对工具](../../../validation/adb_relay_ab.py) · [图像对照工具](../../../validation/adb_image_ab.py)。

## 正式端口验收

`127.0.0.1:15555` 的监听报告 `NoDelay=yes`，Windows 已授权的 ADB shell 返回 UID 2000。转发只随显式启动命令建立，不随 WSL 自启。

在同一正式端口、Limbus Company 前台，按 Windows 请求到取得 1920×1080 RGB 的口径，各预热两次、记录 12 次：[部署前](installed-before.csv)中位 270.2 ms、P95 297.9 ms；[部署后](installed-after.csv)中位 233.0 ms、P95 247.5 ms。此处为分时验收，并非同帧交错 A/B；不作为其它场景的性能承诺。

---

<a id="english"></a>

# WSL loopback ADB: fixed-byte replay and production-port measurements (2026-11-21)

**Merged change: set `NoDelay=yes` on the loopback listener only.** The proxy program, screenshot contents and PNG filter are unchanged. All three interleaved live tests with Limbus Company in the foreground improved.

## 1. Change forwarding only; hold every PNG byte constant

With the game foregrounded, the production encoder produced a **2,093,574-byte** PNG from a 1920×1080 frame. A separate Windows ADB server connected to two loopback endpoints backed by the same authorized Waydroid adbd. The same Android program replayed identical 256 KiB writes, alternating order for 36 pairs per payload after warm-up. Windows verified SHA-256 on each full file and both prefixes. No new captures or decoding.

| Existing systemd proxy → same proxy with NoDelay | Median to first 4 KiB (ms) | Median through Windows receive and close (ms) | Complete receive P95 (ms) |
|---|---:|---:|---:|
| Command with no output | — | 91.7 → 44.2 | 107.1 → 63.3 |
| Fixed 256 KiB prefix | 51.0 → 41.0 | 61.7 → 51.8 | 106.5 → 70.4 |
| Fixed 1 MiB prefix | 54.5 → 50.3 | 75.2 → 69.8 | 164.5 → 87.2 |
| Entire PNG | 59.5 → 44.2 | 132.0 → 116.8 | 187.3 → 137.3 |

[Fixed-byte replay](nodelay.csv). An [Alternative userspace relay](../../../validation/adb_loopback_relay.py) also beat the default endpoint in that run ([CSV](path.csv)), but a persistent Python process is unnecessary. On full-image replay, the original proxy's median from the first 4 KiB to EOF was 64.4 → 61.6 ms. The main improvement is less waiting on short messages and slow requests, not doubled bandwidth. Packet tracing did not isolate an ACK timer.

## 2. Live screenshots: control for port choice

The production `screencap -p` and adbutils 2.10.2 plus the client's OpenCV 4.11 read the full PNG and converted it to RGB. Timings exclude recognition and task scheduling. Compare pairs within each row in ms: live frames are from adjacent instants, not identical images.

| Off → on | Pairs | Median request to RGB | P95 request to RGB |
|---|---:|---:|---:|
| [15555 → 15557](live-nodelay.csv) | 48 | 272.6 → 226.6 | 333.6 → 270.4 |
| [Temporary ports 15556 → 15557](ports-ab.csv) | 24 | 263.4 → 229.2 | 308.5 → 277.0 |
| [Settings swapped: 15557 → 15556](ports-ba.csv) | 24 | 249.2 → 229.9 | 309.9 → 282.9 |

The [Startup script](../../../services/waydroid-acc-start) sets `--socket-property=NoDelay=yes` while retaining the proxy and ADB authentication. [Paired replay tool](../../../validation/adb_relay_ab.py) · [Image comparison tool](../../../validation/adb_image_ab.py).

## Production-port verification

The listener on `127.0.0.1:15555` reported `NoDelay=yes`; the authorized Windows ADB shell returned UID 2000. Forwarding starts with the explicit launcher, not WSL boot.

Same production port, Limbus Company foreground, Windows request to 1920×1080 RGB; two warm-ups then 12 samples each. [Before deployment](installed-before.csv): median 270.2 ms, P95 297.9 ms. [After deployment](installed-after.csv): median 233.0 ms, P95 247.5 ms. These separate time windows are not an interleaved same-frame comparison or a guarantee for other scenes.
