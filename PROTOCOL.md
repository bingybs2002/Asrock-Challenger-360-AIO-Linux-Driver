# Challenger Driver — reverse-engineering notes

Target: `Challenger Driver/DeviceDriver.exe` v1.0.1.13 (PE32, MSVC/MFC, signed via GlobalSign EV).
PDB path: `E:\Workspace\drive\100\102C-(Challenger Driver)(…)\project\Challenger Driver-V1.0.1.13\bin\DeviceDriver.pdb`.
Vendor: Ocypus/TCOMAS ("display-100 / 102C" in update URLs). The device is a CPU cooler with a digital
display; the app reads PC sensors and streams them to the display over USB HID once per second.

All analysis was static (pefile + capstone); nothing was executed. Addresses are VAs (image base 0x400000).

## Files

| File | What it really is |
|---|---|
| `DeviceDriver.exe` | The app: sensor polling, HID protocol, updater, tray icon |
| `XYUI.dll` | Vendor's custom UI toolkit (2660 C++ exports: `XYUI::MDialog`, `CFanDevCtrl`, `Manager::SetCPUTemp`…) |
| `api-ms-win-core-sysinfo-847-32.dll` / `-64.dll` | **Renamed HWiNFO SDK DLL** (`HWiNFO32.DLL` 8.47, REALiX, UPX-packed). Not a Windows API set. Loaded via `LoadLibraryA` for sensor readings |
| `mfc140u/msvcp140/vcruntime140.dll` | Stock VC++ runtime |
| `language/*.lan` | UTF-16 `id=string` tables (1033 en, 2052 zh-CN, 1028 zh-TW) |
| `skins/theme1/*` | UI images and font |

## Device discovery

* Hot-plug: `WM_DEVICECHANGE` handler (`0x406134`) matches the device path against
  `VID_1A2C&PID_4C86` or `VID_26CE&PID_0A13`.
* Interface filter (`0x4062a0`): `HidP_GetCaps` → **UsagePage `0xFF01`, Usage `0x0001`**.
* Enumeration: `SetupDiGetClassDevsA(HID GUID)` → `CreateFileA` (`0x40e900`, `0x40ed30`).
* `hid.dll` exports resolved dynamically (`0x40e7f0`).

## Transport

Everything is **HID feature reports, report ID `0x07`**.

| Primitive | Addr | Call |
|---|---|---|
| SendToAll(pkt) | `0x40d9d0` | for each device: `HidD_SetFeature(h, buf, 0x41)` where `buf` = 64 bytes of packet + trailing 0x00 |
| SetFeature | `0x40eec0` | `HidD_SetFeature` wrapper |
| GetFeature | `0x40ef60` | `DeviceIoControl(h, 0xB0192 /*IOCTL_HID_GET_FEATURE*/, buf, 0x40, buf, 0x40)` overlapped |

### Firmware version query (`0x40daa0`)

1. `SetFeature([07 00 00 … 00], len 0x40)`
2. sleep 16 ms, `GetFeature` (64 B); retried once if `resp[0] != 0x07`
3. `version = resp[1] << 8 | resp[2]`

## Sensor frame (`0x40a840`, called once per second)

Five packets are sent, 10 ms apart. Byte 0 = report ID `0x07`, byte 1 = packet index, bytes 2..7 as
below, bytes 8..63 zero.

Numbers go over the wire as **decimal digits in nibbles**. Most bytes carry **GPU digit in the high
nibble and CPU digit in the low nibble**. Notation: `X.k/h/t/o` = thousands/hundreds/tens/ones digit of X.

Inputs (clamped by the app):

| Sym | Meaning | Range |
|---|---|---|
| `T` / `GT` | CPU / GPU temperature **×10** (so 45.3° → 453); °F if unit flag set (`x*1.8+320`) | 0–9999 |
| `U` / `GU` | CPU / GPU usage % | 0–100 |
| `P` / `GP` | CPU package / GPU power, W | 0–998 (**frame skipped entirely if ≥ 999**) |
| `C` / `GC` | CPU / GPU core clock, MHz | 0–9999 |
| `R` | Selected fan or pump RPM (user-selected from the fan list, otherwise "Pump Speed"/"Fan0") | 0–9999 |

| Pkt | b2 | b3 | b4 | b5 | b6 | b7 |
|---|---|---|---|---|---|---|
| `07 00` temps | `GT.k<<4 \| T.k` | `GT.h<<4 \| T.h` | `GT.t<<4 \| T.t` | `GT.o<<4 \| T.o` | unit (0=°C, 1=°F) | `GTlvl<<4 \| (Tlvl & 0xF)` |
| `07 01` usage | `GU.h<<4 \| U.h` | `GU.t<<4 \| U.t` | `GU.o<<4 \| U.o` | 0 | `GUlvl<<4 \| Ulvl` | 1 |
| `07 02` power | `GP.h<<4 \| P.h` | `GP.t<<4 \| P.t` | `GP.o<<4 \| P.o` | 0 | 0 | 0 |
| `07 03` clock | `GC.k<<4 \| C.k` | `GC.h<<4 \| C.h` | `GC.t<<4 \| C.t` | `GC.o<<4 \| C.o` | 0 | 0 |
| `07 04` fan | `R.k` | `R.h` | `R.t` | `R.o` (whole bytes) | mode | level |

Bar levels (probably the display's gauge segments):

* `GTlvl` = 0 if GT==0 else `min(GT/100 + 1, 10)`; `Ulvl`, `GUlvl` = 0 if 0 else `min(x/10 + 1, 10)`
* `Tlvl` = 1..20 in 5 °C steps (`T ≤ 50 → 1`, `≤ 100 → 2`, … `≤ 1000 → 19`, else 20), computed from the
  °C×10 value. Only the low nibble is sent in pkt 0, so values above 15 get truncated (looks like a vendor bug).
* Pkt 4 `mode/level` depends on a display-mode setting (`this+0x3c8`):
  `0 → (1, CPU-clock level)`, `1 → (4, Tlvl)`, other → `(2, RPM level)`. Both are 1..20 using uneven
  thresholds: clock MHz `500,1000,1200,…,2000,2100,2300,2500,3000,…,7000`; RPM `50,100,150,200,300,450,600,800,1000,…,3000`
  (exact tables in `challenger.py`).

## Sensor sources (HWiNFO thread, `0x4065e0`–`0x4082d7`)

Matched by label substring: CPU temp ← "CPU Package" / "CPU (Tctl/Tdie)"; power ← "cpu package"+"power";
clocks ← "core clocks"/"cpu clock", "gpu clock"; voltage ← "core voltage"/"vcore"; usage ← "CPU Utilization"/"CPU Total",
"GPU Utilization"/"GPU D3D Usage"; fan ← "Pump Speed", "Chassis Fan N", "CHA_FANn", "System N", "AIO Pump".
A discrete GPU (name contains NVIDIA/GeForce/Vega/…) is preferred over the iGPU. Results are also published via a shared-memory
mapping named `xinyao_hardware_mapfile` (`0x404b20`).

## Other behaviour worth knowing

* **Auto-update without signature check.** Downloads `Software-<ver>.zip` / `FirmwareUpdateTool-<ver>.zip` from
  `https://upgrade-1318931438.cos.ap-beijing.myqcloud.com/display-100/102C/{software,firmware}/`, unzips into
  a `<dir>\temp\` folder, and runs the exe. Nothing imports `WinVerifyTrust`, so the only trust anchor is HTTPS to the bucket.
* **Elevated autostart:** registers Task Scheduler task "Challenger Driver StartupTask" (`0x405256`) with a run level and `-hide` arg.
* **Enables `SeDebugPrivilege`** and has a kill-by-process-name routine (`0x401dc0`, `OpenProcess(PROCESS_ALL_ACCESS)` + `TerminateProcess`).
  `OpenHardwareMonitorServer.exe` is referenced nearby; I didn't confirm which process it targets.
* Settings stored in the registry under `SOFTWARE\Challenger Driver` (hive not confirmed): `led_switch`, `switch_interval`, `device_type`, `tempe_type`, `fan_index`.
* No other device commands: `SendToAll` is only called from the sensor frame, so LEDs etc. are not set over HID by this build.

## Running challenger.py on Linux (Fedora)

```sh
pip install hidapi            # if no wheel for your Python: sudo dnf install gcc python3-devel hidapi-devel libusb1-devel systemd-devel
sudo tee /etc/udev/rules.d/99-challenger.rules <<'R'
KERNEL=="hidraw*", ATTRS{idVendor}=="1a2c", ATTRS{idProduct}=="4c86", TAG+="uaccess"
KERNEL=="hidraw*", ATTRS{idVendor}=="26ce", ATTRS{idProduct}=="0a13", TAG+="uaccess"
R
sudo udevadm control --reload && sudo udevadm trigger   # then replug the cooler
```
