#!/usr/bin/env bash
# Built project + local installation. Private headless output only, no Android session.
set -euo pipefail
umask 077
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
runtime=${ACC_RUNTIME:-/usr/local/lib/wsl-waydroid-acc/current}
driver="$root/out/libgallium-26.0.8.so"
test -r "$driver" && test -r "$runtime/weston/gl-renderer.so" || { echo 'Build and install the tested ACC stack first.' >&2; exit 1; }
work=$(mktemp -d); wp=
cleanup() {
    if [[ -n "$wp" ]]; then kill "$wp" 2>/dev/null || :; wait "$wp" || :; fi
    rm -rf -- "$work"
}
trap cleanup EXIT
cd -- "$work"
mkdir -m700 run
export XDG_RUNTIME_DIR="$work/run" GALLIUM_DRIVER=d3d12 MESA_LOADER_DRIVER_OVERRIDE=d3d12
gen() { wayland-scanner client-header "$2" "$1.h"; wayland-scanner private-code "$2" "$1.c"; }
gen xdg /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
gen viewporter /usr/share/wayland-protocols/stable/viewporter/viewporter.xml
gen capture /usr/share/libweston-14/protocols/weston-output-capture.xml
cc -Wall -O2 -I"$root/src" -I. "$here/replay.c" viewporter.c xdg.c capture.c "$root/src/wd-client.c" \
    -L"$runtime/host/egl" -l:libEGL.so.1 -lwayland-client -ldl -o replay
ln -s "$driver" d3d12_dri.so
ulimit -c 0
for mode in separate-surface reuse-anchor replace-visible; do
    LD_LIBRARY_PATH="$runtime/weston:/usr/lib/wsl/lib" WESTON_MODULE_MAP="gl-renderer.so=$runtime/weston/gl-renderer.so" \
        weston --no-config --backend=headless --renderer=gl --socket=lab --debug --shell=kiosk-shell.so \
        --width=1920 --height=1080 --idle-time=0 --log="$work/$mode.weston" & wp=$!
    for ((i=0;i<100;i++)); do [[ ! -S "$XDG_RUNTIME_DIR/lab" ]] || break; sleep 0.04; done
    [[ -S "$XDG_RUNTIME_DIR/lab" ]]
    REPLAY_SOCKET="$XDG_RUNTIME_DIR/lab" LD_PRELOAD="$driver" LD_LIBRARY_PATH="$runtime/host/egl:/usr/lib/wsl/lib" \
        LIBGL_DRIVERS_PATH="$work" timeout 15 ./replay "$mode" || { tail -20 "$work/$mode.weston" >&2; exit 1; }
    kill "$wp"; wait "$wp" || :; wp=
done
