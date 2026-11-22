# 补充修改与测试记录

[English](#english)

## 1. Windows ADB 转发

**实现方式。** Windows 的 ADB 客户端始终连接 `127.0.0.1:15555`，不需要知道容器 IP。转发分两步：

1. **确定目标。** [启动时的 ADB 配置](../services/waydroid-acc-start)通过 `lxc-attach` 在本次容器内读取 `eth0` 的 IPv4。它要求 `ro.adb.secure=1`；如果镜像在启动时清除了 USB 调试开关，只有开发者选项仍开启才恢复该开关。转发前还会从 WSL 探测容器的 TCP 5555 是否可达。
2. **转发字节，不代替认证。** 在 WSL 中用 `systemd-run` 创建临时的 `waydroid-adb-local.socket`，仅监听回环地址；有连接时由 `systemd-socket-proxyd` 将字节流转发到本次的 `容器IP:5555`。`NoDelay=yes` 避免监听端接受的 TCP 连接因小包合并而增加延迟。Windows 密钥仍由 Android 授权。套接字和代理都随容器停止，不随 WSL 自启；转发失败也不阻断 Waydroid 启动。

Windows 端执行以下命令，首次连接需在 Android 确认授权：

```powershell
adb connect 127.0.0.1:15555
adb -s 127.0.0.1:15555 shell id -u
```

**测试结果。** 正式端口报告 `NoDelay=yes`，已授权的 shell 返回 UID 2000。用同一份 2,093,574 字节的 PNG 重放，开关前后 Windows 接收耗时中位数为 **132 → 117 ms**，P95 为 **187 → 137 ms**；每次收到的字节均通过 SHA-256 核对。将设置在两个临时端口间对调，实时截图的耗时改善仍随设置而非端口变化。

## 2. 容器虚拟多点触屏

**实现方式。** 触屏由 WSL 内核创建；Android 只看到一个新的输入设备，不需要访问宿主的 `/dev/uinput`。

1. **创建设备。** Android 引导完成后，[触屏服务](../services/systemd/waydroid-acc-touch.service)加载 `evdev`、`uinput`，运行[虚拟触屏程序](../src/virtual-touch.c)。程序通过 uinput 注册带 `INPUT_PROP_DIRECT` 的 Type B 多点触屏：10 个触点槽，用 tracking ID 区分手指，压力范围 0–255。X/Y 上限是启动分辨率的宽/高各减一。进程持有 uinput 文件描述符，让设备在服务运行期间保持存在。
2. **只发布这个节点。** 程序用 `UI_GET_SYSNAME` 取得本次设备的 `inputN`。[节点管理程序](../services/waydroid-acc-touch-node)从 sysfs 读取对应 `eventN` 的主、次设备号，在容器的 `/dev` 内先创建同设备号的字符节点、设为 `root:input 0660`，再硬链接到 `/dev/input/eventN`。这样 Android 的 EventHub 在发现节点时就能打开它；不会覆盖其他节点或暴露整个宿主输入目录。
3. **输入与清理。** minitouch 写入该 event 节点，Android 按直接触摸处理；原有 Wayland 鼠标和键盘仍走各自的路径。服务停止时，管理程序核对它记录的设备号和 inode，只删除这次发布的节点。

**测试结果。** Windows 经普通 ADB shell 运行原版 minitouch，可自动选中触屏。握手返回 `v 1` 和 `^ 10 1919 1079 255`（10 个触点，X/Y 最大值 1919/1079，最大压力 255）。点击、拖动及原鼠标操作在原型阶段通过；正式服务单独停启后，节点清理与重建通过。

## 3. Android 无损 PNG 截图

**实现方式。** 改的是抓帧之后的编码和输出，不改 Android 的截屏权限、显示选择或合成。[部署脚本](../deploy/wsl-waydroid-acc/deploy.sh)把[命令入口](../services/screencap)装到 `/system_ext/bin/screencap`，通过 PATH 接管不带绝对路径的调用，保留原版 `/system/bin/screencap`。

1. **抓帧。** 仅当参数恰好为 `-p` 时，入口才调用不带参数的原版 screencap，把原始帧而非 PNG 通过管道交给[快速编码器](../src/adb-png.c)。其他参数原样传给原程序。
2. **无损编码。** 编码器读取帧头，仅接受 1920×1080 或 1080×1920 的 sRGB RGBA8888，并逐行检查所有像素的 alpha 都是 255。每行用 PNG Sub 记录与左侧像素同一通道的差值，再用 libdeflate level 1 压缩；生成带 sRGB 信息和块 CRC 的 RGBA PNG，每次最多写出 256 KiB。Windows 仍可用原来的 `adb shell screencap -p`。
3. **回退。** 不支持的帧在写出任何 PNG 字节前返回状态码 75。入口随后用平台的 `/system/bin/screencap -p` **重新抓一帧**，用 `toybox dd` 将输出合并为 256 KiB 块；不是给前一帧拼接另一份 PNG。直接调用平台程序的绝对路径即可绕过这两项优化。

**测试结果。** 同一真实帧分别走参考编码器和快速编码器，解码后的 **8,294,400 个 RGBA 字节**与原始帧一致，色彩元数据一致，PNG 块 CRC 校验通过。下表均从 Windows 发起请求计到取得 RGB；各行是不同轮次，不能跨行相减或累加收益。

| 本机 1920×1080 场景 | 每种配置次数 | 耗时中位数 |
|---|---:|---:|
| 游戏内弹窗：平台 PNG 直接输出 → 合并输出块 | 12 | 2264 → 553 ms |
| 游戏标题页：分块输出 → 快速 PNG 编码 | 16 | 537 → 260 ms |
| 游戏前台，正式端口：快速 PNG 不变，开启 NoDelay | 12 | 270 → 233 ms（P95：298 → 248 ms） |


## 4. Weston 窗口与输入

**实现方式。** [启动入口](../services/waydroid-acc-start)负责校验参数，并把模式、宽高和鼠标模式写入 `/run/waydroid-acc-window.env`。真正的窗口约束、缩放和手势转换发生在嵌套 Weston 内：

- **窗口与画面。** 默认模式保留可变窗口。`-w fix` 让 [Wayland 后端](../weston/libweston/backend-wayland/wayland.c)向外层 xdg-shell 请求相同的最小和最大窗口尺寸，并抑制拖边请求；`-w ratio` 允许外窗改变大小，但保持 Android 输出模式不变。[GL 渲染器](../weston/libweston/renderer-gl/gl-renderer.c)用[等比矩形](../weston/libweston/acc-fit.h)将画面居中绘制，剩余区域清黑。输入使用同一矩形反算 Android 坐标，因此缩放后点击位置仍与内容对齐。
- **鼠标与触摸。** 默认 `-m pointer` 保留指针。`-m touch` 在后端把左键按下、拖动和松开转换为一个触点的 down、motion 和 up，不向 Android 发送鼠标悬停或滚轮。按住 Ctrl 和左键时先不落指；拖动达到 4 个输出坐标单位后才发送双指事件。若第一指在 `(x,y)`，第二指就在 `(宽−1−x, 高−1−y)`，所以对称中心是**画面中心**，不是按下位置。结束时，若默认抓取中只有这两个合成触点，先取消客户端手势，再释放 Weston 内部触点，避免把抬起误报为点击。这条输入路径与第 2 节的 uinput 设备彼此独立。

**测试结果（2026-11-21）。** 新构建通过共享纹理离屏检查。在独立 800×450 Weston 窗口用 Windows 实际拖同一条边：fix 的窗口尺寸不变；ratio 的外框变窄，测试圆保持圆形，上下出现黑边。正式 Waydroid 随后用 `sudo waydroid-acc-start -w ratio -r 1280x720` 重启，`wm size` 在拖边前后都报告 `Physical size: 1280x720`。Android 识别的虚拟触屏坐标为 0–1279、0–719，minitouch 握手为 `^ 10 1279 719 255`；从锁屏实际送入上滑到达桌面。在缩窄的 Windows 窗口用鼠标点击桌面的设置图标，打开系统设置，锁屏、桌面、设置三处的内容比例均经 Windows 侧截图核对。未测试多显示器 DPI 切换或量取缩放时的游戏帧率。
<a id="digitalis-arm64"></a>

## 5. 可选的 ARM64 应用兼容方案：Digitalis 与 MuMu 单库

**实现方式。** Digitalis 在 x86_64 Waydroid 中执行 ARM64 代码。它用 [Waydroid overlay 部署](../deploy/digitails/deploy.sh)，不重打 system 镜像，不使用 KernelSU，也不随图形栈安装。

- **应用内的 ARM64 库：** 安装器备份将被改写的 overlay 文件，停容器，将 74 项转译器、代理库、客体库和配置按包内路径写入 overlay。在 `vendor/build.prop` 中将 `ro.dalvik.vm.native.bridge` 设为 `libberberis_arm64.so`，让重启后的 Android 运行时选用 Digitalis，而不是原来的 NDK bridge。
- **独立的 ARM64 程序：** 安装器用空操作配置覆盖旧的 `ndk_translation.rc`，避免两个转译器争用同一批规则；重启后，[规则管理程序](../deploy/digitails/binfmt.py)只把 `arm64_dyn` 和 `arm64_exe` 两条 binfmt_misc 规则指向 Digitalis 的 ARM64 runner，不清空其他规则。下文的 MuMu 单库属于 ARM64 客体侧，不是用来替换上述宿主 bridge 的。

**使用已验证的构建包。** 仓库已收录[实机同版的 Digitalis 归档和独立部署脚本](../deploy/digitails/)。直接部署无需编译，也不需要访问本机的 AOSP 工作区。

**从源码构建。** 本机镜像为 Android 16 QPR2（`BP4A.251205.006`），使用 Digitalis 的 `android16` 分支，转译器提交 `8a2c30cb5383f89f8175eacc06ab1065ab7c5b6f`。要从源码重建当前版本（**CC1 + 短叶函数**），先按 [Digitalis 目录说明](../system-optimize/digitails/README.md) 顺序应用两份补丁，再在已同步的本机 WSL 工作区以普通用户运行：

```bash
bash ~/digitails/tools/build-bridge
cd ~/digitails/aosp/digitalis/dist/digitalis-prebuilts
sha256sum -c SHA256SUMS
```

该命令使用本机构建工作区的脚本（不随本仓库提供），构建并校验 74 项转译器、代理库、guest 库及配置，不构建整套系统镜像。新建工作区请按 [Digitalis 上游集成说明](https://github.com/DigitalisX64/digitalis/blob/android16/docs/integrating-digitalis.md) 准备匹配的 AOSP 源码。

**可选的 MuMu 单库。** 可以从自己安装的 MuMu 模拟器提取 ARM64 guest 版 `libnativebridge.so`，用于改善部分应用兼容性。本机来源为 MuMu Android 15（`V417IR`）；在 Windows 上连接对应模拟器后只提取这一份：

```powershell
adb connect 127.0.0.1:16384
adb -s 127.0.0.1:16384 pull /system/lib64/houdini/arm64/libnativebridge.so ./libnativebridge.so
Get-FileHash ./libnativebridge.so -Algorithm SHA256
```

本机已验证样本的 SHA-256 是 `f72a269d577ed39ec409312675b41e51398de7f70c4157aaf0328f90035123c6`。在 WSL 中对应的安装位置是 `/var/lib/waydroid/overlay/system/lib64/arm64/libnativebridge.so`（容器内为 `/system/lib64/arm64/libnativebridge.so`）。不要替换 x86_64 宿主或 ART APEX 的同名库，也不需要复制 Houdini。仓库不分发 MuMu 文件；这里记录的是已测组合，不代表该库对所有应用都有收益。

**游戏兼容性（2026-11-22）。** 据用户实机游玩确认，在 Waydroid overlay 部署的 Digitalis 转译层与 MuMu 的 ARM64 客体库 `libnativebridge.so` 同时加载时，**明日方舟、碧蓝档案和 Limbus Company** 均可正常运行并实际游玩。这一结果针对两者组合，不能单独归因于 MuMu 单库。

---

<a id="english"></a>

# Additional changes and test results

## 1. Windows ADB forwarding

**How it works.** Windows ADB always connects to `127.0.0.1:15555`; it does not need the container IP.

1. **Resolve the destination.** The [ADB setup](../services/waydroid-acc-start) uses `lxc-attach` to read the current container's IPv4 address on `eth0`. It requires `ro.adb.secure=1`. If the image cleared USB debugging at boot, the launcher restores that setting only if Developer options remain enabled. WSL probes TCP port 5555 before opening the forward.
2. **Forward the stream, not the authorization.** A transient systemd socket, `waydroid-adb-local.socket`, listens only on WSL loopback. On connection, `systemd-socket-proxyd` relays bytes to the current container IP on port 5555. `NoDelay=yes` disables Nagle's small-write buffering on accepted TCP connections. Android still authorizes the Windows key. The socket and proxy stop with the container and do not start at WSL boot; failure to set up the forward does not prevent Waydroid from starting.

Run the following on Windows. On the first connection, approve the authorization prompt in Android:

```powershell
adb connect 127.0.0.1:15555
adb -s 127.0.0.1:15555 shell id -u
```

**Test results.** The deployed port reported `NoDelay=yes` and an authorized shell returned UID 2000. Replaying the same 2,093,574-byte PNG before and after enabling the option reduced median Windows receive time from **132 to 117 ms** and P95 from **187 to 137 ms**. The received bytes passed SHA-256 verification on every run. When the settings were swapped between two temporary ports, the live screenshot improvement followed the setting, not the port.

## 2. Virtual multi-touch screen for the container

**How it works.** WSL creates the touchscreen; Android sees one new input device and never needs access to host `/dev/uinput`.

1. **Create the device.** After Android boots, the [touch service](../services/systemd/waydroid-acc-touch.service) loads `evdev` and `uinput` and runs the [device program](../src/virtual-touch.c). It registers an `INPUT_PROP_DIRECT` Type B touchscreen with ten contact slots, tracking IDs and pressure from 0 to 255. The X/Y maxima are the startup width and height minus one. The process keeps the uinput descriptor open for the lifetime of the device.
2. **Publish only its event node.** The program obtains this device's `inputN` with `UI_GET_SYSNAME`. The [node manager](../services/waydroid-acc-touch-node) reads the matching `eventN` major/minor numbers from sysfs. In the container's `/dev`, it creates a character node with those numbers, sets `root:input 0660`, then hard-links it into `/dev/input/eventN`. Android EventHub can open the node as soon as it appears. No existing node is replaced and the host input directory is not exposed.
3. **Deliver input and clean up.** minitouch writes to this event node; Android receives direct touch input. The original Wayland mouse and keyboard remain independent. On shutdown the manager checks the recorded device identity and inode before unlinking the published node.

**Test results.** Stock minitouch, run from Windows via an ordinary ADB shell, selected the touchscreen automatically. Its handshake returned `v 1` and `^ 10 1919 1079 255` (10 contacts, maximum X/Y of 1919/1079, maximum pressure 255). Taps, drags and the original mouse worked in prototype testing. Stopping and starting the deployed service independently also removed and recreated the node correctly.

## 3. Lossless Android PNG screenshots

**How it works.** The changes begin after capture; Android still selects the display, composes the frame and enforces capture permissions. The [installer](../deploy/wsl-waydroid-acc/deploy.sh) places a [PATH wrapper](../services/screencap) at `/system_ext/bin/screencap` without replacing `/system/bin/screencap`.

1. **Capture.** Only an invocation with exactly one argument, `-p`, takes the new path. The wrapper calls stock screencap without arguments and pipes the raw frame, not a PNG, to the [encoder](../src/adb-png.c). Other arguments go directly to the stock program.
2. **Encode losslessly.** The encoder checks the header for 1920×1080 or 1080×1920 sRGB RGBA8888 and verifies every alpha byte is 255. Per row, PNG Sub stores each channel's difference from the corresponding channel to its left. Libdeflate compresses at level 1. The encoder emits an RGBA PNG with sRGB metadata and chunk CRCs, writing at most 256 KiB at a time. Windows still uses `adb shell screencap -p`.
3. **Fall back.** An unsupported frame returns status 75 before any PNG bytes are written. The wrapper then **captures a new frame** with `/system/bin/screencap -p` and coalesces writes into 256 KiB blocks with `toybox dd`. It never appends a second PNG to a partial first one. Calling the stock absolute path bypasses both optimizations.

**Test results.** The reference and fast encoders processed the same captured frame. All **8,294,400 decoded RGBA bytes** matched the source, color metadata matched, and PNG chunk CRCs passed. The table measures from the Windows request until RGB is available. Each row is a separate experiment: do not subtract values across rows or add the gains together.

| Local 1920×1080 scene | Runs per configuration | Median elapsed time |
|---|---:|---:|
| In-game dialog: direct platform PNG output → coalesced writes | 12 | 2264 → 553 ms |
| Game title screen: chunked output → fast PNG encoder | 16 | 537 → 260 ms |
| Game in foreground, deployed port: unchanged fast encoder, NoDelay enabled | 12 | 270 → 233 ms (P95: 298 → 248 ms) |


<a id="weston-window-and-input-en"></a>

## 4. Weston window and input

**How it works.** The [launcher](../services/waydroid-acc-start) validates the arguments and writes the window mode, dimensions and mouse mode to `/run/waydroid-acc-window.env`. Nested Weston implements the actual behavior:

- **Window and display.** The default window is resizable. With `-w fix`, the [Wayland backend](../weston/libweston/backend-wayland/wayland.c) requests identical minimum and maximum sizes from the parent xdg-shell and suppresses edge resizing. With `-w ratio`, resizing the outer window leaves the Android output mode unchanged. The [GL renderer](../weston/libweston/renderer-gl/gl-renderer.c) centers the image in an [aspect-preserving rectangle](../weston/libweston/acc-fit.h) and clears the remaining area to black. Input uses the inverse mapping of that same rectangle so clicks stay aligned with the image.
- **Mouse and touch.** The default `-m pointer` keeps pointer input. With `-m touch`, the backend translates left-button press, drag and release into a single contact's down, motion and up without forwarding hover or wheel events to Android. Ctrl + left press only arms a pinch: a drag of at least four output-coordinate units starts two contacts. For a first contact at `(x,y)`, the second is at `(width−1−x, height−1−y)`, mirrored about the **display center**, not the press position. When these are the only contacts under the default grab, the backend ends the client gesture with cancel before releasing its internal contacts, so the releases are not delivered as taps. This path is independent of the uinput device in section 2.

**Test results (2026-11-21).** The new build passed the offscreen shared-texture check. Dragging the same edge of a standalone 800×450 Weston window on Windows left its size unchanged in fix mode. In ratio mode the outer window narrowed, the test circle remained round, and black bars appeared above and below it. Waydroid was then restarted with `sudo waydroid-acc-start -w ratio -r 1280x720`. Before and after resizing, `wm size` reported `Physical size: 1280x720`. Android recognized touchscreen axes of 0–1279 and 0–719; minitouch reported `^ 10 1279 719 255`. An injected upward swipe unlocked the screen and reached Home. A mouse click on the Settings icon in the narrowed Windows window opened Settings. Windows screenshots confirmed the content aspect ratio on the lock screen, Home and Settings. Multi-monitor DPI changes and game frame rates while scaling were not tested.

<a id="digitalis-arm64-en"></a>

## 5. Optional ARM64 app compatibility: Digitalis and one MuMu library

**How it works.** Digitalis executes ARM64 code in x86_64 Waydroid. Its [overlay installer](../deploy/digitails/deploy.sh) does not rebuild the system image, use KernelSU or run as part of graphics deployment.

- **ARM64 libraries inside apps:** The installer backs up the overlay files it will change, stops the container and installs the 74 translator, proxy, guest-library and configuration entries at their packaged paths. It sets `ro.dalvik.vm.native.bridge=libberberis_arm64.so` in `vendor/build.prop` so that the Android runtime selects Digitalis rather than the NDK bridge after restart.
- **Standalone ARM64 programs:** The installer masks the old `ndk_translation.rc` with a no-op configuration to prevent competing registrations. After restart the [rule manager](../deploy/digitails/binfmt.py) points only the `arm64_dyn` and `arm64_exe` binfmt_misc rules at the Digitalis ARM64 runner; it does not flush other rules. The optional MuMu library below belongs on the ARM64 guest side, not in place of the host bridge.

**Use the verified build.** The repository includes [the Digitalis archive used on the test system and its standalone deployment script](../deploy/digitails/). Direct deployment does not require compilation or access to the local AOSP workspace.

**Build from source.** The tested image is Android 16 QPR2 (`BP4A.251205.006`). The build used Digitalis's `android16` branch at translator commit `8a2c30cb5383f89f8175eacc06ab1065ab7c5b6f`. To rebuild the current version (**CC1 + short leaf functions**), first apply both patches in order as described in the [Digitalis directory](../system-optimize/digitails/README.md#english), then run as an unprivileged user in the synced local WSL workspace:

```bash
bash ~/digitails/tools/build-bridge
cd ~/digitails/aosp/digitalis/dist/digitalis-prebuilts
sha256sum -c SHA256SUMS
```

This command uses a script from the local build workspace (not included in this repository). It builds and verifies the 74 translator, proxy library, guest library and configuration files, not an entire system image. For a new workspace, follow the [upstream Digitalis integration instructions](https://github.com/DigitalisX64/digitalis/blob/android16/docs/integrating-digitalis.md) to prepare matching AOSP sources.

**Optional MuMu library.** You can extract the ARM64 guest `libnativebridge.so` from your own MuMu installation for possible compatibility improvements in some apps. The tested source was MuMu Android 15 (`V417IR`). Connect to the corresponding emulator on Windows and extract only this file:

```powershell
adb connect 127.0.0.1:16384
adb -s 127.0.0.1:16384 pull /system/lib64/houdini/arm64/libnativebridge.so ./libnativebridge.so
Get-FileHash ./libnativebridge.so -Algorithm SHA256
```

The tested file's SHA-256 was `f72a269d577ed39ec409312675b41e51398de7f70c4157aaf0328f90035123c6`. Its WSL install path is `/var/lib/waydroid/overlay/system/lib64/arm64/libnativebridge.so` (inside the container: `/system/lib64/arm64/libnativebridge.so`). Do not replace the x86_64 host or ART APEX library of the same name. There is no need to copy Houdini. This repository does not distribute MuMu files; the tested combination is not a claim that the library improves every app.

**Game compatibility (2026-11-22).** The user confirmed through gameplay that **Arknights, Blue Archive and Limbus Company** all run and are playable with both the Waydroid overlay installation of Digitalis and MuMu’s ARM64 guest `libnativebridge.so` loaded. This result applies to the combination; it does not isolate the contribution of the MuMu library.
