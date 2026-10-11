#!/bin/sh
sudo systemctl disable --now challenger-display.service
sudo rm -f /etc/systemd/system/challenger-display.service /usr/local/bin/challenger \
    /etc/modprobe.d/it87.conf /etc/modules-load.d/it87.conf
sudo systemctl daemon-reload
