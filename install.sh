#!/bin/sh
# Build and install the Challenger display service (Fedora). Run from this folder: sh install.sh
set -e
cd "$(dirname "$0")"
make

# fan sensors: load it87 at boot
sudo install -m 644 it87.conf /etc/modprobe.d/it87.conf
echo it87 | sudo tee /etc/modules-load.d/it87.conf >/dev/null
sudo modprobe -r it87 2>/dev/null || true
sudo modprobe it87

sudo install -m 755 challenger /usr/local/bin/challenger
sudo install -m 644 challenger-display.service /etc/systemd/system/challenger-display.service
sudo systemctl daemon-reload
sudo systemctl enable challenger-display.service
sudo systemctl restart challenger-display.service
systemctl --no-pager status challenger-display.service | head -5
