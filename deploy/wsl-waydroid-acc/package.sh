#!/usr/bin/env bash
# Maintainer command: rebuild the distributable, never from out/ or a live system.
set -euo pipefail
here=$(cd -P "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cd "$root"
for dir in runtime host; do (cd "$dir" && sha256sum --strict --quiet -c SHA256SUMS); done
name=wsl-waydroid-acc-prebuilts-2026-11-22.tar.gz
tmp=$(mktemp -d "$here/.pack.XXXXXXXX")
trap 'rm -rf -- "$tmp"' EXIT
# Package only manifested payloads, not local files accidentally left in runtime/.
{
    for dir in runtime host; do
        echo "$dir/SHA256SUMS"
        awk -v d="$dir" '{print d "/" $2}' "$dir/SHA256SUMS"
    done
    printf '%s\n' LICENSE THIRD_PARTY_NOTICES.md vendor/libdeflate-COPYING
    find licenses -type f -print
} | LC_ALL=C sort -u >"$tmp/files"
# Installation sets executable permissions explicitly. Include only file entries;
# tar creates parent directories during extraction.
tar --format=ustar --mtime=@0 --owner=0 --group=0 --numeric-owner --mode=0644 \
    --no-recursion -cf - -T "$tmp/files" | gzip -n -9 >"$tmp/$name"
(cd "$tmp" && sha256sum "$name" >SHA256SUMS)
mv -fT "$tmp/$name" "$here/$name"
mv -fT "$tmp/SHA256SUMS" "$here/SHA256SUMS"
