#!/usr/bin/env bash
# Rebuild the current Waydroid acceleration stack inside WSL.
set -euo pipefail
root=$(cd -P "$(dirname "$0")" && pwd)
case ${1:-} in ''|--offscreen) ;; *) echo "Usage: bash build.sh [--offscreen]" >&2; exit 2;; esac
tag=$(printf '%s' "$root" | sha256sum | cut -c1-12)
work=${ACC_WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/wsl-waydroid-acc/$tag}
out="$root/out"
mkdir -p "$work" "$out"
exec 8>"$work/build.lock"; flock 8
sdk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [[ -z $sdk && -z ${NDK:-} ]]; then
    bundle="$root/toolchains/android-ndk-r27c"
    archive=ndk-linux-x86_64.tar.gz
    (cd "$bundle" && sha256sum --strict --quiet -c SHA256SUMS)
    digest=$(awk -v name="$archive" '$2 == name {print $1}' "$bundle/SHA256SUMS")
    [[ $digest =~ ^[0-9a-f]{64}$ ]]
    sdk="$work/android-ndk-r27c-$digest"
    if [[ ! -d $sdk ]]; then
        (
            tmp=$(mktemp -d "$work/.ndk.XXXXXXXX")
            trap 'rm -rf -- "$tmp"' EXIT
            tar -xzf "$bundle/$archive" -C "$tmp"
            mv -T "$tmp/android-ndk-r27c" "$sdk"
        )
    fi
fi
ndk=${NDK:-$sdk/toolchains/llvm/prebuilt/linux-x86_64/bin}
if [[ -d $ndk/toolchains ]]; then ndk="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"; fi
[[ -x $ndk/clang-18 ]] || { echo "Android NDK r27c compiler unavailable at $ndk/clang-18" >&2; exit 1; }
(cd "$root/vendor" && sha256sum --strict --quiet -c SHA256SUMS)
(cd "$root/host" && sha256sum --strict --quiet -c SHA256SUMS)
# Include the vendor revision in the build directory to isolate Meson caches.
prepare(){
    local project=$1 archive=$2 checksum dst manifest old file
    checksum=$(sha256sum "$root/vendor/$archive" | cut -c1-12)
    dst="$work/$project-$checksum"
    if [[ ! -e $dst/meson.build ]]; then
        local tmp
        tmp=$(mktemp -d "$work/.unpack.XXXXXXXX")
        tar -xzf "$root/vendor/$archive" -C "$tmp"
        mv -T "$tmp" "$dst"
    fi
    manifest="$dst/.acc-overlay"
    local list
    list=$(mktemp "$work/.overlay.XXXXXXXX")
    (cd "$root/$project" && find . -type f ! -name cmd_line.txt ! -name texture.xml -print | LC_ALL=C sort) >"$list"
    if [[ -f $manifest ]]; then
        while IFS= read -r old; do
            if tar -tzf "$root/vendor/$archive" "$old" >/dev/null 2>&1; then
                tar -xzf "$root/vendor/$archive" -C "$dst" "$old"; touch "$dst/$old"
            else
                rm -f -- "$dst/$old"
            fi
        done < <(LC_ALL=C comm -23 "$manifest" "$list")
    fi
    while IFS= read -r file; do
        cmp -s "$root/$project/$file" "$dst/$file" || install -D -m644 "$root/$project/$file" "$dst/$file"
    done <"$list"
    mv -f "$list" "$manifest"
    printf '%s\n' "$dst"
}
jobs=${JOBS:-12}
mesa=$(prepare mesa mesa-26.0.8.tar.gz)
mb="$mesa-build"
if [[ ! -f $mb/build.ninja ]]; then
    meson setup "$mb" "$mesa" -Dplatforms= -Degl-native-platform=surfaceless -Dgallium-drivers=d3d12 \
        -Dvulkan-drivers= -Dglx=disabled -Dgbm=disabled -Dllvm=disabled -Dbuildtype=release \
        -Dgallium-va=disabled -Dvideo-codecs= -Dgallium-rusticl=false
fi
gallium=libgallium-26.0.8.so
ninja -C "$mb" -j "$jobs" "src/gallium/targets/dri/$gallium"
install -m755 "$mb/src/gallium/targets/dri/$gallium" "$out/"

weston=$(prepare weston weston-14.0.2.tar.gz)
gl="$weston/libweston/renderer-gl"
wb="$weston-build"
generate_protocol(){
    local mode=$1 file=$2
    wayland-scanner "$mode" "$root/weston/texture.xml" "$gl/$file.new"
    if cmp -s "$gl/$file.new" "$gl/$file"; then rm "$gl/$file.new"; else mv "$gl/$file.new" "$gl/$file"; fi
}
generate_protocol server-header wd-server.h
generate_protocol private-code wd-protocol.c
if [[ ! -f $wb/build.ninja ]]; then
    meson setup "$wb" "$weston" --prefix=/usr --libdir=lib/x86_64-linux-gnu \
        -Dbackend-drm=false -Dbackend-drm-screencast-vaapi=false -Dbackend-default=wayland \
        -Dbackend-headless=false -Dbackend-pipewire=false -Dbackend-rdp=false -Dbackend-vnc=false \
        -Dbackend-x11=false -Dremoting=false -Dpipewire=false -Dscreenshare=false -Dxwayland=false \
        -Dshell-desktop=false -Dshell-fullscreen=false -Dshell-ivi=false -Dshell-kiosk=false \
        -Dcolor-management-lcms=false -Dimage-jpeg=false -Dimage-webp=false -Dsystemd=false \
        '-Dtools=[]' '-Dsimple-clients=[]' -Ddemo-clients=false -Dtests=false -Ddoc=false
fi
ninja -C "$wb" -j "$jobs" libweston/renderer-gl/gl-renderer.so libweston/libweston-14.so.0.0.2 libweston/backend-wayland/wayland-backend.so
install -m755 "$wb/libweston/backend-wayland/wayland-backend.so" "$out/"
install -m755 "$wb/libweston/renderer-gl/gl-renderer.so" "$out/"
install -m755 "$wb/libweston/libweston-14.so.0.0.2" "$out/libweston-14.so.0"

src="$root/src"
cc=("$ndk/clang-18" -O1 -g "-ffile-prefix-map=$root=." -fno-builtin -fPIC)
api21=(--target=x86_64-linux-android21)
api29=(--target=x86_64-linux-android29)
obj="$work/android"
mkdir -p "$obj"
"${cc[@]}" "${api29[@]}" -fno-emulated-tls -c "$src/loader_so.c" -o "$obj/loader.o"
"${cc[@]}" "${api21[@]}" -I "$root/include/android_stub" -c "$src/native-window.c" -o "$obj/window.o"
for unit in egl_shim egl_image egl_present_angle; do
    "${cc[@]}" "${api21[@]}" -I "$root/include" -c "$src/$unit.c" -o "$obj/$unit.o"
done
"${cc[@]}" "${api29[@]}" -shared -Wl,-z,defs,-soname,libEGL_d3d12.so \
    "$obj"/{loader,window,egl_shim,egl_image,egl_present_angle}.o -lm -llog -o "$out/libEGL_angle.so"
"${cc[@]}" "${api21[@]}" -c "$src/gles_shim.c" -o "$obj/gles.o"
"${cc[@]}" "${api21[@]}" -c "$src/gles_stubs.S" -o "$obj/stubs.o"
"${cc[@]}" "${api21[@]}" --driver-mode=g++ -std=c++17 -fno-exceptions -fno-rtti -c "$src/platform.cpp" -o "$obj/platform.o"
python3 "$src/gles1.py" "$ndk" "$obj"
"${cc[@]}" "${api21[@]}" -shared -Wl,-z,defs,-soname,libGLESv2_d3d12.so @"$obj/aliases.rsp" \
    "$obj/gles.o" "$obj/stubs.o" "$obj/platform.o" "$obj/fixed.c" -o "$out/libGLESv2_angle.so"
# The HAL's platform filename and its Android ABI name are independent.
hwc_soname=$(<"$src/hardware/hwcomposer.soname")
"${cc[@]}" "${api21[@]}" -shared -O2 -I "$src" -I "$root/include" -Wl,-z,defs,-soname,"$hwc_soname" \
    "$src/adapter.c" "$src/wd-client.c" "$src/core.c" -ldl -llog -o "$out/hwcomposer.waydroid.so"

host=(cc -std=gnu11 -O2 -Wall "-L$root/host/egl" -Wl,-rpath,'$ORIGIN/../host/egl')
"${host[@]}" "$src/consumer.c" -l:libEGL.so.1 -lcrypto -o "$out/texture-consumer"
# Build only the official portable C and x86-64 runtime-dispatched implementations.
# The crate is a checksummed source tarball; neither Rust nor a runtime .so is needed.
b3key=$(sha256sum "$root/vendor/blake3-1.8.2.crate" | cut -c1-12)
b3="$work/blake3-$b3key"
if [[ ! -f $b3/c/blake3.h ]]; then
    tmp=$(mktemp -d "$work/.blake3.XXXXXXXX")
    tar -xzf "$root/vendor/blake3-1.8.2.crate" --strip-components=1 -C "$tmp" blake3-1.8.2/c
    mv -T "$tmp" "$b3"
fi
b3src=("$b3"/c/{blake3,blake3_portable,blake3_dispatch}.c
       "$b3"/c/blake3_{sse2,sse41,avx2,avx512}_x86-64_unix.S)
"${host[@]}" -I"$b3/c" "$src/broker.c" "${b3src[@]}" -l:libEGL.so.1 -l:libsystemd.so.0 -o "$out/input-broker"
cc -O2 -Wall -Wextra -Werror "$src/virtual-touch.c" -o "$out/virtual-touch"
# x86_64 Android PNG encoder for ADB. Only build the compressor and checksums.
png_vendor=$(mktemp -d "$work/.png.XXXXXXXX")
(
    trap 'rm -rf -- "$png_vendor"' EXIT
    tar -xzf "$root/vendor/libdeflate-1.23.tar.gz" -C "$png_vendor"
    l="$png_vendor/libdeflate-1.23"
    "$ndk/clang-18" "${api29[@]}" -std=c11 -O3 -Wall -Wextra -Werror -I"$l" \
        -c "$src/adb-png.c" -o "$png_vendor/adb-png.o"
    "$ndk/clang-18" "${api29[@]}" -std=c11 -O3 -I"$l" "$png_vendor/adb-png.o" "$l"/lib/{deflate_compress,zlib_compress,adler32,crc32,utils,x86/cpu_features}.c \
        -o "$out/waydroid-acc-png"
)
(cd "$out" && sha256sum lib*so* wayland-backend.so gl-renderer.so hwcomposer.waydroid.so texture-consumer input-broker virtual-touch waydroid-acc-png > SHA256SUMS)
if [[ ${1:-} == --offscreen ]]; then
    cc -O2 -I"$src" "$root/validation/check.c" -L"$root/host/egl" -l:libEGL.so.1 -ldl -o "$work/check"
    ln -sfn "$out/$gallium" "$work/d3d12_dri.so"
    LD_PRELOAD="$out/$gallium" LD_LIBRARY_PATH="$root/host/egl:/usr/lib/wsl/lib" GALLIUM_DRIVER=d3d12 \
        MESA_LOADER_DRIVER_OVERRIDE=d3d12 LIBGL_DRIVERS_PATH="$work" "$work/check"
fi
