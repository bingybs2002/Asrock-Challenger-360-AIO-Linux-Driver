# Challenger cooler display on Linux

Shows **CPU temperature + fan RPM** on the cooler's display (`26ce:0a13` / `1a2c:4c86`), updated every second.
Tested on Fedora 44, firmware 0x0023, Ryzen (k10temp) on a Gigabyte B650 Gaming X AX V2 (IT8689E fan chip).

| File | Purpose |
|---|---|
| `challenger.c` + `Makefile` | The daemon: reads hwmon sensors, writes HID feature reports to `/dev/hidraw` (no dependencies) |
| `challenger-display.service` | systemd unit, runs it as root at boot |
| `it87.conf` | modprobe option so the fan chip driver loads on Gigabyte boards |
| `install.sh` / `uninstall.sh` | Build + install / remove everything |
| `challenger.py` | Python reference / experiment tool for the full protocol (all fields, all layouts) |
| `PROTOCOL.md` | Reverse-engineering notes |

## Install

```sh
sh install.sh      # needs sudo; builds, loads it87, installs + enables the service
```

Check / stop / remove:

```sh
systemctl status challenger-display
journalctl -u challenger-display
sh uninstall.sh
```

## Manual use

```sh
make
sudo ./challenger --dry-run     # show current sensor values and the packets
sudo ./challenger --version     # firmware version
sudo ./challenger               # stream every second (Ctrl+C to stop)
```

Options: `--temp-sensor CHIP/ATTR` (default `k10temp/temp1`; Intel: `coretemp/temp1`),
`--fan-sensor CHIP/ATTR` (default `it8689/fan1` = CPU_FAN header), `--fahrenheit`.
List what your machine has: `grep . /sys/class/hwmon/hwmon*/name`, then look for `fan*_input` / `temp*_input`.
To change what the service uses, edit `ExecStart` in `challenger-display.service` and re-run `install.sh`.

## What the display can show (fw 0x0023)

The packet-4 mode byte selects the layout:

| Mode byte | Big number | Small number |
|---|---|---|
| 2 (also 3, 5, 6) | fan RPM | CPU temp |
| 4 | — | CPU temp only |
| 1 | CPU clock (untested) | |

No layout shows GPU temp; the Windows app cycles layouts over time.

## Gotchas

* **Fan RPM is 0 / `it8689/fan1 not found`:** `it87` isn't loaded. Gigabyte ACPI claims the chip's ports, so it needs
  `ignore_resource_conflict=1` (that's what `it87.conf` sets). The small theoretical risk: ACPI firmware and the driver
  can both touch the chip.
* **Permission denied running the tools manually:** `/dev/hidraw*` is root-only, so use `sudo`. The service runs as root.
* **Stop the service before experimenting** with `challenger.py` (`sudo systemctl stop challenger-display`), or it
  overwrites your frames within a second.
