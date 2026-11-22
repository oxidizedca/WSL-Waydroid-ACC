#!/usr/bin/env bash
# Optional standalone deployment; never invoked by the graphics installer.
set -euo pipefail
export LC_ALL=C
here=$(cd -P -- "$(dirname -- "$0")" && pwd)
mode=${1:-verify}
usage(){ echo "Usage: bash deploy.sh {verify|check|install|restore BACKUP_DIR}"; }
case $mode in
    -h|--help) usage; exit 0;;
    verify|check|install) (( $# <= 1 )) || { usage; exit 2; };;
    restore) (( $# == 2 )) || { usage; exit 2; };;
    *) usage; exit 2;;
esac
die(){ echo "Digitalis: $*" >&2; exit 1; }
umask 077
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
tarball=digitalis-prebuilts-2026-11-22.tar.gz
(cd "$here"; sha256sum --strict --quiet -c SHA256SUMS)
# Pin the archive independently of the checksum file before extracting as root.
echo "a0effa8dd91d58aa4f96310d44c3fda9f360e7bc7d9fe9736c7a3cdf117d54c0  $here/$tarball" | sha256sum --strict --quiet -c -
tar --no-same-owner -xzf "$here/$tarball" -C "$tmp"
pkg=$tmp/digitalis-prebuilts
(cd "$pkg"; sha256sum --strict --quiet -c SHA256SUMS)
# Distribution notices are verified inside the archive, never added to the overlay/backup list.
(cd "$pkg"; sha256sum -- Digitalis-LICENSE THIRD-PARTY-NOTICES.txt NOTICE-MAP.tsv | cmp -s - NOTICE.sha256) ||
    die "Bundled license/notice verification failed."
patches=(patches/0001-cc1.patch patches/0002-short-leaf.patch)
(cd "$pkg"; sha256sum -- "${patches[@]}" | cmp -s - PATCHES.sha256) ||
    die "Bundled source patch verification failed."
mapfile -t files < <(awk '{print $2}' "$pkg/SHA256SUMS")
(( ${#files[@]} == 74 )) || die "Wrong payload count"
if [[ $mode == verify ]]; then echo "Verified archive, 74 Digitalis payloads, 3 bundled license/notice files and 2 source patches."; exit; fi
(( EUID == 0 )) || die "Run with sudo in the target WSL distro."
base=/var/lib/waydroid
lower=$base/overlay
store=$base/digitalis-backups
ndk=libndk_translation.so
dig=libberberis_arm64.so
rc=system/etc/init/ndk_translation.rc
prop=vendor/build.prop
mumu=system/lib64/arm64/libnativebridge.so
printf '%s\n' '# Disabled while Digitalis owns the ARM64 binfmt handlers.' > "$tmp/ndk-disabled"
files+=("$rc" "$prop")
a(){ lxc-attach -P "$base/lxc" -n waydroid -- "$@"; }
live_root(){
    local p
    p=$(lxc-info -P "$base/lxc" -n waydroid -pH)
    [[ $p =~ ^[1-9][0-9]*$ && -d /proc/$p/root/system ]] || die "Waydroid must be running"
    live=/proc/$p/root
}
target(){
    [[ $(a getprop ro.product.device) == waydroid_x86_64 &&
       $(a getprop ro.build.version.sdk) == 36 && $(a getprop ro.build.id) == BP4A.251205.006 &&
       $(a getprop ro.dalvik.vm.isa.arm64) == x86_64 && $(a getprop ro.enable.native.bridge.exec) == 1 &&
       ,$(a getprop ro.product.cpu.abilist64), == *,arm64-v8a,* ]] || die "Untested image / ARM64 configuration."
    [[ ! -e $live/data/adb/modules/waydroid_digitalis_arm64 && ! -e $live/data/adb/modules_update/waydroid_digitalis_arm64 ]] || die "Remove the Digitalis KernelSU module and reboot first."
}
handlers(){ python3 "$here/binfmt.py" "$live/proc/sys/fs/binfmt_misc" "$pkg/system/etc/binfmt_misc" "$@"; }
bridge_mapped(){
    local z
    z=$(a pidof zygote64) && [[ $z =~ ^[0-9]+$ ]] && grep -Fq "/system/lib64/$1" "$live/proc/$z/maps"
}
mumu_hash(){
    if [[ -f $live/$mumu ]]; then sha256sum "$live/$mumu" | cut -d' ' -f1
    elif [[ ! -e $live/$mumu ]]; then echo absent; else return 1; fi
}
matches(){
    (cd "$lower" && sha256sum --strict --quiet -c "$pkg/SHA256SUMS") &&
    (cd "$live" && sha256sum --strict --quiet -c "$pkg/SHA256SUMS") &&
    cmp -s "$tmp/ndk-disabled" "$lower/$rc" && cmp -s "$lower/$rc" "$live/$rc" &&
    grep -Fxq "ro.dalvik.vm.native.bridge=$dig" "$lower/$prop" &&
    [[ $(a getprop ro.dalvik.vm.native.bridge) == "$dig" && $(handlers) == 'digitalis digitalis' ]] &&
    bridge_mapped "$dig"
}
if [[ $mode == check ]]; then
    live_root; target; matches || die "Live overlay, bridge or ARM64 handler mismatch."
    echo "OK: overlay + live payload (74), zygote64, two Digitalis handlers; optional guest libnativebridge: $(mumu_hash)"
    exit
fi
# Share the graphics deploy lock. Do not race a separate manual launch.
exec 9>/run/waydroid-acc-deploy.lock; flock -n 9 || die "Another deployment is running."
launcher=/usr/local/sbin/waydroid-acc-start
[[ -x $launcher && -d $lower && $(realpath "$lower") == "$lower" ]] || die "Configured Waydroid-ACC installation required."
safe_paths(){
    local f
    for f in "${files[@]}"; do
        [[ $(realpath -m "$lower/$f") == "$lower/$f" && ! -L $lower/$f ]] || die "Unexpected symlink: $f"
        [[ ! -e $lower/$f || -f $lower/$f ]] || die "Unexpected file type: $f"
    done
}
safe_paths
if [[ $mode == install ]]; then
    live_root; target
    if matches >/dev/null 2>&1; then echo "Already deployed and verified; no restart."; exit; fi
    bridge=$(a getprop ro.dalvik.vm.native.bridge)
    [[ $bridge == "$dig" || $bridge == "$ndk" ]] || die "Unknown current bridge: $bridge"
    [[ $(grep -c '^ro.dalvik.vm.native.bridge=' "$live/$prop") == 1 ]] &&
        grep -Fxq "ro.dalvik.vm.native.bridge=$bridge" "$live/$prop" || die "Unexpected vendor/build.prop"
    if ! cmp -s "$live/$rc" "$tmp/ndk-disabled"; then
        echo "d1c797cb40b5366b155d8c187f44a75d736f633dae54b41f0b19b5c10f70bd95  $live/$rc" | sha256sum --quiet -c - || die "Unknown NDK init file"
    fi
    for f in "${files[@]}"; do
        if [[ $f == vendor/* ]]; then upper=$base/overlay_rw/vendor/${f#vendor/}; else upper=$base/overlay_rw/system/$f; fi
        [[ ! -e $upper && ! -L $upper ]] || die "Writable overlay shadows $f; inspect before installing."
    done
    cp --dereference --preserve=all "$live/$prop" "$tmp/vendor.prop"
    sed "s/^ro\.dalvik\.vm\.native\.bridge=.*/ro.dalvik.vm.native.bridge=$dig/" "$live/$prop" > "$tmp/vendor.prop"
    install -d -m700 "$store"; backup=$(mktemp -d "$store/before-XXXXXXXX")
    previous=$(handlers); printf '%s\n' "$previous" > "$backup/handlers"
    printf '%s\n' "$bridge" > "$backup/bridge"; mumu_hash > "$backup/mumu"
    if [[ -f /run/waydroid-acc-window.env ]]; then cp /run/waydroid-acc-window.env "$backup/window"; else : > "$backup/window"; fi
    : > "$backup/present"; : > "$backup/absent"
    for f in "${files[@]}"; do
        if [[ -f $lower/$f ]]; then echo "$f" >> "$backup/present"; else echo "$f" >> "$backup/absent"; fi
    done
    tar --numeric-owner --acls --xattrs --xattrs-include='*' --selinux -C "$lower" -cpf "$backup/overlay.tar" -T "$backup/present"
    (cd "$backup"; sha256sum overlay.tar present absent bridge handlers mumu window > SHA256SUMS)
else
    backup=$(realpath -e -- "$2") || die "No such backup"
    [[ $backup == "$store"/before-* && ${backup%/*} == "$store" && $(stat -c '%u:%a' "$backup") == 0:700 ]] || die "Expected a root-owned Digitalis backup under $store."
fi
(cd "$backup"; sha256sum --strict --quiet -c SHA256SUMS)
cmp <(printf '%s\n' "${files[@]}" | sort) <(cat "$backup/present" "$backup/absent" | sort) || die "Wrong backup file list."
win=$(sed -n 's/^ACC_WINDOW_MODE=//p' "$backup/window"); win=${win:-free}
width=$(sed -n 's/^ACC_WIDTH=//p' "$backup/window"); height=$(sed -n 's/^ACC_HEIGHT=//p' "$backup/window")
width=${width:-1920}; height=${height:-1080}
for axis in "$width" "$height"; do
    [[ $axis =~ ^[1-9][0-9]{0,3}$ ]] && (( axis >= 128 && axis <= 8192 )) || die "Unexpected window resolution"
done
args=(-r "${width}x${height}"); case $win in fix|ratio) args+=(-w "$win");; free) :;; *) die "Unexpected window mode";; esac
mouse=$(sed -n 's/^ACC_MOUSE_MODE=//p' "$backup/window")
case ${mouse:-pointer} in
    pointer|touch) args+=(-m "${mouse:-pointer}");;
    *) die "Unexpected mouse mode";;
esac
echo "Backup: $backup"
# If startup fails this backup can be restored even with the container stopped.
echo "Recovery: sudo bash '$here/deploy.sh' restore '$backup'"
systemctl stop waydroid-container
if [[ $mode == install ]]; then
    for f in "${files[@]}"; do
        case $f in
            "$prop") [[ -d $lower/vendor ]] || install -d -m755 "$lower/vendor"; cp --preserve=all "$tmp/vendor.prop" "$lower/$prop";;
            "$rc") install -D -m644 "$tmp/ndk-disabled" "$lower/$rc";;
            system/bin/*) install -D -m755 "$pkg/$f" "$lower/$f";;
            *) install -D -m644 "$pkg/$f" "$lower/$f";;
        esac
    done
else
    while IFS= read -r f; do rm -f -- "$lower/$f"; done < "$backup/absent"
    tar --numeric-owner --acls --xattrs --xattrs-include='*' --selinux -C "$lower" -xpf "$backup/overlay.tar"
fi
"$launcher" "${args[@]}" 9>&-
live_root
if [[ $mode == install ]]; then
    handlers digitalis digitalis
    matches || die "Post-install check failed; use the recovery command above."
else
    read -r dyn exe < "$backup/handlers"; handlers "$dyn" "$exe"
    bridge=$(<"$backup/bridge")
    [[ $(a getprop ro.dalvik.vm.native.bridge) == "$bridge" ]] && bridge_mapped "$bridge" || die "Restored bridge not active"
fi
[[ $(mumu_hash) == "$(<"$backup/mumu")" ]] || die "Optional guest library changed; inspect mounts."
echo "Digitalis $mode verified."
