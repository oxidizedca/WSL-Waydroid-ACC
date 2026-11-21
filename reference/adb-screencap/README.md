# Windows ADB 截图：合并 Android 端 PNG 输出（2026-11-21）

[English](#english)

[本次对照使用的 Windows 截图实现](https://github.com/KIYI671/AhabAssistantLimbusCompany/blob/V1.6.2/module/automation/input_handlers/simulator/simulator_control.py)通过 adbutils shell 请求 PNG，读完整个 stdout 后才调用 OpenCV 解码并转 RGB。在本机，Android 的 `/system/bin/screencap -p` 会边编码边把 PNG 分段写到 stdout，并非一次性输出。可参考[对应 LineageOS 实现](https://github.com/LineageOS/android_frameworks_base/blob/lineage-23.2/cmds/screencap/screencap.cpp)。改变客户端截图间隔不会消除单次请求的耗时。

- 场景一：1920×1080 的 Limbus Company 游戏内弹窗。分别用原版及临时候选执行 12 对交替 A/B，每组先各预热一次。初步排查时，原版 PNG 返回 Windows 约 1.9–2.4 s，丢弃输出约 0.48–0.52 s，未编码的原始 RGBA 返回 Windows 约 0.21–0.26 s。三者均从 Windows 发起，包含发命令、等待退出的开销，不能相减视为编码器 CPU 计时。
- 场景二：重启 Waydroid、重新冷启动游戏，在标题页改用不带绝对路径的 `screencap -p` 命令测试正式安装入口，对照仍使用绝对路径的系统原版。
- 正式计时使用adbutils 2.10.2 和本机 Windows 客户端附带的 OpenCV 4.11.0，统计从调用 shell 到获得 RGB 数组的墙钟时间，不含 0.25 s 截图间隔、图像识别、点击或失败重连。各帧检查 PNG 头尾及实际解码尺寸，存 CSV 指标，不存游戏截图。
- 另取同一份真实 PNG 原字节过一次重分块，输入与输出 SHA-256 完全相同。不同时间抓到的游戏帧并不要求哈希相同。

| 场景 | 样本/版 | 总耗时中位数，原版 → 合并 | ADB 部分中位数，原版 → 合并 | OpenCV 部分中位数，原版 → 合并 | 总耗时降幅 |
|---|---:|---:|---:|---:|---:|
| [游戏内弹窗](game.csv) | 12 | 2263.8 → 553.3 ms | 2235.2 → 524.5 ms | 29.0 → 28.6 ms | 75.6% |
| [正式部署后的标题页](deployed-title.csv) | 8 | 2506.9 → 505.2 ms | 2478.0 → 475.8 ms | 29.4 → 28.6 ms | 79.8% |

**复现分块对照。** 使用[Windows 采集脚本](../../validation/adb_capture.py)（adbutils 2.10.2、NumPy、OpenCV）。将[仅合并输出的入口](encoder/buffered-screencap)推到设备测试路径并赋予执行权限；ADB 连通后，用 `--stock /system/bin/screencap --candidate <设备测试路径> --pairs 12` 指定两端。若按脚本默认值走 PATH，将测到已安装的快速编码器，不是本页的单变量对照。

本页隔离的是输出分块的收益，不代表最终端到端耗时。基线还包括[无损快速编码](encoder/README.md)和[回环转发 NoDelay](relay/README.md)。

---

<a id="english"></a>

# Windows ADB screenshots: batching Android PNG output (2026-11-21)

The [Windows capture implementation](https://github.com/KIYI671/AhabAssistantLimbusCompany/blob/V1.6.2/module/automation/input_handlers/simulator/simulator_control.py) requests PNG via adbutils shell and reads stdout to completion before decoding and converting to RGB in OpenCV. On this device, Android's `/system/bin/screencap -p` writes successive chunks while encoding rather than writing the whole PNG at once. See the [LineageOS implementation](https://github.com/LineageOS/android_frameworks_base/blob/lineage-23.2/cmds/screencap/screencap.cpp). Changing the screenshot interval does not eliminate per-request latency.

- Scene 1: a 1920×1080 Limbus Company dialog. Stock and experimental output were warmed once each and compared in 12 alternating pairs. Initial observations: stock PNG to Windows about 1.9–2.4 s; discarding output about 0.48–0.52 s; sending unencoded RGBA about 0.21–0.26 s. All include issuing the command from Windows and waiting for exit; differences are not encoder CPU time.
- Scene 2: after restarting Waydroid and cold-launching the game, the title screen was measured with the installed wrapper selected by `screencap -p`; the control used the absolute system path.
- Timed runs used adbutils 2.10.2 and the Windows client's OpenCV 4.11.0: elapsed shell request to RGB array, excluding the 0.25 s interval, recognition, clicks and reconnection. PNG boundaries and decoded dimensions were checked for every frame. Only CSV metrics were kept.
- A real PNG replayed through the batching step retained the exact input SHA-256. Separate live captures at different times need not match.

| Scene | Samples/version | Median total: stock → buffered | Median ADB: stock → buffered | Median OpenCV: stock → buffered | Total time reduction |
|---|---:|---:|---:|---:|---:|
| [In-game dialog](game.csv) | 12 | 2263.8 → 553.3 ms | 2235.2 → 524.5 ms | 29.0 → 28.6 ms | 75.6% |
| [Title screen after deployment](deployed-title.csv) | 8 | 2506.9 → 505.2 ms | 2478.0 → 475.8 ms | 29.4 → 28.6 ms | 79.8% |

**Reproduction:** use the [Windows capture script](../../validation/adb_capture.py) (adbutils 2.10.2, NumPy and OpenCV). Push the [Buffered-output wrapper](encoder/buffered-screencap) to an executable device test path. Once connected, pass `--stock /system/bin/screencap --candidate DEVICE_TEST_PATH --pairs 12`. The default PATH now selects the fast encoder, which would not isolate output batching.

This comparison isolates batching, not final end-to-end performance. The baseline also includes the [Fast lossless encoder](encoder/README.md#english) and [NoDelay loopback forwarding](relay/README.md#english).
