#!/usr/bin/env bash
# Install into an already configured Waydroid and WSLg system.
set -euo pipefail
root=$(cd -P "$(dirname "$0")" && pwd)
mode=${1:-apply}
case "$mode" in
    apply|restore) source="$root/runtime"; services="$source/services" ;;
    built) source="$root/out"; services="$root/services" ;;
    *) echo 'Usage: bash deploy.sh [apply|built|restore]' >&2; exit 2 ;;
esac
[[ $EUID == 0 ]] || { echo 'Run as WSL root' >&2; exit 1; }
exec 9>/run/waydroid-acc-deploy.lock
flock -n 9
prefix=/usr/local/lib/wsl-waydroid-acc
current="$prefix/current"
launcher=/usr/local/sbin/waydroid-acc-start
launcher_target="$current/bin/${launcher##*/}"
state=/var/lib/waydroid/wsl-waydroid-acc
config=/etc/tmpfiles.d/waydroid-acc.conf
marker=/run/waydroid-acc-query-on
units=/etc/systemd/system
input=waydroid-acc-input.service
consumer=waydroid-acc-consumer.service
weston=waydroid-acc-weston.service
hook=waydroid-container.service.d/graphics-acc.conf
for bundle in "$root/host" "$root/runtime"; do (cd "$bundle" && sha256sum --strict --quiet -c SHA256SUMS); done
if [[ $mode == built ]]; then (cd "$source" && sha256sum --strict --quiet -c SHA256SUMS); fi

# The existing Android overlay supplies the stock vendor HAL, platform libraries
# and D3D12/UMD. Preserve allocation data, Android ownership and SELinux xattrs.
# This host installation has one WSL session user, specified by the unit templates.
account=$(sed -n 's/^User=//p' "$services/systemd/$input")
userdir=$(getent passwd "$account" | cut -d: -f6)
[[ -n $account && -n $userdir ]]
overlay=/var/lib/waydroid/overlay_rw
android_files=("$overlay/system/system/lib64/libEGL_angle.so" "$userdir/.local/share/waydroid/data/local/tmp/mesa/libgallium-26.0.8.so"
               "$overlay/system/system/lib64/libGLESv2_angle.so" "$overlay/vendor/lib64/hw/hwcomposer.waydroid.so")
for path in "${android_files[@]}"; do
    name=${path##*/}
    test -f "$path"
    if ! cmp -s "$path" "$root/runtime/$name"; then
        checksum=$(cd "${path%/*}" && sha256sum "$name")
        grep -Fxq -- "$checksum" "$state/installed.sha256"
    fi
done
[[ ! -e $current || -L $current ]]
if [[ -e $launcher || -L $launcher ]]; then
    [[ -L $launcher && $(readlink "$launcher") == "$launcher_target" ]] || {
        echo "Refusing to replace unrelated $launcher" >&2; exit 1
    }
fi
install -d -m755 "$prefix/releases" /usr/local/sbin
install -d -m700 "$state"
release=$(mktemp -d "$prefix/releases/$mode.XXXXXXXX")
backup=$(mktemp -d "$state/deploy.XXXXXXXX")
echo "Runtime $release; backup $backup"
chmod 755 "$release"
install -d "$release"/{bin,weston,host/egl,host/gallium/targets/dri}
for name in texture-consumer input-broker virtual-touch; do install -m755 "$source/$name" "$release/bin/$name"; done
install -m755 "$services/"{waydroid-acc-wait-weston,waydroid-acc-start,waydroid-acc-touch-node} "$release/bin/"
install -m644 "$root/host/egl/libEGL.so.1" "$release/host/egl/"
driver=libgallium-26.0.8.so
dri="$release/host/gallium/targets/dri"
install -m644 "$root/host/gallium/targets/dri/$driver" "$dri/"
ln -s "$driver" "$dri/d3d12_dri.so"
weston_modules=(libweston-14.so.0 gl-renderer.so wayland-backend.so)
for name in "${weston_modules[@]}"; do install -m644 "$source/$name" "$release/weston/$name"; done
install -m644 "$services/weston.ini" "$release/weston/weston.ini"
socket_dir="$userdir/.local/share/waydroid/data/local/tmp/waydroid-acc"
vendor_hal="$overlay/vendor/lib64/hw/hwcomposer.vendor-base.so"
video_init="$overlay/system/system/etc/init/waydroid-acc-video.rc"
[[ $(sha256sum "$vendor_hal" | cut -d' ' -f1) == 942fad47d8372ea1eb6873a8ccad0954d91592b0fc6137d8eff2f638043f1a29 ]]
if [[ -f $video_init ]]; then cmp "$video_init" "$services/decoder-output.rc"; fi
# A PATH shim for bare "screencap -p". Never replace the platform executable.
capdir="$overlay/system/system/system_ext/bin"
cap="$capdir/screencap"
cap_src="$services/screencap"
png="$capdir/waydroid-acc-png"
for dir in "$capdir" /var/lib/waydroid/{rootfs,overlay}/system/system_ext/bin; do
    for name in screencap waydroid-acc-png; do
        path="$dir/$name"
        [[ -e $path || -L $path ]] || continue
        if [[ ! -L $path && -f $path ]]; then
            sum=$(cd "$dir" && sha256sum "$name")
            grep -Fxq -- "$sum" "$state/installed.sha256" && continue
            # First upgrade from the verified 256 KiB buffered-only wrapper.
            [[ $sum == "5f1e47af12f6af9b3fbf9597c72abbef4055932715629af25b4c3931a10344b7  screencap" ]] && continue
            [[ $name == screencap ]] && cmp -s "$cap_src" "$path" && continue
        fi
        echo "Refusing to overwrite unrelated $path" >&2; exit 1
    done
done
active_units=()
for unit in "$input" "$consumer" "$weston"; do
    if systemctl is-active --quiet "$unit"; then active_units+=("$unit"); fi
done
paths=("${android_files[@]}" "$units/$input" "$units/$consumer" "$units/$weston" "$units/$hook" "$units/waydroid-acc-touch.service"
       "$units/multi-user.target.wants/$input" "$units/multi-user.target.wants/$consumer"
       "$units/multi-user.target.wants/waydroid-container.service"
       "$current" "$launcher" "$config" "$marker" "$video_init" "$state/installed.sha256" "$cap" "$png")
for i in "${!paths[@]}"; do
    path=${paths[i]}
    printf '%s\n' "$path" >>"$backup/paths"
    if [[ -e $path || -L $path ]]; then cp -aT "$path" "$backup/$i"; fi
done
# A same-filesystem rename keeps every installed file readable during replacement.
replace(){
    local from=$1 to=$2 tmp
    tmp=$(mktemp -d "${to%/*}/.wd-acc.XXXXXXXX") || return 1
    cp -aT "$from" "$tmp/new" && mv -fT "$tmp/new" "$to" && rmdir "$tmp"
}
install_android(){
    local to=$1 from tmp
    from="$source/${to##*/}"
    cmp -s "$from" "$to" && return 0
    tmp=$(mktemp -d "${to%/*}/.wd-acc.XXXXXXXX")
    cp --preserve=all "$to" "$tmp/new"
    cat "$from" > "$tmp/new"
    cmp "$from" "$tmp/new"
    mv -fT "$tmp/new" "$to"
    rmdir "$tmp"
}
recover(){
    local status=$?
    trap - EXIT
    if ((status)); then
        echo "Restoring previous deployment from $backup" >&2
        set +e
        systemctl stop waydroid-container
        systemctl stop "$input" "$consumer" "$weston" 2>/dev/null
        for i in "${!paths[@]}"; do
            if [[ -e $backup/$i || -L $backup/$i ]]; then replace "$backup/$i" "${paths[i]}"
            else rm -f -- "${paths[i]}"; fi
        done
        systemctl daemon-reload
        if ((${#active_units[@]})); then systemctl start "${active_units[@]}"; fi
        if [[ -x $launcher ]]; then "$launcher" 9>&-; else systemctl start waydroid-container; fi
    fi
    exit "$status"
}
trap recover EXIT
systemctl stop waydroid-container
if ((${#active_units[@]})); then systemctl stop "${active_units[@]}"; fi
install -d -o "$account" -g "$(id -gn "$account")" -m0711 "$socket_dir"
if [[ ! -f $video_init ]]; then install -m644 "$services/decoder-output.rc" "$video_init"; fi
# Match the image's root:shell 0751 bin directory when copying it up.
if [[ ! -d $capdir ]]; then install -d -o 0 -g 2000 -m0751 "$capdir"; fi
install -o 0 -g 2000 -m0755 "$cap_src" "$backup/new-screencap"
install -o 0 -g 2000 -m0755 "$source/waydroid-acc-png" "$backup/new-png"
replace "$backup/new-png" "$png"
replace "$backup/new-screencap" "$cap"
for path in "${android_files[@]}"; do install_android "$path"; done
ln -s "$release" "$backup/new-current"
ln -s "$launcher_target" "$backup/new-launcher"
replace "$backup/new-current" "$current"
replace "$backup/new-launcher" "$launcher"
for file in "$input" "$consumer" "$weston" "$hook" waydroid-acc-touch.service; do
    install -D -m644 "$services/systemd/$file" "$backup/new-units/$file"
    replace "$backup/new-units/$file" "$units/$file"
done
install -m644 "$services/query.conf" "$config"
systemd-tmpfiles --create "$config"
test -f "$marker"
systemctl daemon-reload
systemctl enable "$input" "$consumer"
systemctl restart "$input" "$consumer"
"$launcher" 9>&-

for name in "$input" "$consumer" "$weston"; do systemctl is-active --quiet "$name"; done
for spec in "$input:input-broker" "$consumer:texture-consumer"; do
    pid=$(systemctl show "${spec%:*}" -p MainPID --value)
    [[ $(readlink -e "/proc/$pid/exe") == "$release/bin/${spec#*:}" ]]
    for lib in host/egl/libEGL.so.1 "host/gallium/targets/dri/$driver"; do grep -qF "$release/$lib" "/proc/$pid/maps"; done
done
pid=$(systemctl show "$weston" -p MainPID --value)
for lib in "${weston_modules[@]}"; do grep -qF "$release/weston/$lib" "/proc/$pid/maps"; done
test -S "$socket_dir/input"
test -S "$socket_dir/frames"
sf=$(pgrep -xo surfaceflinger)
hwc=$(pgrep -xo composer@2.1-se)
# lxc-attach inherits the WSL PATH, not the PATH set by adbd.
lxc-attach -P /var/lib/waydroid/lxc -n waydroid -- /system/bin/test -x /system_ext/bin/screencap
cmp "$cap_src" "/var/lib/waydroid/rootfs/system/system_ext/bin/screencap"
cmp "$source/waydroid-acc-png" "/var/lib/waydroid/rootfs/system/system_ext/bin/waydroid-acc-png"
grep -qF "/vendor/lib64/hw/${vendor_hal##*/}" "/proc/$hwc/maps"
for name in libEGL_angle.so libGLESv2_angle.so; do cmp "$source/$name" "/proc/$sf/root/system/lib64/$name"; done
cmp "$source/$driver" "/proc/$sf/root/data/local/tmp/mesa/$driver"
cmp "$source/hwcomposer.waydroid.so" "/proc/$hwc/root/vendor/lib64/hw/hwcomposer.waydroid.so"
for path in "${android_files[@]}"; do (cd "${path%/*}" && sha256sum "${path##*/}"); done >"$backup/new-hashes"
(cd "$capdir" && sha256sum screencap waydroid-acc-png) >>"$backup/new-hashes"
replace "$backup/new-hashes" "$state/installed.sha256"
# The packaged container is WantedBy=multi-user.target. Keep the verified
# session running, but let the launcher start the container on demand next boot.
systemctl disable waydroid-container.service
trap - EXIT
echo "Installed and booted $release"
