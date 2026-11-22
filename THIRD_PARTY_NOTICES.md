# 第三方许可证与版权声明

[English](#english)

根目录的 [GPL-3.0-or-later 授权说明](README.md#开源许可证)适用于项目有权授权的原创部分。下表列的是仓库实际附带的源码、头文件、工具链和二进制；上游文件保留原许可证。对外发布二进制时，不要只附根目录的 GPL 文本：按分发内容附上相应版权声明、许可证和 NOTICE；如适用许可证要求提供对应源码，还须另行履行。下表不能代替许可证原文。

若从独立运行包阅读本文，包内的 `LICENSE`、`licenses/` 和 `vendor/libdeflate-COPYING` 可离线查阅；指向 README、补丁及其他组件的链接以完整源码仓库为准。Digitalis 的声明随其独立归档分发。

## 图形栈与截图工具

| 随附内容 | 许可证与声明的位置 |
|---|---|
| **Mesa 26.0.8**：`vendor/` 源码包、`mesa/` 覆盖文件、`host/` 的 EGL 和 Gallium、`runtime/` 的 Gallium | Mesa 核心主要是 MIT，不代表整个源码包只有 MIT。[上游说明](licenses/mesa/license.rst) · [归档中的许可证及例外](licenses/mesa/licenses/) · [源码版权声明](licenses/mesa/source-notices.txt) · [Ubuntu 包的逐组件版权清单](licenses/mesa/debian-copyright)。后者作归属参考，不表示本仓库的二进制由 Ubuntu 编译。 |
| **Weston 14.0.2**：`vendor/` 源码包、`weston/` 覆盖文件、`runtime/` 的 libweston、GL renderer 和 Wayland backend | [Weston COPYING](licenses/weston/COPYING) · [源码及协议声明](licenses/weston/source-notices.txt) · [逐组件版权清单](licenses/weston/debian-copyright)。源码归档中的美术资源另有[自己的条款](licenses/weston/data-COPYING)，不应套用于三个运行库。 |
| **Wayland 1.24.0 / wayland-protocols 1.47**：`src/wayland-*.h`、由核心协议生成的 `src/core.c`、HWC 与 Weston 使用的协议代码 | [Wayland 文件头原文](licenses/wayland/source-notices.txt) · [Wayland 版权清单](licenses/wayland/debian-copyright) · [标准协议逐项清单](licenses/wayland-protocols/debian-copyright)。生成代码也保留对应协议的声明；项目自己的 `weston/texture.xml` 不是上游标准协议。 |
| **Microsoft DirectX-Headers**：实际编译使用的系统头文件、Mesa 归档自带的备用版本 | [系统包版权声明](licenses/directx-headers/debian-copyright) · [归档内备用版本的原始许可证](licenses/directx-headers/bundled-LICENSE)。版本和提交见[上游版本表](README.md#当前版本使用的上游源码)。这不授权分发 Windows 驱动。 |
| **BLAKE3 1.8.2**：`vendor/` 的 crate；C 和汇编实现编入 `runtime/input-broker` | 上游提供三选一授权，不是三项叠加：[CC0-1.0](licenses/blake3/LICENSE_CC0)、[Apache-2.0](licenses/blake3/LICENSE_A2) 或 [Apache-2.0 WITH LLVM-exception](licenses/blake3/LICENSE_A2LLVM)。 |
| **libdeflate 1.23**：`vendor/` 的源码包；压缩与校验代码编入 `runtime/waydroid-acc-png` | [上游 COPYING](vendor/libdeflate-COPYING) 含 Eric Biggers、Google LLC 的版权声明及 MIT 许可全文。 |

Mesa、Weston 的源码声明按构建输入及头文件归集，并另含 Mesa EGL 源码声明；收录某个文件不表示其代码一定进入了每个 ELF。源包中的其他组件仍以包内原文件为准。Ubuntu 清单中引用的公共许可证可离线查阅：[Apache-2.0](licenses/common/Apache-2.0)、[GPL-1](licenses/common/GPL-1)、[GPL-2](licenses/common/GPL-2)、[GPL-3](LICENSE)。

## Android 接口和编译工具链

- **AOSP 头文件与 HWC 适配库**：`include/`、`src/hardware/` 中有原样导入、兼容裁剪和生成的头文件。保留各文件自身的版权和许可，包括 Apache-2.0 及部分 Android 原生头的 BSD 类条款；没有文件头的生成文件不能据此认定为本项目原创。[随仓库头文件的声明](licenses/aosp-header-notices.txt) · [AOSP 模块 NOTICE](licenses/AOSP-NOTICES.txt) · [比对提交和文件示例](README.md#当前版本使用的上游源码)。
- **NDK r27c 与 ANGLE**：`toolchains/android-ndk-r27c/` 包含实际工具链，不仅是下载脚本。归档保留各子目录的声明；另从归档原样取出 [NDK NOTICE](licenses/ndk/NOTICE)、[NOTICE.toolchain](licenses/ndk/NOTICE.toolchain)、[LLVM NOTICE](licenses/ndk/llvm-NOTICE) 和 [sysroot NOTICE](licenses/ndk/sysroot-NOTICE)，供随工具链或运行件分发时查阅。`src/platform.cpp` 编译使用 NDK 自带的 ANGLE `EGL/Platform.h`（Copyright 2015 The ANGLE Project Authors）；[ANGLE 许可证](licenses/angle/LICENSE)来自 AOSP 的 ANGLE checkout `ff25cbdf`，不据此推定 NDK 头文件的精确提交。
- **Android EGL/GLES shim、HWC 和宿主服务**：`runtime/` 是混合来源的部署集合，不是一个统一授权的新上游库。HWC 包含 Wayland 核心协议代码并使用 Android HAL 头；EGL shim 使用 ANGLE 平台接口；broker 和 PNG 工具分别嵌入上表的 BLAKE3 和 libdeflate。宿主进程链接的系统库不等于随仓库复制了那些库的二进制。

## Digitalis 可选转译包

CC1 与短叶函数补丁只修改现有转译器代码，未引入新的第三方依赖；其余 73 项产物未变。当前归档沿用同一份 NOTICE 对应表，并附[两份源码补丁](system-optimize/digitails/patches)。

[Digitalis 打包工程许可证](system-optimize/digitails/Digitalis-LICENSE)是 Apache-2.0，但它不能代替其中 Android guest 库、转译器静态依赖及工具链运行库的声明。[Digitalis 预编译包](deploy/digitails/)已内置该许可证及以下两份文件，原包单独分发无需另附；拆包再分发时请保留这些声明：

- [第三方声明原文](system-optimize/digitails/THIRD-PARTY-NOTICES.txt)：包括 Bionic libc、libm 和 linker、BoringSSL、ICU/Unicode、SQLite、zlib、LLVM 运行库等。内容相同的文本仅列一次，保留所有来源路径及原文 SHA-256。
- [74 项产物的 NOTICE 对应表](system-optimize/digitails/NOTICE-MAP.tsv)：将归档逐项与本次 AOSP 构建输出核对哈希后，按实际安装路径匹配 Soong 的许可元数据，递归收集非动态、非工具依赖的声明；独立随包的动态库按自己的安装路径核对。对元数据未直接给出声明文本的预编译依赖，另按依赖路径补入该构建的 Clang NOTICE、Rust 1.88 标准库版权说明及相关许可；BoringSSL 另附其上游 LICENSE。对应表同时列出这些补充来源。此表反映构建系统声明的依赖，不把每个同名 guest 代理当成完整上游库。

## 不在授权范围内的内容

本仓库不随附完整 Waydroid / LineageOS 系统镜像、WSL 内核、微软或显卡厂商的驱动，也不随附 MuMu 的 `libnativebridge.so`。文档中的版本、提取示例或文件哈希不构成再分发授权。测试截图和录屏中出现的第三方应用、游戏画面及标识不因收录而改用本项目的 GPL。可选 KernelSU ZIP 只包含仓库内模块的三个脚本或配置文件，不含 KernelSU 本体。

---

<a id="english"></a>

# Third-party licenses and copyright notices

The [GPL-3.0-or-later license statement](README.en.md#license) applies to original work that the project has the right to license. The following inventory covers the source, headers, toolchain and binaries actually included in this repository. Upstream files retain their own licenses. When distributing binaries, do not include only the root GPL text: provide the copyright statements, licenses and NOTICE files applicable to the materials you distribute, and separately meet any applicable requirement to provide corresponding source. This inventory does not replace the license texts.

In the standalone runtime archive, `LICENSE`, `licenses/` and `vendor/libdeflate-COPYING` are available offline. Links to READMEs, patches and other components refer to the full source repository. Digitalis ships its notices in its own archive.

## Graphics stack and screenshot tools

| Bundled material | Licenses and notices |
|---|---|
| **Mesa 26.0.8**: source archive in `vendor/`, overrides in `mesa/`, EGL and Gallium in `host/`, and Gallium in `runtime/` | Mesa core is primarily MIT-licensed, but that is not the only license in the source archive. [Upstream overview](licenses/mesa/license.rst) · [Licenses and exceptions in the archive](licenses/mesa/licenses/) · [Source copyright notices](licenses/mesa/source-notices.txt) · [Ubuntu per-component copyright inventory](licenses/mesa/debian-copyright). The Ubuntu inventory is an attribution reference; it does not imply that Ubuntu built this repository's binaries. |
| **Weston 14.0.2**: source archive in `vendor/`, overrides in `weston/`, and libweston, GL renderer and Wayland backend in `runtime/` | [Weston COPYING](licenses/weston/COPYING) · [Source and protocol notices](licenses/weston/source-notices.txt) · [Per-component copyright inventory](licenses/weston/debian-copyright). Artwork in the source archive has [its own terms](licenses/weston/data-COPYING); do not apply those terms to the three runtime libraries. |
| **Wayland 1.24.0 / wayland-protocols 1.47**: `src/wayland-*.h`, `src/core.c` generated from the core protocol, and protocol code used by HWC and Weston | [Original Wayland header notices](licenses/wayland/source-notices.txt) · [Wayland copyright inventory](licenses/wayland/debian-copyright) · [Standard protocol inventory](licenses/wayland-protocols/debian-copyright). Generated code retains the notices of its corresponding protocol. The project's `weston/texture.xml` is not an upstream standard protocol. |
| **Microsoft DirectX-Headers**: the system headers used to build and the fallback version included in the Mesa archive | [System package copyright notice](licenses/directx-headers/debian-copyright) · [Original license of the bundled fallback](licenses/directx-headers/bundled-LICENSE). See the [upstream revisions table](README.en.md#upstream-sources-used-by-the-current-version) for versions and commits. These notices do not grant permission to distribute Windows drivers. |
| **BLAKE3 1.8.2**: the crate in `vendor/`; its C and assembly implementation is compiled into `runtime/input-broker` | Upstream offers a choice of three licenses, not three cumulative requirements: [CC0-1.0](licenses/blake3/LICENSE_CC0), [Apache-2.0](licenses/blake3/LICENSE_A2), or [Apache-2.0 WITH LLVM-exception](licenses/blake3/LICENSE_A2LLVM). |
| **libdeflate 1.23**: the source archive in `vendor/`; compression and checksum code is compiled into `runtime/waydroid-acc-png` | [Upstream COPYING](vendor/libdeflate-COPYING) contains the copyright notices for Eric Biggers and Google LLC and the full MIT license. |

Mesa and Weston source notices were collected from build inputs and headers, with additional notices from the Mesa EGL source. Listing a file does not imply that its code is present in every ELF. For other components in the source archives, consult the original files in those archives. Common license texts referenced by the Ubuntu inventories are available offline: [Apache-2.0](licenses/common/Apache-2.0), [GPL-1](licenses/common/GPL-1), [GPL-2](licenses/common/GPL-2), [GPL-3](LICENSE).

## Android interfaces and build toolchain

- **AOSP headers and HWC adapter:** `include/` and `src/hardware/` contain unmodified imports, compatibility subsets and generated headers. Each file retains its own copyright and terms, including Apache-2.0 and BSD-style terms in some Android native headers. A generated file with no header is not necessarily original work of this project. [Notices for the bundled headers](licenses/aosp-header-notices.txt) · [AOSP module NOTICE files](licenses/AOSP-NOTICES.txt) · [Reference commits and example files](README.en.md#upstream-sources-used-by-the-current-version).
- **NDK r27c and ANGLE:** `toolchains/android-ndk-r27c/` contains the actual toolchain, not just a download script. The archive retains notices in its subdirectories. Unmodified copies of [NDK NOTICE](licenses/ndk/NOTICE), [NOTICE.toolchain](licenses/ndk/NOTICE.toolchain), [LLVM NOTICE](licenses/ndk/llvm-NOTICE) and [sysroot NOTICE](licenses/ndk/sysroot-NOTICE) are also available for reference when distributing the toolchain or runtime files. `src/platform.cpp` compiles against the NDK's ANGLE `EGL/Platform.h` (Copyright 2015 The ANGLE Project Authors). The [ANGLE license](licenses/angle/LICENSE) comes from AOSP ANGLE checkout `ff25cbdf`; this does not identify the exact NDK header commit.
- **Android EGL/GLES shim, HWC and host services:** `runtime/` is a deployment collection with several origins, not a new upstream library under a single license. HWC includes Wayland core protocol code and uses Android HAL headers; the EGL shim uses the ANGLE platform interface. The broker embeds BLAKE3, and the PNG tool embeds libdeflate, as listed above. Linking host processes to system libraries does not mean those library binaries were copied into this repository.

## Optional Digitalis translation package

The CC1 and short-leaf patches modify existing translator code without adding third-party dependencies; the other 73 artifacts are unchanged. The archive retains the same NOTICE mapping and includes [both source patches](system-optimize/digitails/patches).

The [Digitalis packaging project license](system-optimize/digitails/Digitalis-LICENSE) is Apache-2.0. It does not replace the notices for the Android guest libraries, the translator's static dependencies or the toolchain runtimes. The [Digitalis prebuilt archive](deploy/digitails/) includes that license and both files below. The intact archive can be distributed on its own; retain these notices when redistributing unpacked contents:

- [Original third-party notices](system-optimize/digitails/THIRD-PARTY-NOTICES.txt): cover Bionic libc, libm and linker, BoringSSL, ICU/Unicode, SQLite, zlib, LLVM runtimes and others. Identical texts are included once, with all source paths and the SHA-256 of the original text retained.
- [NOTICE mapping for the 74 artifacts](system-optimize/digitails/NOTICE-MAP.tsv): each archive entry was matched by hash to the AOSP build output. Its actual installation path was then matched to Soong license metadata, recursively collecting notices for dependencies that are neither dynamic nor tools. Dynamic libraries shipped separately were checked against their own installation paths. For prebuilt dependencies whose metadata did not directly provide notice text, dependency paths were used to include this build's Clang NOTICE, Rust 1.88 standard library copyright statement and related licenses. BoringSSL's upstream LICENSE is also included. The mapping identifies these additional sources. It reflects dependencies declared by the build system; it does not treat every similarly named guest proxy as a full copy of an upstream library.

## Materials not covered by the project's license grant

This repository does not include complete Waydroid / LineageOS system images, a WSL kernel, Microsoft or GPU vendor drivers, or MuMu's `libnativebridge.so`. Documenting a version, extraction example or file hash does not grant redistribution rights. Third-party apps, game imagery and logos appearing in test screenshots or recordings are not relicensed under the project's GPL. The optional KernelSU ZIP contains only the module's three scripts or configuration files from this repository, not KernelSU itself.
