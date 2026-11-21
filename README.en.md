# WSL-Waydroid-ACC

[简体中文](README.md) | English

## 0. Before you start

This project was developed entirely with DeepSeek V4.1 Flash 1017 and GPT-6.2 Astra. It is an alpha experiment, not a thoroughly tested release. Long-term maintenance is not guaranteed; the intention is to share an approach that others can improve on.

The project modifies Android system libraries. Applications may detect those changes. The author accepts no responsibility for account bans resulting from use of this project.

## 1. Overview

WSL-Waydroid-ACC brings GPU acceleration to **x86_64 Waydroid on WSL2 using the Windows host GPU**. Android applications continue to use EGL and OpenGL ES. Mesa's D3D12 backend submits their rendering work to the Windows graphics driver. Eligible application layers and the physical display's composition target travel as shared textures to a nested Weston compositor, which WSLg displays on the Windows desktop.

**Deployment baseline (2026-11-21): `built.0L1nxt11`.** This was the release directory name on the test machine when the baseline was frozen, not a Git commit. The 11 binaries and 12 scripts/configuration files in `runtime/` were checked against the installed files. Host EGL and Gallium have a separate [manifest](host/SHA256SUMS). `out/` holds subsequent builds and is not a restore source. After deployment, both host services and Android load their components from the installation, not the source tree or NDK. [Baseline SHA-256 manifest](runtime/SHA256SUMS)

The [optional KernelSU module](system-optimize/README.md#english) is separate from the graphics baseline and is not installed by default. Optional ARM64 compatibility uses a Waydroid overlay, not a KernelSU module; see [Digitalis and the optional MuMu library](MISC.md#digitalis-arm64-en). The [Digitalis prebuilt archive](system-optimize/digitails/README.md#english) includes its license and third-party notices and can be distributed separately.

## 2. How it works

### How a frame reaches Windows

1. **Access D3D12 from an Android process.** The EGL/GLES shims receive Android graphics calls. A custom ELF loader handles symbol resolution, relocations and thread-local storage across Bionic and glibc, allowing Mesa Gallium's D3D12 driver to call WSL's DXCore and D3D12 libraries and use the host GPU through `/dev/dxg`. [Loader](src/loader_so.c) · [EGL shim](src/egl_shim.c)
2. **Hand application windows to SurfaceFlinger.** Application windows still dequeue buffers from Android BufferQueue. When dimensions, format and usage qualify, the producer exports a GPU texture and a readiness signal. The host input broker tracks them by buffer ID and content generation. RenderEngine can sample that generation directly and reuse an import on repeated binds. Gralloc still allocates Android buffers and supplies their handles and layouts; a CPU mirror is produced on demand. [Window publication](src/window.h) · [Input broker](src/broker.c) · [RenderEngine input](src/input.h)
3. **Hand the physical display's CLIENT target to Weston.** RenderEngine copies the composed image to a shareable GPU texture. Through the project's Wayland protocol, HWC asks nested Weston to import and sample it. Successful imports avoid GPU-to-CPU readback and a full-frame `wl_shm` copy. The first import requires confirmation; validated producer/size combinations can request subsequent imports asynchronously. On failure, the current image remains visible while a CPU mirror is requested for recovery. [Target export](src/producer.h) · [HWC submission](src/client-hwc.h) · [Weston import](weston/libweston/renderer-gl/texture.c)

### Remaining bottlenecks

- **No end-to-end dma-buf passthrough between Android gralloc and the host GPU on this WSL2 stack.** Waydroid can allocate and pass dma-bufs inside the Linux guest, but the host GPU exposes rendering resources through D3D12 and `/dev/dxg`. The tested dxgkrnl/Mesa/gralloc combination lacks an interface through which both sides can import the same allocation. Passing a gralloc fd to Weston therefore does not let it sample the texture the host GPU just rendered. [WSL GPU architecture](https://devblogs.microsoft.com/directx/directx-heart-linux/) · [Linux dma-buf import/export](https://docs.kernel.org/6.18/driver-api/dma-buf.html)

  Android buffers still serve queueing and release, while D3D12 shared textures travel separately. Weston imports them with `GL_HANDLE_TYPE_OPAQUE_FD_EXT`. The shared path avoids a full CPU readback, but the physical CLIENT target still needs a GPU snapshot and readiness synchronization. CPU consumers still need a mirror. [Mesa export](mesa/src/mesa/state_tracker/st_cb_readpixels.c) · [Weston import](weston/libweston/renderer-gl/texture.c)
- **GPU snapshots still cost a copy and synchronization.** Preallocation creates the destination in advance, but RenderEngine still has to blit from the current framebuffer, submit work and pass the readiness signal. In the Settings test the median combined time for target creation, submission and handle processing fell from 3.19 to 1.54 ms. [Stage timings](reference/RESULT.md#english)
- **Buffer release and cross-process replies can delay the slowest frames.** A producer may wait for a free BufferQueue slot or for the broker to confirm visibility. Letting the broker handle other windows' requests while waiting for a GPU query did not consistently improve reply P95 across two runs. With all optimizations on, notification-shade display-frame P95 was 16.45 and 17.68 ms respectively; the latter exceeds the 16.67 ms budget at 60 Hz. The measurements do not isolate the broker as the cause. [Comparison](reference/display.csv) · [Queue and reply timings](reference/stages-reverse.csv)
- **The shared path is constrained by the buffer contract.** Readback, format conversion and shared-memory copies can still occur for ineligible dimensions, formats, strides or usage flags, when a consumer needs CPU pixels, or when Weston cannot import the texture. Recheck gralloc layouts, the HWC interface and window eligibility when porting to a different image or resolution. [Window eligibility](src/window.h) · [HWC fallback](src/adapter.c)

### Known issue in the tested image

With the lock screen disabled, launching Settings from Home after a cold boot can make the expanding splash look transparent. In this LineageOS 23.2 image, Quickstep uses its initial navigation-bar position of zero as the bottom crop edge until it receives the first position callback. Restarting just the launcher reproduces it; opening and closing the notification shade restores the animation. With swipe-to-unlock enabled, the first launch after a cold boot and unlock looked correct without touching the shade. This is documented as an image issue: the project does not patch the launcher, SystemUI or graphics layer, or simulate opening the shade at startup. [On-device comparison and cause](reference/splash/README.md#english)

## 3. Test results

**Baseline verification, 2026-11-21.** Weston, the input broker and the texture consumer all mapped the same installed release. The Android graphics libraries, screenshot tooling and host service configuration were checked against the installed files. The baseline includes the memory and window changes below; it does not enable experimental scheduling policies. [Frozen files and verification commands](reference/baseline-20261121.md#english) · [Read-only CPU/GPU scheduling measurements](system-optimize/results/scheduling/README.md#english)

**Display and memory at 1080p.** The default physical display is 1920×1080, with no Weston top panel. During Settings launch, scrolling and return to Home, Weston imported 67 full-size EGLImages. In two scrolling batches, display-frame P50 was 7.50 / 7.81 ms and P95 was 9.31 / 9.67 ms. Each row below describes a separate comparison: do not add savings across rows or treat the 1920×1048 measurements as a like-for-like comparison. [1080p samples](reference/display-1080.csv)

| Baseline change | Result under matched conditions |
|---|---|
| [Full-buffer BLAKE3 validation in the broker](reference/shade-1080/README.md#english) | All bytes, including row padding, remain covered. Combined shade-open/close P95 over Calculator: 19.26 → 15.22 ms. Opening alone still measured 19.00 ms. |
| [Reuse imports within a generation](reference/shade-1080/validated-import/README.md#english) | Reuse occurs only after the broker validates the writable buffer for that request. Shade-open P95: 19.02 → 16.15 ms; 22/452 frames still exceeded the 60 Hz budget. |
| [Share read-only AMD driver pages](reference/memory-1080/loader-system/README.md#english) | Whole-stack working set: 4862 → 2977 MiB at Home; 5884 → 4047 MiB after opening Settings, Jelly and Calculator. No swap at those checkpoints. |
| [Release ELF parser mappings and fds; share read-only Gallium pages](reference/memory-1080/loader-lifetime/deployed/README.md#english) | Relative to AMD sharing already enabled, the desktop and multi-app working sets fell by a further 178.4 and 217.8 MiB. |
| [Reuse CPU staging slots](reference/memory-1080/remaining/pool-deployed/README.md#english) | Repeatedly opening Search and returning Home reduced its pool from 8 slots to 1 (63.28 → 7.91 MiB). None of the four measured actions had a worse display-frame P95. |
| [Share read-only libcrypto and libstdc++ pages](reference/memory-1080/remaining/shared-split/README.md#english) | In the multi-app test, enabling one library saved 65.05 or 28.46 MiB, respectively; together they saved 93.55 MiB. These are resident physical pages of the target mappings, deduplicated across 12 graphics processes, not whole-machine savings. No repeatable shade-open tail regression could be attributed to one library. |

The following configuration was read from Windows, the running WSL distribution and the Waydroid container on 2026-11-20.

| Component | Measured version or configuration |
|---|---|
| Windows | Windows 11 Pro Insider Preview, x64, OS build `29708.1003` |
| GPU / Windows driver | AMD Radeon RX 7900 XTX, display driver `32.0.32029.2010` |
| WSL / WSLg | WSL `3.0.5.0`, distribution in WSL 2 mode; WSLg `1.0.79` |
| WSL distribution | Ubuntu `26.04.1 LTS` (Resolute Raccoon), x86_64 |
| WSL kernel | `uname -r`: `7.2.8-microsoft-standard-WSL2`, selected in `.wslconfig`; the bundled kernel reported by `wsl --version` was `6.18.58.1-1` |
| Waydroid | `1.6.3`, container vendor type `MAINLINE` |
| Waydroid image | Android 16, LineageOS 23.2, `ro.lineage.version=23.2-20260717-UNOFFICIAL-WayDroidATV-waydroid_x86_64`. OTA channel `a16-qpr2`, `GAPPS` system image, security patch `2026-07-01` |
| Renderer and display | SurfaceFlinger reported `D3D12 (AMD Radeon RX 7900 XTX)`, OpenGL ES 3.1, Mesa 26.0.8. Nested Weston 14.0.2. Android's physical output then was 1920 × 1048; the current baseline defaults to 1920 × 1080. |

The **2026-11-20 build** passed offscreen validation of 12 shared-texture generations at two sizes. After deployment and restart, Home, Settings and return to Home were inspected from Windows, followed by a manual trial. [Offscreen output](reference/baseline-build.log) · [Pixel and readiness checks](validation/check.c)

In two batches run in opposite orders, Settings scrolling improved from 11.53 / 11.43 to 6.97 / 7.08 ms (SurfaceFlinger display-frame P50). A separate 12-second, 720-frame Windows recording covered scrolling, app switching, the notification shade, Recents and return to Home. Frame-by-frame screening found no all-black frames or horizontal black bands in that recording. These timings and video are for the then-current 1920×1048 setup, not frame-by-frame acceptance of the current 1080p baseline. [Method and results](reference/RESULT.md#english) · [60 FPS recording](reference/111-60fps.mp4) · [Frame log](reference/111-60fps.csv)

### Whole-stack resource use and frame rate with Blue Archive in the foreground

A 60-second sample was taken on 2026-11-20 from 13:13:08 to 13:14:08 (UTC+8) on the 1048p build. Both the buffer Unity submitted and Android's output were **1920 × 1048**; the game's internal 3D resolution was not measured. Totals cover the Android container, nested Weston, input broker, texture consumer and WSL-side container, session and network helpers. They exclude the Windows-side WSLg client and dedicated GPU memory.

| Metric | 60-second result |
|---|---|
| Total Waydroid CPU | **138.5%** average, or 1.38 logical cores: **8.65%** of the 16 available to WSL. Peak 230.1% in 1.5-second samples. |
| Total Waydroid memory | **12.12 GiB** average including cache. Subtracting about 5.01 GiB of inactive file cache gives an estimated **7.11 GiB** average working set, peak 7.24 GiB. |
| Game process alone | **101.9%** average CPU, **2.21 GiB** average PSS (shared pages apportioned proportionally). |
| Game-layer ready rate | **3,266 frames, 54.43 FPS** average. Median per-second count 56, minimum 8. |
| Game frame-ready interval | P50 **17.11 ms**, P95 **26.08 ms**, longest **522.6 ms**. |
| SurfaceFlinger display composition | **3,267 completed frames, 54.45 frames/s** over the same interval. |

Only 8 game frames became ready between seconds 32 and 33, and 20 in the next second. This interval contained the longest gap. Actual present timestamps for the game layer returned `-1`, so game FPS uses deduplicated buffer-ready times, cross-checked against SurfaceFlinger completions. The sample does not establish what caused the stall. [Resource and frame counts at 1.5-second intervals](reference/bluearchive-20261120-131308.csv) · [Per-frame ready times](reference/bluearchive-20261120-131308-frames.csv)

## 4. Build instructions

### Upstream sources and baseline checkouts

`build.sh` unpacks the bundled Mesa and Weston sources and applies local files from [mesa/](mesa/) and [weston/](weston/). Use the commits below to check out upstream originals and compare changes. The actual build inputs are the repository's archives and overlays. [Archive SHA-256 manifest](vendor/SHA256SUMS) · [Build script](build.sh)

| Upstream | Tag / commit | Used for |
|---|---|---|
| Mesa | `mesa-26.0.8` → `60e95b787857afbc9a00b693b91c0d9c8923a430` | Gallium D3D12, texture export and synchronization. The commit in the original archive matches the tag. [Commit](https://gitlab.freedesktop.org/mesa/mesa/-/commit/60e95b787857afbc9a00b693b91c0d9c8923a430) |
| Weston | `14.0.2` → `015b3b4d4c05da44a22349ea6e651d1a8f678c59` | Modified `libweston-14.so.0`, `gl-renderer.so` and private `wayland-backend.so`. Host Weston package: `14.0.2-5`. [Commit](https://gitlab.freedesktop.org/wayland/weston/-/commit/015b3b4d4c05da44a22349ea6e651d1a8f678c59) |
| BLAKE3 | `1.8.2`; archive commit `df610ddc3b93841ffc59a87e3da659a15910eb46` | 32-byte broker CPU-mirror digest. Builds C and x86-64 assembly directly from the bundled upstream crate; no Cargo, network or runtime BLAKE3 shared library. The archive contains upstream licenses. [Commit](https://github.com/BLAKE3-team/BLAKE3/commit/df610ddc3b93841ffc59a87e3da659a15910eb46) |
| libdeflate | [v1.23](https://github.com/ebiggers/libdeflate/tree/v1.23); SHA-256 in the [vendor manifest](vendor/SHA256SUMS) | Only the sources needed for x86-64 compression and checksums are built into the Android PNG encoder; no runtime shared library. [License](vendor/libdeflate-COPYING) |
| Microsoft DirectX-Headers | System headers: `v1.619.1` → `9e393d6d8a3b30dcc6f2806ef604ec16a27b0d7e`, Ubuntu package `1.619.1-1` | Mesa's Meson log confirms pkg-config selected the system headers. The Mesa archive also contains fallback checkout `v1.618.1` (`64c650b0c6642ab0f3cdf674e38401e02ccd2460`). [System commit](https://github.com/microsoft/DirectX-Headers/commit/9e393d6d8a3b30dcc6f2806ef604ec16a27b0d7e) · [Bundled commit](https://github.com/microsoft/DirectX-Headers/commit/64c650b0c6642ab0f3cdf674e38401e02ccd2460) |
| Wayland | `1.24.0` → `736d12ac67c20c60dc406dc49bb06be878501f86` | Android HWC uses the bundled client headers, verified against Ubuntu `libwayland-dev 1.24.0-2`. The client protocol code identifies `wayland-scanner 1.24.0`; the Weston side is generated from this project's XML at build time. [Commit](https://gitlab.freedesktop.org/wayland/wayland/-/commit/736d12ac67c20c60dc406dc49bb06be878501f86) · [Shared-texture protocol](weston/texture.xml) |
| wayland-protocols | `1.47` → `88223018d1b578d0d8869866da66d9608e05f928` | Standard protocols used by Weston; Meson detected Ubuntu `1.47-1`. [Commit](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/commit/88223018d1b578d0d8869866da66d9608e05f928) |
| Android NDK | `r27c`, revision `27.2.12479018` | Android x86_64 toolchain and sysroot, including the ANGLE `EGL/Platform.h` used by `platform.cpp`. No separate ANGLE source checkout. [NDK checksum](toolchains/android-ndk-r27c/SHA256SUMS) |

**Android HAL and window headers.** `include/hardware/{hardware,gralloc,fb}.h` and `include/cutils/native_handle.h` originally came from the AOSP GitHub mirror's `main` branch; no commit was pinned at download time. The examples below match the indicated `android16-qpr2-release` files byte for byte. Other compatibility headers are locally trimmed; these matches do not identify the entire Android image's repo manifest.

| AOSP repository | Comparison commit | Files verified |
|---|---|---|
| `platform/hardware/libhardware` | [`67b44bb0f7d1df962e737cfe9baf716bab45792e`](https://android.googlesource.com/platform/hardware/libhardware/+/67b44bb0f7d1df962e737cfe9baf716bab45792e) | [hwcomposer.h](src/hardware/hwcomposer.h), [hwcomposer_defs.h](src/hardware/hwcomposer_defs.h), [gralloc.h](include/hardware/gralloc.h) |
| `platform/system/core` | [`e9d1fa3705d7fbd0ac1c942bc4c276161bfbfed1`](https://android.googlesource.com/platform/system/core/+/e9d1fa3705d7fbd0ac1c942bc4c276161bfbfed1) | [native_handle.h](include/cutils/native_handle.h), [properties.h](include/android_stub/cutils/properties.h) |
| `platform/frameworks/native` | [`fcbde2bcff56ca1d7bec9cabf8bdca5e288bf103`](https://android.googlesource.com/platform/frameworks/native/+/fcbde2bcff56ca1d7bec9cabf8bdca5e288bf103) | [vndk/window.h](include/android_stub/vndk/window.h), [nativebase.h](include/android_stub/nativebase/nativebase.h) |
| `platform/system/logging` | [`58dac317ea08b8f275ebd01ce864acab2de30b93`](https://android.googlesource.com/platform/system/logging/+/58dac317ea08b8f275ebd01ce864acab2de30b93) | [log.h](include/android_stub/log/log.h), [log_id.h](include/android_stub/log/log_id.h) |

**Runtime versions.** Waydroid container manager `1.6.3` corresponds to `5b7e2e71be3f6bfaaaab3b461251dacaf1ce4991`. The graphics adapter loads the image's `hwcomposer.vendor-base.so` at runtime. Its exact source commit is unknown; deployment pins the binary by SHA-256: `942fad47d8372ea1eb6873a8ccad0954d91592b0fc6137d8eff2f638043f1a29`. See section 3 for the full LineageOS image ID. [Waydroid commit](https://github.com/waydroid/waydroid/commit/5b7e2e71be3f6bfaaaab3b461251dacaf1ce4991) · [HAL loading](src/adapter.c) · [Deployment checks](deploy.sh)

The test machine's separately built WSL kernel used `ZoeHao2026/wsl-kernel-7.2.8` at `53bb4a4b310111d020155333d1a378cbc80af869`. It is based on Linux stable `v7.2.8`, with Microsoft WSL changes ported from `linux-msft-wsl-6.18.40.1` (`14794180686c2fb6307fbe359c359bec765249f3`). [Kernel port and versions](https://github.com/ZoeHao2026/wsl-kernel-7.2.8/tree/53bb4a4b310111d020155333d1a378cbc80af869) · [Microsoft WSL kernel commit](https://github.com/microsoft/WSL2-Linux-Kernel/commit/14794180686c2fb6307fbe359c359bec765249f3)

### 4.1 WSL kernel and devices

`build.sh` builds userspace components. Offscreen GPU tests require a working WSL D3D12 device. Running Waydroid also requires Binder, graphics buffers, container networking and image mounts. Check these options to reproduce the test configuration (`=y`: built in; `=m`: module).

| Purpose | Test kernel configuration | What to verify |
|---|---|---|
| Host GPU | `CONFIG_HYPERV=y`, `CONFIG_HYPERV_VMBUS=y`, `CONFIG_DXGKRNL=y` | WSL and the container can access `/dev/dxg`; WSL provides `/usr/lib/wsl/lib/libd3d12.so` and `libdxcore.so`. |
| Android IPC and shared memory | `CONFIG_ANDROID_BINDER_IPC=y`, `CONFIG_ANDROID_BINDERFS=y`, `CONFIG_MEMFD_CREATE=y` | `/dev/binderfs` provides Binder devices; the container exposes `/dev/binder`, `/dev/hwbinder` and `/dev/vndbinder`. |
| Graphics buffers and synchronization | `CONFIG_DMA_SHARED_BUFFER=y`, `CONFIG_DMABUF_HEAPS=y`, `CONFIG_DMABUF_HEAPS_SYSTEM=y`, `CONFIG_SYNC_FILE=y` | `/dev/dma_heap/system` exists and can allocate system-memory buffers. |
| DRM devices used by this image | `CONFIG_DRM=y`, `CONFIG_DRM_VKMS=m`, `CONFIG_DRM_VGEM=m` | `vkms` and `vgem` are loaded. `gralloc.gbm.device` points to VKMS at `/dev/dri/card1`; `renderD128` is VGEM. Actual GPU rendering uses `/dev/dxg`. |
| Virtual multitouch | `CONFIG_INPUT=y`, `CONFIG_INPUT_EVDEV=m`, `CONFIG_INPUT_UINPUT=m` (or built in) | Modules must match the kernel. The touch service loads them on demand; no modules-load.d entry is needed. |
| Android images and writable layer | `CONFIG_BLK_DEV_LOOP=y`, `CONFIG_EROFS_FS=m`, `CONFIG_EXT4_FS=y`, `CONFIG_OVERLAY_FS=y` | The system image uses EROFS and vendor uses ext4; a writable overlay is mounted at runtime. |
| LXC and container networking | `CONFIG_NAMESPACES=y` with `UTS_NS`, `IPC_NS`, `USER_NS`, `PID_NS`, `NET_NS`; `CONFIG_CGROUPS=y`, `CONFIG_CGROUP_BPF=y`, `CONFIG_VETH=y`, `CONFIG_BRIDGE=m`, `CONFIG_BRIDGE_NETFILTER=m`, `CONFIG_NF_CONNTRACK=y`, `CONFIG_NF_NAT=y` | The test machine uses cgroup v2 and the `waydroid0` bridge. |

A custom kernel must retain WSL Hyper-V and dxgkrnl support. The module VHDX must match the booted kernel. Select the files with `kernel` and `kernelModules` under `[wsl2]` in the Windows user's `.wslconfig`. Stage sampling with `validation/measure.py` also requires `CONFIG_FTRACE=y` and `CONFIG_UPROBE_EVENTS=y`. [Microsoft kernel build instructions](https://github.com/microsoft/WSL2-Linux-Kernel#build-instructions) · [Custom kernel configuration](https://learn.microsoft.com/en-us/windows/wsl/wsl-config)

Check the current kernel and devices in WSL. `/proc/config.gz` requires `CONFIG_IKCONFIG_PROC`.

```bash
uname -r
zgrep -E '^CONFIG_(DXGKRNL|ANDROID_BINDER_IPC|ANDROID_BINDERFS|DMABUF_HEAPS_SYSTEM|SYNC_FILE|DRM_VKMS|DRM_VGEM|EROFS_FS)=' /proc/config.gz
ls -l /dev/dxg /dev/dma_heap/system /dev/binderfs /dev/dri
```

### 4.2 Build dependencies on Ubuntu 26.04 LTS

In Bash on x86_64 Ubuntu, install the host compiler, Mesa/Weston build tools and development packages. `directx-headers-dev` is in Ubuntu's `universe` component; `libwayland-bin` supplies `wayland-scanner`. The Android build uses the bundled NDK r27c (27.2.12479018). [Ubuntu DirectX-Headers package](https://packages.ubuntu.com/source/resolute/directx-headers) · [Build script](build.sh)

```bash
sudo apt update
sudo apt install build-essential binutils meson ninja-build pkgconf bison flex \
  python3 python3-mako python3-packaging python3-yaml directx-headers-dev \
  libdrm-dev libelf-dev libexpat1-dev zlib1g-dev libzstd-dev libdisplay-info-dev \
  libwayland-bin libwayland-dev wayland-protocols libudev-dev libinput-dev \
  libevdev-dev libxkbcommon-dev libxcb1-dev libpixman-1-dev libgbm-dev \
  libegl-dev libgl-dev libgles-dev libpng-dev libcairo2-dev libpango1.0-dev \
  libfontconfig-dev libglib2.0-dev liblcms2-dev libssl-dev libsystemd0
```

### 4.3 Build, offscreen validation and deployment

Run the following from the repository root. The script verifies the manifests in `toolchains/`, `vendor/` and `host/`, unpacks the NDK and prepares the Mesa/Weston builds in the WSL user's cache. It writes the graphics binaries (including the private Wayland window backend), WSL touch program, Android PNG encoder and SHA-256 manifest to `out/`. Use `bash build.sh` to build only; `--offscreen` validates shared textures using the newly built Gallium driver.

```bash
unset NDK ANDROID_NDK_HOME ANDROID_NDK_ROOT
JOBS=4 bash build.sh --offscreen
(cd out && sha256sum -c SHA256SUMS)
```

Deployment targets a **Waydroid instance already configured with this project's Android graphics libraries and overlay**. It requires WSLg, systemd and compatible Weston. On another machine, check the WSL user in the service templates, the vendor HAL in the image and the WSL graphics libraries in the container. Deploying restarts both Waydroid and nested Weston. [Deployment script](deploy.sh) · [Host service templates](services/systemd/) · [Android graphics library manifest](reference/android-libraries.sha256)

| WSL command | Action |
|---|---|
| `sudo bash deploy.sh built` | Deploy the new `out/` and development `services/`, without changing the frozen baseline. |
| `sudo bash deploy.sh apply` or `sudo bash deploy.sh restore` | Install or restore `runtime/` and its frozen scripts/configuration; no build required. No argument means `apply`. |
| `sudo waydroid-acc-start` | Start or restart the installed release from its standalone directory. [Launcher](services/waydroid-acc-start) |

**Window and resolution:** `-w` is short for `--weston`; `-r` for `--resolution`. Either can be used alone, in any order. Resolution refers to the **Android content area**, not the title bar, borders or Windows DPI scaling. Each invocation restarts the session; it does not resize it live or save the arguments as new defaults.

| WSL command | Window behavior |
|---|---|
| `sudo waydroid-acc-start` | Start at 1920×1080 with Weston's usual free resizing. |
| `sudo waydroid-acc-start -r 1600x900` | Start at 1600×900; still freely resizable. |
| `sudo waydroid-acc-start -w fix -r 1280x720` | Fix the content area at 1280×720. The window can be moved or closed; ordinary edge drags do not resize it. |
| `sudo waydroid-acc-start --weston ratio --resolution 1280x720` | Keep Android at 1280×720; fit the content into a resizable outer window, centered at its original aspect ratio with black bars. |

Use `WIDTHxHEIGHT`; both axes accept integers from 128 to 8192. This does not mean every size has been stress-tested on the GPU, in games or under memory pressure. The aspect ratio comes from `-r`, not a hard-coded 16:9. Fixed mode requests a normal-window size lock through xdg-shell; forced snapping and cross-monitor DPI behavior depend on WSLg. Scaling uses the GL viewport of one EGL window without readback. Non-native sizes redraw the whole frame rather than reusing native damage rectangles. Virtual touch axes follow the startup size. Non-1080p modes do not expand the validated shared-texture or PNG fast-path allowlists. The frozen baseline includes the custom Wayland backend and disables the Weston top panel. [Window and input verification](MISC.md#weston-window-and-input-en)

Deployment starts the container once to verify the display stack. On success it disables boot-time startup of `waydroid-container.service` without stopping the current session. Opening WSL alone will not start Weston via `multi-user.target`; run `sudo waydroid-acc-start` when you want the UI.

Host programs and Weston modules are installed in `/usr/local/lib/wsl-waydroid-acc/`; Android components go into the overlay and container data directory. This image also has an optional `services.jar` change for the startup GPU-status warning. [Standalone verification](reference/standalone/README.md#english) · [GPU startup notification](reference/android-system/README.md#english)

For Windows ADB access, virtual multitouch and faster lossless PNG screenshots, see [Additional configuration and test results](MISC.md#english).

## License

Original project material is licensed under the [GNU General Public License, version 3](LICENSE), or (at your option) any later version (`GPL-3.0-or-later`). Third-party components retain their own licenses and copyright notices; see [Third-party notices](THIRD_PARTY_NOTICES.md#english).
