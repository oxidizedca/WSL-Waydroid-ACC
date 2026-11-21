# Limbus Company：一张无损 ADB 截图的耗时去哪了？

[English](#english)

2026-11-21，Windows → 127.0.0.1:15555 → WSL → Waydroid；游戏保持前台，1920×1080。没有重启、修改游戏或客户端 或替换已安装的截图入口。Windows 使用 adbutils 2.10.2 与 客户端自带的 OpenCV 4.11.0，按实际 API 接收完整 PNG、imdecode、BGR→RGB；不计游戏识别或任务调度。这里测的是正常不透明 sRGB 帧走快速路径，不代表系统 PNG 回退或远程网络。

## 数字

两批分别 36、24 轮，各先预热两轮；每种路径合计 60 次。正常入口 `screencap -p` 的请求到 RGB **中位 239.1 ms、均值 250.5 ms、P95 308.1 ms**；PNG 中位 2.08 MB（十进制）。插入时间戳的等价流水线均值 252.0 ms（中位 244.9 ms），各段按同一张图分账，以下为均值而不是中位数相加：

| 阶段 | ms | 占比 | 边界 |
|---|---:|---:|---|
| Android 抓帧、交给编码器 | 62.2 | 24.7% | 调用平台 screencap 前至编码器读完 raw；其中约 52.0 ms 到读完帧头，10.1 ms 读余下像素、检查 alpha 并逐行做 Sub 过滤 |
| PNG 编码的剩余部分 | 18.2 | 7.2% | raw EOF 后的无损压缩、CRC 和封包，直到可写出第一字节 |
| ADB 调用和接收 | 151.3 | 60.0% | Windows 发请求至返回完整字节，扣掉上两段；含启动、调度、WSL 转发、Windows 客户端读取与关闭；**不是纯网络耗时** |
| Windows 解码到 RGB | 20.4 | 8.1% | OpenCV 解 PNG 并转通道顺序 |
| **合计** | **252.0** | **100%** | 各段底层未取整；表格有舍入差 |

另将未改的平台抓帧器与已部署的正式编码器串行运行，中间 raw 使用 memfd、PNG 使用私有 tmpfs。这时**抓帧 64.0 ms、完整编码 23.4 ms**（均值，含子进程启动和输出），避免把与 raw 读取重叠的行过滤误认为免费，也不要把这两项再加到流水线表里。

## ADB：固定开销与数据量

- 流水线中的 151.3 ms 可再拆成：**64.7 ms** 为请求至首个 4 KiB、扣掉 Android 抓帧与编码；**86.6 ms** 为首个 4 KiB 到完整返回。Android 计时全部使用同一个 monotonic 时钟；与 Windows 比较的是区间长度，不假定双方时钟同步。
- 每轮重发刚生成的**同一份 PNG**，既不抓帧也不编码。Windows 收完的均值 **152.9 ms**（中位 140.1 ms），与流水线扣除值接近。相同路径重发前 1 MiB、前 256 KiB、无输出命令均值分别为 **97.9、81.4、93.0 ms**。每次都检查了全图或对应前缀的 SHA-256。不同命令的首包和结束时机不同，不能用空命令均值做逐帧固定扣减。
- 第二批又单独标记 Windows 的 EOF 与 close。在已生成的全图上，从首个 4 KiB 到读完及观察到 EOF 的中位数 **71.3 ms**；只发 1 MiB 时 **5.9 ms**，256 KiB 时 **0.65 ms**。这是应用层时间，可能包括缓冲、分块、对象拼接和调度，**不能当成链路带宽**。收完后的 close 本身约 **10.4 ms**；本机 adbutils 的关闭实现会 sleep(0.01)。按 serial 选择设备还会查询本机 ADB server 版本。这些不该归因于 Android 编码器。单凭此表不能断言是哪个转发进程或 TCP 选项导致了余下等待。
- 同一 PNG 的重放耗时与流水线扣除值接近，因此将转发单独做了[固定字节、交换端口和正式端口对照](../relay/README.md)。该结果支持启用 NoDelay，不需要改变 Windows 解码器或减损图像质量。

## 采样与复现

[第一批原始数据](limbus.csv) · [带 EOF/close 打点的复测](limbus-detail.csv) · [Android 分段与重放](../../../validation/adb-stages.c) · [编码器打点生成器](../../../validation/adb_stage_probe.py) · [Windows 采集器](../../../validation/adb_stages.py)。

在仓库根目录用 NDK r27c 的 x86_64 Android clang 编译 adb-stages.c 为 stage（API 30）；运行 `python3 validation/adb_stage_probe.py <临时目录>/probe.c`，把 probe.c 按 build.sh 中 adb-png.c 的参数与 vendor/libdeflate-1.23 编译链接为 probe。两者安装到设备上仅供测试的可执行目录；另在设备 tmpfs 上建一个 adb shell 独占、可写的 0700 目录。Windows 运行 `python validation/adb_stages.py result.csv --bin <设备测试程序目录> --scratch <设备私有暂存目录> --cycles 24`。结束后删除两个临时目录。不要把探针替换进 /system_ext；测速期间不要更改画面或同时跑另一个截图任务。

每轮轮换正式入口、流水线和串行/重放组的顺序；重放在串行抓取后进行，同组内不同大小轮换顺序。正常动态画面的独立抓取并不宣称是同一帧。保存的只有数字，已清理设备临时 PNG、可执行文件及临时构建目录；没有部署改动。本次未测试完整任务耗时。

---

<a id="english"></a>

# Limbus Company: where does a lossless ADB screenshot spend its time?

2026-11-21, Windows → 127.0.0.1:15555 → WSL → Waydroid, game in foreground at 1920×1080. No restart or changes to the game, client or installed capture wrapper. Windows used adbutils 2.10.2 and bundled OpenCV 4.11.0 to receive the complete PNG, imdecode and convert BGR→RGB. Recognition and task scheduling were excluded. This measures the opaque sRGB fast path, not platform PNG fallback or a remote network.

## Timings

Batches of 36 and 24 cycles followed two warm-ups each, yielding 60 samples per path. Normal `screencap -p` request to RGB: **median 239.1 ms, mean 250.5 ms, P95 308.1 ms**; median PNG 2.08 decimal MB. An equivalent instrumented pipeline averaged 252.0 ms (median 244.9 ms). The table attributes each frame's time by stage and reports means, not sums of medians.

| Stage | ms | Share | Measurement interval |
|---|---:|---:|---|
| Android capture and handoff | 62.2 | 24.7% | From before platform screencap to raw input consumed: about 52.0 ms through the frame header, then 10.1 ms reading pixels, checking alpha and applying Sub per row |
| Remaining PNG encoding | 18.2 | 7.2% | Lossless compression, CRC and packaging after raw EOF until the first byte is ready to write |
| ADB request and receive | 151.3 | 60.0% | Windows request through full receive minus the two Android stages. Includes startup, scheduling, WSL forwarding, Windows reads and close; **not pure network time** |
| Windows decode to RGB | 20.4 | 8.1% | Decode PNG in OpenCV and reorder channels |
| **Total** | **252.0** | **100%** | Calculated before rounding; displayed values may not sum exactly |

In a separate serial test, the stock capture program wrote raw data to memfd and the deployed encoder wrote PNG to private tmpfs. **Capture averaged 64.0 ms; complete encoding 23.4 ms**, including child startup and output. Filtering overlaps raw input in the pipeline; it is not free. Do not add these figures to the pipeline table.

## ADB: fixed overhead and payload size

- Of the pipeline's 151.3 ms, **64.7 ms** elapsed from request to first 4 KiB after subtracting Android capture/encoding, and **86.6 ms** from that first read to complete return. Android used one monotonic clock; comparisons with Windows use interval lengths, not synchronized clocks.
- Replaying **the same PNG** without capture or encoding averaged **152.9 ms** to receive (median 140.1 ms), close to the pipeline residual. Replaying the first 1 MiB, first 256 KiB or an empty command averaged **97.9, 81.4 and 93.0 ms**, respectively. Full payloads/prefixes were hash-checked. First-byte and completion boundaries differ between commands; do not subtract the empty-command mean from every frame.
- The second batch timed Windows EOF and close separately. Median first 4 KiB to EOF: **71.3 ms** for the full PNG, **5.9 ms** for 1 MiB and **0.65 ms** for 256 KiB. These application timings include possible buffering, chunk handling, concatenation and scheduling; they are **not link bandwidth**. Close after receive took about **10.4 ms**; local adbutils calls sleep(0.01) there. Device selection by serial also queries the local ADB server version. Neither is encoder cost, and these figures do not isolate a forwarding process or TCP option.
- Replay agreed with the pipeline residual, motivating [Fixed-byte, port-swap and production-port tests](../relay/README.md#english). The evidence supports NoDelay without changing the Windows decoder or image quality.

## Sampling and reproduction

[First raw batch](limbus.csv) · [Repeat with EOF/close timestamps](limbus-detail.csv) · [Android timing and replay](../../../validation/adb-stages.c) · [Encoder instrumentation generator](../../../validation/adb_stage_probe.py) · [Windows collector](../../../validation/adb_stages.py).

At repository root, use NDK r27c x86_64 Android clang (API 30) to compile adb-stages.c as stage. Run `python3 validation/adb_stage_probe.py TMPDIR/probe.c` and build probe with vendor/libdeflate-1.23 and the adb-png.c arguments in build.sh. Install both in a test executable directory on the device; create a separate private, writable 0700 directory for adb shell on tmpfs. On Windows run `python validation/adb_stages.py result.csv --bin DEVICE_TEST_BIN --scratch PRIVATE_DEVICE_DIR --cycles 24`. Remove both temporary directories afterward. Do not replace anything in /system_ext, change scenes or run another capture job during timing.

Each cycle rotated the installed entry, pipeline and serial/replay order. Replay followed serial capture and rotated payload sizes. Live captures of a dynamic scene are not claimed to be identical. Only numbers were kept; device test images, executables and temporary builds were removed. No deployment changed and full-task time was not measured.
