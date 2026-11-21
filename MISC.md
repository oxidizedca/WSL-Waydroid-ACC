# 补充修改与测试记录

[English](#english)

## 1. Windows ADB 转发

**修改方式。** 在 WSL 执行 `sudo waydroid-acc-start`。[启动脚本](services/waydroid-acc-start)读取本次容器的 eth0 IPv4，用临时 systemd 套接字将 WSL 的 `127.0.0.1:15555` 转发到容器的 TCP 5555。套接字设置 `NoDelay=yes`，仅监听回环，不对局域网开放。脚本检查 Android 的 ADB 密钥认证；只在开发者选项已开启时恢复 USB 调试，不预授权 Windows 密钥。转发随容器停止，不随 WSL 自动启动。

Windows 端执行以下命令，首次连接需在 Android 确认授权：

```powershell
adb connect 127.0.0.1:15555
adb -s 127.0.0.1:15555 shell id -u
```

**测试结果。** 正式端口报告 `NoDelay=yes`，已授权的 shell 返回 UID 2000。用同一份 2,093,574 字节的 PNG 重放，开关前后 Windows 接收耗时中位数为 **132 → 117 ms**，P95 为 **187 → 137 ms**；每次收到的字节均通过 SHA-256 核对。将设置在两个临时端口间对调，实时截图的耗时改善仍随设置而非端口变化。[端口对照与原始数据](reference/adb-screencap/relay/README.md)

## 2. 容器虚拟多点触屏

**修改方式。** 按[构建部署说明](README.md#43-编译离屏验证与部署)部署后，启动命令在 Android 完成引导后拉起 [waydroid-acc-touch.service](services/systemd/waydroid-acc-touch.service)。[虚拟设备程序](src/virtual-touch.c)通过宿主 uinput 创建 Type B 直接触屏；[节点管理脚本](services/waydroid-acc-touch-node)根据 sysfs 实例定位这一次的 event 节点，以 `root:input`、`0660` 将该字符设备发布到容器。只发布该节点，不映射整个 `/dev/input` 或 `/dev/uinput`。停用时核验并清理自己的节点，原 Wayland 鼠标和键盘保留。默认配置对应 1920×1080；基线从启动参数读取宽高，用于设置虚拟触屏的 X/Y 轴范围。

**测试结果。** Windows 经普通 ADB shell 运行原版 minitouch，可自动选中触屏。握手返回 `v 1` 和 `^ 10 1919 1079 255`（10 个触点，X/Y 最大值 1919/1079，最大压力 255）。点击、拖动及原鼠标操作在原型阶段通过；正式服务单独停启后，节点清理与重建通过。

## 3. Android 无损 PNG 截图

**修改方式。** 默认部署（`sudo bash deploy.sh apply`）即安装 [screencap 入口](services/screencap)和[快速 PNG 编码器](src/adb-png.c)。Windows 仍调用 `adb -s 127.0.0.1:15555 shell screencap -p`。Android 原版程序负责权限检查与抓帧；对 1920×1080 或 1080×1920、不透明的 sRGB RGBA8888 帧，执行 PNG Sub 过滤、libdeflate level 1 无损压缩，并按 256 KiB 分块写出。未支持的尺寸、格式、色域或透明度在写出任何 PNG 字节前回退到平台编码。需要对照时直接调用 `/system/bin/screencap -p`，不用切换整套部署。

**测试结果。** 同一真实帧分别走参考编码器和快速编码器，解码后的 **8,294,400 个 RGBA 字节**与原始帧一致，色彩元数据一致，PNG 块 CRC 校验通过。下表均从 Windows 发起请求计到取得 RGB；各行是不同轮次，不能跨行相减或累加收益。

| 本机 1920×1080 场景 | 每种配置次数 | 耗时中位数 |
|---|---:|---:|
| 游戏内弹窗：平台 PNG 直接输出 → 合并输出块 | 12 | 2264 → 553 ms |
| 游戏标题页：分块输出 → 快速 PNG 编码 | 16 | 537 → 260 ms |
| 游戏前台，正式端口：快速 PNG 不变，开启 NoDelay | 12 | 270 → 233 ms（P95：298 → 248 ms） |

第一项是[分块传输测试](reference/adb-screencap/README.md)，第二项见[同帧校验与编码器部署记录](reference/adb-screencap/encoder/README.md)，第三项为[正式端口部署验收](reference/adb-screencap/relay/README.md)。另有[抓帧、编码、ADB 接收及 Windows 解码的分段记录](reference/adb-screencap/stages/README.md)。

## 4. Weston 窗口与输入

**修改方式。** [启动入口](services/waydroid-acc-start)在停止容器前校验参数，向宿主 Weston 和虚拟触屏传递同一组尺寸。[Wayland 后端](weston/libweston/backend-wayland/wayland.c)在 fix/ratio 下分离 Android 输出模式和外窗内容区。fix 发送相同的最小/最大窗口几何并抑制拖边请求；ratio 在配置外窗大小时不改变输出模式。[GL 渲染器](weston/libweston/renderer-gl/gl-renderer.c)与输入映射使用[同一个等比矩形](weston/libweston/acc-fit.h)，缩放区域以外清成黑色。默认模式不套用缩放策略。

**测试结果（2026-11-21）。** 新构建通过共享纹理离屏检查。在独立 800×450 Weston 窗口用 Windows 实际拖同一条边：fix 的窗口尺寸不变；ratio 的外框变窄，测试圆保持圆形，上下出现黑边。正式 Waydroid 随后用 `sudo waydroid-acc-start -w ratio -r 1280x720` 重启，`wm size` 在拖边前后都报告 `Physical size: 1280x720`。Android 识别的虚拟触屏坐标为 0–1279、0–719，minitouch 握手为 `^ 10 1279 719 255`；从锁屏实际送入上滑到达桌面。在缩窄的 Windows 窗口用鼠标点击桌面的设置图标，打开系统设置，锁屏、桌面、设置三处的内容比例均经 Windows 侧截图核对。未测试多显示器 DPI 切换或量取缩放时的游戏帧率。
<a id="digitalis-arm64"></a>

## 5. 可选的 ARM64 应用兼容方案：Digitalis 与 MuMu 单库

Digitalis 为 x86_64 Waydroid 提供 ARM64 应用转译。使用 **Waydroid overlay** 安装；它不是 KernelSU 模块，也不随仓库根目录的 `build.sh` 或 `deploy.sh` 构建、部署。

**使用已验证的构建包。** 仓库已收录[实机同版的 Digitalis 归档和独立部署脚本](system-optimize/digitails/README.md)。直接部署无需编译，也不需要访问本机的 AOSP 工作区。

**从源码构建。** 本机镜像为 Android 16 QPR2（`BP4A.251205.006`），使用 Digitalis 的 `android16` 分支，转译器提交 `8a2c30cb5383f89f8175eacc06ab1065ab7c5b6f`。在已经同步好的本机 WSL 工作区，以普通用户运行：

```bash
bash ~/digitails/tools/build-bridge
cd ~/digitails/aosp/digitalis/dist/digitalis-prebuilts
sha256sum -c SHA256SUMS
```

该命令使用本机构建工作区的脚本（不随本仓库提供），构建并校验 74 项转译器、代理库、guest 库及配置，不构建整套系统镜像。新建工作区请按 [Digitalis 上游集成说明](https://github.com/DigitalisX64/digitalis/blob/android16/docs/integrating-digitalis.md) 准备匹配的 AOSP 源码。仓库内的独立部署脚本负责备份、写入 overlay，并核对 bridge 属性、Zygote 映射及 `arm64_dyn`、`arm64_exe` 的实际解释器；不能仅凭 Android 属性判断转译生效。

**可选的 MuMu 单库。** 可以从自己安装的 MuMu 模拟器提取 ARM64 guest 版 `libnativebridge.so`，用于改善部分应用兼容性。本机来源为 MuMu Android 15（`V417IR`）；在 Windows 上连接对应模拟器后只提取这一份：

```powershell
adb connect 127.0.0.1:16384
adb -s 127.0.0.1:16384 pull /system/lib64/houdini/arm64/libnativebridge.so ./libnativebridge.so
Get-FileHash ./libnativebridge.so -Algorithm SHA256
```

本机已验证样本的 SHA-256 是 `f72a269d577ed39ec409312675b41e51398de7f70c4157aaf0328f90035123c6`。在 WSL 中对应的安装位置是 `/var/lib/waydroid/overlay/system/lib64/arm64/libnativebridge.so`（容器内为 `/system/lib64/arm64/libnativebridge.so`）。不要替换 x86_64 宿主或 ART APEX 的同名库，也不需要复制 Houdini。仓库不分发 MuMu 文件；这里记录的是已测组合，不代表该库对所有应用都有收益。

**兼容性验证（2026-11-21）。** 在上述 overlay 和 MuMu 单库组合下，明日方舟、Limbus Company 均可冷启动到标题画面。分别从两款游戏进程的挂载视图核对：74 项 Digitalis 文件与构建包一致，ARM64 guest 的 MuMu 单库与上述 SHA-256 一致。独立 ARM64 动态 ELF 的 Bionic/pthread 检查也通过。未验证登录及后续玩法，不能据此将兼容性改善单独归因于 MuMu 单库。

---

<a id="english"></a>

# Additional changes and test results

## 1. Windows ADB forwarding

**How it works.** Run `sudo waydroid-acc-start` in WSL. The [launcher](services/waydroid-acc-start) reads the current container's eth0 IPv4 address and uses a transient systemd socket to forward WSL's `127.0.0.1:15555` to TCP port 5555 in the container. The socket uses `NoDelay=yes` and listens only on loopback, not on the LAN. The script checks that Android uses ADB key authentication; it restores USB debugging only if Developer options are already enabled and does not preauthorize the Windows key. Forwarding stops with the container and does not start when WSL boots.

Run the following on Windows. On the first connection, approve the authorization prompt in Android:

```powershell
adb connect 127.0.0.1:15555
adb -s 127.0.0.1:15555 shell id -u
```

**Test results.** The deployed port reported `NoDelay=yes` and an authorized shell returned UID 2000. Replaying the same 2,093,574-byte PNG before and after enabling the option reduced median Windows receive time from **132 to 117 ms** and P95 from **187 to 137 ms**. The received bytes passed SHA-256 verification on every run. When the settings were swapped between two temporary ports, the live screenshot improvement followed the setting, not the port. [Port comparison and raw data](reference/adb-screencap/relay/README.md#english)

## 2. Virtual multi-touch screen for the container

**How it works.** After [building and deploying](README.en.md#43-build-offscreen-validation-and-deployment), the launcher starts [waydroid-acc-touch.service](services/systemd/waydroid-acc-touch.service) once Android has booted. The [virtual device program](src/virtual-touch.c) creates a Type B direct touchscreen through host uinput. The [node management script](services/waydroid-acc-touch-node) identifies this instance's event node through sysfs and exposes that character device in the container with owner/group `root:input` and mode `0660`. It does not map all of `/dev/input` or expose `/dev/uinput`. On shutdown the service verifies and removes only its own node. The original Wayland mouse and keyboard remain available. The default is 1920×1080; the launcher passes the selected width and height to set the touchscreen's X/Y axis ranges.

**Test results.** Stock minitouch, run from Windows via an ordinary ADB shell, selected the touchscreen automatically. Its handshake returned `v 1` and `^ 10 1919 1079 255` (10 contacts, maximum X/Y of 1919/1079, maximum pressure 255). Taps, drags and the original mouse worked in prototype testing. Stopping and starting the deployed service independently also removed and recreated the node correctly.

## 3. Lossless Android PNG screenshots

**How it works.** The default deployment (`sudo bash deploy.sh apply`) installs the [screencap entry point](services/screencap) and [fast PNG encoder](src/adb-png.c). Windows continues to call `adb -s 127.0.0.1:15555 shell screencap -p`. Android's stock program handles permissions and frame capture. For opaque sRGB RGBA8888 frames at 1920×1080 or 1080×1920, the fast path applies the PNG Sub filter, compresses losslessly with libdeflate level 1, and writes 256 KiB chunks. Unsupported dimensions, formats, color spaces or alpha values fall back to platform encoding before any PNG bytes are written. For a comparison, invoke `/system/bin/screencap -p` directly; there is no need to switch deployments.

**Test results.** The reference and fast encoders processed the same captured frame. All **8,294,400 decoded RGBA bytes** matched the source, color metadata matched, and PNG chunk CRCs passed. The table measures from the Windows request until RGB is available. Each row is a separate experiment: do not subtract values across rows or add the gains together.

| Local 1920×1080 scene | Runs per configuration | Median elapsed time |
|---|---:|---:|
| In-game dialog: direct platform PNG output → coalesced writes | 12 | 2264 → 553 ms |
| Game title screen: chunked output → fast PNG encoder | 16 | 537 → 260 ms |
| Game in foreground, deployed port: unchanged fast encoder, NoDelay enabled | 12 | 270 → 233 ms (P95: 298 → 248 ms) |

See the [chunked transfer test](reference/adb-screencap/README.md#english), [same-frame validation and encoder deployment](reference/adb-screencap/encoder/README.md#english), and [deployed port verification](reference/adb-screencap/relay/README.md#english) for the three rows respectively. A [stage-by-stage breakdown](reference/adb-screencap/stages/README.md#english) covers capture, encoding, ADB reception and Windows decoding.

<a id="weston-window-and-input-en"></a>

## 4. Weston window and input

**How it works.** The [launcher](services/waydroid-acc-start) validates arguments before stopping the container and passes the same dimensions to host Weston and the virtual touchscreen. In fix and ratio modes, the [Wayland backend](weston/libweston/backend-wayland/wayland.c) separates the Android output mode from the outer window's content area. In fix mode it sends identical minimum and maximum window geometry and suppresses edge-resize requests. In ratio mode it leaves the output mode unchanged when the outer window is configured. The [GL renderer](weston/libweston/renderer-gl/gl-renderer.c) and input mapping use [the same aspect-preserving rectangle](weston/libweston/acc-fit.h); the area outside is cleared to black. The default mode does not apply this scaling policy.

**Test results (2026-11-21).** The new build passed the offscreen shared-texture check. Dragging the same edge of a standalone 800×450 Weston window on Windows left its size unchanged in fix mode. In ratio mode the outer window narrowed, the test circle remained round, and black bars appeared above and below it. Waydroid was then restarted with `sudo waydroid-acc-start -w ratio -r 1280x720`. Before and after resizing, `wm size` reported `Physical size: 1280x720`. Android recognized touchscreen axes of 0–1279 and 0–719; minitouch reported `^ 10 1279 719 255`. An injected upward swipe unlocked the screen and reached Home. A mouse click on the Settings icon in the narrowed Windows window opened Settings. Windows screenshots confirmed the content aspect ratio on the lock screen, Home and Settings. Multi-monitor DPI changes and game frame rates while scaling were not tested.

<a id="digitalis-arm64-en"></a>

## 5. Optional ARM64 app compatibility: Digitalis and one MuMu library

Digitalis translates ARM64 apps for x86_64 Waydroid. It is installed through the **Waydroid overlay**, not as a KernelSU module. The repository's top-level `build.sh` and `deploy.sh` do not build or deploy it.

**Use the verified build.** The repository includes [the Digitalis archive used on the test system and its standalone deployment script](system-optimize/digitails/README.md#english). Direct deployment does not require compilation or access to the local AOSP workspace.

**Build from source.** The tested image is Android 16 QPR2 (`BP4A.251205.006`). The build used Digitalis's `android16` branch at translator commit `8a2c30cb5383f89f8175eacc06ab1065ab7c5b6f`. In the already-synced local WSL workspace, run as an unprivileged user:

```bash
bash ~/digitails/tools/build-bridge
cd ~/digitails/aosp/digitalis/dist/digitalis-prebuilts
sha256sum -c SHA256SUMS
```

This command uses a script from the local build workspace (not included in this repository). It builds and verifies the 74 translator, proxy library, guest library and configuration files, not an entire system image. For a new workspace, follow the [upstream Digitalis integration instructions](https://github.com/DigitalisX64/digitalis/blob/android16/docs/integrating-digitalis.md) to prepare matching AOSP sources. The standalone deployment script backs up the current installation, writes the overlay, and verifies the bridge property, Zygote mappings and actual interpreters for `arm64_dyn` and `arm64_exe`. Android properties alone do not prove translation is active.

**Optional MuMu library.** You can extract the ARM64 guest `libnativebridge.so` from your own MuMu installation for possible compatibility improvements in some apps. The tested source was MuMu Android 15 (`V417IR`). Connect to the corresponding emulator on Windows and extract only this file:

```powershell
adb connect 127.0.0.1:16384
adb -s 127.0.0.1:16384 pull /system/lib64/houdini/arm64/libnativebridge.so ./libnativebridge.so
Get-FileHash ./libnativebridge.so -Algorithm SHA256
```

The tested file's SHA-256 was `f72a269d577ed39ec409312675b41e51398de7f70c4157aaf0328f90035123c6`. Its WSL install path is `/var/lib/waydroid/overlay/system/lib64/arm64/libnativebridge.so` (inside the container: `/system/lib64/arm64/libnativebridge.so`). Do not replace the x86_64 host or ART APEX library of the same name. There is no need to copy Houdini. This repository does not distribute MuMu files; the tested combination is not a claim that the library improves every app.

**Compatibility verification (2026-11-21).** With the overlay and single MuMu library described above, Arknights and Limbus Company both cold-started to their title screens. In each game's process mount view, all 74 Digitalis files matched the archive and the ARM64 guest MuMu library matched the SHA-256 above. A standalone ARM64 dynamic ELF also passed the Bionic/pthread check. Login and subsequent gameplay were not tested, and these results do not isolate the contribution of the MuMu library.
