#!/usr/bin/env bash
set -euo pipefail
work=$1 out=$2 data=$3
packages="$data/../../system/packages.list"
install -m755 "$out/input-broker" "$work/bin/.input.new"
mv -fT "$work/bin/.input.new" "$work/bin/input-broker"
sed -e 's/^RestartSec=.*/RestartSec=0.2/' -e 's/^Description=.*/Description=Waydroid layer input texture broker/' \
    -e "s|^ExecStart=.*|ExecStart=$work/bin/input-broker $data/waydroid-acc/input $packages|" \
    /etc/systemd/system/waydroid-acc-consumer.service > /etc/systemd/system/waydroid-acc-input.service
sed -i '/^\[Service\]/a NotifyAccess=main\nFileDescriptorStoreMax=80' /etc/systemd/system/waydroid-acc-input.service
systemctl daemon-reload
systemctl enable waydroid-acc-input
systemctl restart waydroid-acc-input
for ((i=0;i<50;i++)); do [[ -S "$data/waydroid-acc/input" ]] && break; sleep .1; done
systemctl is-active --quiet waydroid-acc-input && test -S "$data/waydroid-acc/input"
