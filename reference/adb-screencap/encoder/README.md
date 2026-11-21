# Android PNG 编码：同帧验证与 Windows 端耗时（2026-11-21）

[English](#english)

**对照是仅使用 256 KiB 分块输出，不是最初的无缓冲 screencap。** 客户端代码、图像识别、分辨率和调试权限均未改。Windows 使用 adbutils 2.10.2、客户端附带的 OpenCV 4.11.0，复现 shell → 完整 PNG → imdecode → RGB。每种先预热一次，之后按对交替顺序。表格是单次截图的中位数，不含截图间隔、任务调度或重连；不同运行的动画内容不可直接当作同一帧。

| 实机场景 | 每版次数 | 仅合并输出 → 快速编码，总耗时 | PNG 大小中位数（十进制 MB） |
|---|---:|---:|---:|
| [部署前标题页](lib1-title.csv) | 16 | 604 → 357 ms | 2.44 → 2.86 |
| [部署后标题页](deployed-title.csv) | 16 | 537 → 260 ms | 1.80 → 2.06 |
| [部署后桌面](deployed-desktop.csv) | 10 | 306 → 162 ms | 0.58 → 1.03 |

## 为什么选这条路径

将一份实际游戏原始帧分别喂给 [Android Bitmap PNG 编码器](platform-reference.c)、系统 zlib level 1、libdeflate level 1 和 level 3。每项预热一次、计 8 次，含 lxc-attach 和进程启动、从同一文件读帧、输出至 /dev/null，不含抓屏、ADB 和 Windows 解码。[编码原始样本](encode-only.csv) 的中位数分别是 **454、83、54、77 ms**。只用系统 zlib 也有提升，但[标题页端到端](title.csv) 仍需 417 ms；最终选 libdeflate level 1。PNG 占用的字节通常增多，慢速网络不一定得到本机回环转发的收益。

[正式实现](../../../src/adb-png.c) 只对特定 raw 头和全不透明像素启用；不依赖 调用方的进程名，不读取 Weston 图层，不跳过 Android 的截图权限。对每行做可逆的 PNG Sub 过滤并压缩，保留 RGBA 四通道及 sRGB/sBIT。整个编码确认成功后才开始向 stdout 写 PNG，不在写了一半时追加回退图片。每次截图分配约两个帧大小的临时缓冲（另有压缩器工作区），进程退出即释放。

## 正确性和边界

- 对同一帧，Windows 完整解码不同编码器的 PNG，比较全部 **8,294,400 个 RGBA 字节**与原始帧一致，且 IHDR、sRGB、sBIT 内容一致，所有块 CRC 通过。正式构建与通过验证的 level 1 编码对该帧的输出也逐字节相同。这里的 Android 参考输出是对同一份 raw 使用平台 Bitmap PNG 编码；不是比较两个不同时刻截的屏。
- 用非目标尺寸、非目标格式、P3 标志和带透明度的样本验证：返回 75 且 stdout 为空；模拟不支持的 raw 输入经过正式入口逻辑会走系统 PNG。截断的合法格式输入返回错误，不生成残缺图片。另对 1080×1920 的不透明合成渐变帧验证了正式二进制的纵向编码，解码后逐字节一致。未在真实 HDR 屏幕上验证。
- 测试 raw 和 PNG 已清除；记录只留代码和计数。仅测单次截图，不含完整游戏任务。
- 复测：用 Windows adb push 将[仅合并输出的对照入口](buffered-screencap)放入仅测试用的设备路径，chmod 755，然后运行 [adb_capture.py](../../../validation/adb_capture.py) 并传 `--stock 设备上的旧入口 --stock-label platform_buffered --candidate screencap --candidate-label fast_png --pairs 16`。抓图权限和实际部署说明见 [补充修改与测试记录](../../../MISC.md)。[LineageOS 原版 raw 和 PNG 保存路径](https://github.com/LineageOS/android_frameworks_base/blob/lineage-23.2/cmds/screencap/screencap.cpp)。

---

<a id="english"></a>

# Android PNG encoding: same-frame validation and Windows timings (2026-11-21)

**The control used 256 KiB buffered output, not the original unbuffered screencap.** Client code, image recognition, resolution and debugging permissions stayed the same. Windows used adbutils 2.10.2 and the client's OpenCV 4.11.0: shell request → complete PNG → imdecode → RGB. Each variant had one warm-up; timed pairs alternated order. Medians are for a single screenshot, excluding capture interval, task scheduling and reconnection. Animated frames from different runs are not identical inputs.

| On-device scene | Samples/version | End-to-end: buffered output → fast encoder | Median PNG size (decimal MB) |
|---|---:|---:|---:|
| [Title before deployment](lib1-title.csv) | 16 | 604 → 357 ms | 2.44 → 2.86 |
| [Title after deployment](deployed-title.csv) | 16 | 537 → 260 ms | 1.80 → 2.06 |
| [Home after deployment](deployed-desktop.csv) | 10 | 306 → 162 ms | 0.58 → 1.03 |

## Why this encoder

The same raw game frame was passed to the [Android Bitmap PNG encoder](platform-reference.c), system zlib level 1, and libdeflate levels 1 and 3. One warm-up and 8 timed runs each included lxc-attach, process startup, reading the same file and writing to /dev/null, but not capture, ADB or Windows decoding. The respective medians were **454, 83, 54 and 77 ms** ([Raw encoding samples](encode-only.csv)). System zlib improved encoding but still took 417 ms in the [end-to-end title-screen test](title.csv). The selected encoder uses libdeflate level 1. Its PNGs are usually larger; slow networks may not benefit as the local loopback did.

The [production encoder](../../../src/adb-png.c) only accepts specific raw headers and fully opaque pixels. It does not inspect the caller's process name or Weston layers or bypass Android capture permissions. Each row gets a reversible Sub filter; RGBA, sRGB and sBIT are preserved. No PNG bytes reach stdout until encoding succeeds, so no fallback image is appended to a partial one. Temporary buffers total about two frames plus compressor workspace, released at process exit.

## Correctness and limits

- Windows decoded PNGs from different encoders for the same frame. All **8,294,400 RGBA bytes** matched raw input; IHDR, sRGB and sBIT matched and every chunk CRC passed. The deployed and validated level-1 encoders also produced byte-identical PNGs for this input. The Android reference used platform Bitmap PNG on the same raw frame, not a second screenshot.
- Unsupported sizes, formats, P3 flags and transparency returned 75 with empty stdout. An unsupported raw fixture routed through the wrapper used platform PNG; truncated input with a supported header failed without emitting a partial image. The deployed portrait encoder reproduced every byte of an opaque synthetic 1080×1920 gradient. A real HDR display was not tested.
- Only code and measurements were kept, not test raw or PNG images. These are per-request results, not whole-game-task timings.
- To reproduce, use Windows adb push to place the [Buffered-output control wrapper](buffered-screencap) in a test-only device path, chmod 755, then run [adb_capture.py](../../../validation/adb_capture.py) with `--stock DEVICE_CONTROL_PATH --stock-label platform_buffered --candidate screencap --candidate-label fast_png --pairs 16`. See [Additional changes and tests](../../../MISC.md#english) for permissions and deployment, and [LineageOS raw and PNG output](https://github.com/LineageOS/android_frameworks_base/blob/lineage-23.2/cmds/screencap/screencap.cpp) for platform behavior.
