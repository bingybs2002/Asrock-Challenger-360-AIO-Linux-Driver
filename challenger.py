"""Minimal re-implementation of the Challenger cooler display protocol (see PROTOCOL.md).

    python3 challenger.py --dry-run                  # print the 5 packets
    python3 challenger.py --version                  # query firmware version
    python3 challenger.py --cpu-temp 45.3 --rpm 1450 # send one frame
    python3 challenger.py --live                     # stream real CPU temp + fan RPM every second

Linux uses /dev/hidraw directly (stdlib only); elsewhere it falls back to the `hidapi` package.
"""
import argparse
import glob
import os
import sys
import time

IDS = [(0x1A2C, 0x4C86), (0x26CE, 0x0A13)]
USAGE_PAGE, USAGE = 0xFF01, 0x0001
REPORT_ID = 0x07


def d(x, place):  # decimal digit: place 1000/100/10/1
    return (x // place) % 10


def lvl10(x):  # usage / GPU-temp gauge: 0 when 0, else x/10+1 capped at 10
    return 0 if x == 0 else min(x // 10 + 1, 10)


# exact threshold tables from DeviceDriver.exe (level = index of first threshold >= x, +1)
TEMP_STEPS = [50 * i for i in range(1, 19)] + [1000]                    # CPU temp x10   (0x40ac2c)
CLOCK_STEPS = [500, 1000, 1200, 1400, 1600, 1800, 2000, 2100, 2300, 2500,
               3000, 3500, 4000, 4500, 5000, 5500, 6000, 6500, 7000]     # CPU MHz        (0x40ae54)
RPM_STEPS = [50, 100, 150, 200, 300, 450, 600, 800, 1000, 1200,
             1400, 1600, 1800, 2000, 2200, 2400, 2600, 2800, 3000]       # fan RPM        (0x40b005)


def step_lvl(x, steps):
    for i, s in enumerate(steps, 1):
        if x <= s:
            return i
    return len(steps) + 1


def build_frame(cpu_temp=0.0, gpu_temp=0.0, cpu_usage=0, gpu_usage=0, cpu_power=0, gpu_power=0,
                cpu_clock=0, gpu_clock=0, rpm=0, fahrenheit=False, mode=2):
    T, GT = min(int(cpu_temp * 10), 9999), min(int(gpu_temp * 10), 9999)
    tlvl = step_lvl(T, TEMP_STEPS) if T > 0 else 1
    gtlvl = 0 if GT == 0 else min(GT // 100 + 1, 10)
    if fahrenheit:  # the app converts after computing levels
        T, GT = min(int(T * 1.8 + 320), 9999), min(int(GT * 1.8 + 320), 9999)
    U, GU = min(int(cpu_usage), 100), min(int(gpu_usage), 100)
    P, GP = int(cpu_power), int(gpu_power)
    if P >= 999 or GP >= 999:
        raise ValueError("app skips the frame when power >= 999 W")
    C, GC, R = min(int(cpu_clock), 9999), min(int(gpu_clock), 9999), min(int(rpm), 9999)

    nib = lambda g, c: ((g & 0xF) << 4) | (c & 0xF)
    pk = [
        [0, nib(d(GT, 1000), d(T, 1000)), nib(d(GT, 100), d(T, 100)), nib(d(GT, 10), d(T, 10)),
         nib(d(GT, 1), d(T, 1)), int(fahrenheit), nib(gtlvl, tlvl)],
        [1, nib(d(GU, 100), d(U, 100)), nib(d(GU, 10), d(U, 10)), nib(d(GU, 1), d(U, 1)),
         0, nib(lvl10(GU), lvl10(U)), 1],
        [2, nib(d(GP, 100), d(P, 100)), nib(d(GP, 10), d(P, 10)), nib(d(GP, 1), d(P, 1)), 0, 0, 0],
        [3, nib(d(GC, 1000), d(C, 1000)), nib(d(GC, 100), d(C, 100)), nib(d(GC, 10), d(C, 10)),
         nib(d(GC, 1), d(C, 1)), 0, 0],
    ]
    # pkt-4 mode byte picks the display layout (tested on fw 0x0023):
    #   2 (and unknown values) = big fan RPM + small CPU temp;  4 = CPU temp only;  1 = CPU clock
    if mode == 0:
        tail = [1, step_lvl(C, CLOCK_STEPS) if C > 0 else 0]
    elif mode == 1:
        tail = [4, tlvl]
    else:
        tail = [2, step_lvl(R, RPM_STEPS) if R > 0 else 0]
    pk.append([4, d(R, 1000), d(R, 100), d(R, 10), d(R, 1)] + tail)
    # report ID + 64 data bytes, like HidD_SetFeature(h, buf, 0x41) in the app
    return [bytes([REPORT_ID] + p + [0] * (64 - len(p))) for p in pk]


class Hidraw:
    """Feature reports on a /dev/hidraw node via ioctl, no third-party packages."""

    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR)

    @staticmethod
    def _ioc(nr, size):  # _IOC(_IOC_READ|_IOC_WRITE, 'H', nr, size)
        return (3 << 30) | (size << 16) | (ord("H") << 8) | nr

    def send_feature_report(self, data):
        import fcntl
        fcntl.ioctl(self.fd, self._ioc(0x06, len(data)), bytearray(data))  # HIDIOCSFEATURE

    def get_feature_report(self, report_id, length):
        import fcntl
        buf = bytearray([report_id] + [0] * (length - 1))  # length includes the report ID
        fcntl.ioctl(self.fd, self._ioc(0x07, len(buf)), buf)  # HIDIOCGFEATURE
        return list(buf)

    def close(self):
        os.close(self.fd)


def find_hidraw():
    """The cooler exposes several hidraw nodes; pick the one whose report descriptor has usage page 0xFF01."""
    for node in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        try:
            with open(f"{node}/device/uevent") as f:
                hid_id = next(l for l in f if l.startswith("HID_ID=")).strip().split("=")[1]
            with open(f"{node}/device/report_descriptor", "rb") as f:
                desc = f.read()
        except (OSError, StopIteration):
            continue
        _, vid, pid = (int(x, 16) for x in hid_id.split(":"))
        if (vid, pid) in IDS and bytes([0x06, USAGE_PAGE & 0xFF, USAGE_PAGE >> 8]) in desc:
            return "/dev/" + os.path.basename(node)
    return None


def open_device():
    if sys.platform.startswith("linux"):
        path = find_hidraw()
        if path is None:
            raise SystemExit("Challenger device not found")
        return Hidraw(path)
    import hid  # Windows / macOS: pip install hidapi
    for info in hid.enumerate():
        if (info["vendor_id"], info["product_id"]) in IDS and \
                info.get("usage_page") == USAGE_PAGE and info.get("usage") == USAGE:
            dev = hid.device()
            dev.open_path(info["path"])
            return dev
    raise SystemExit("Challenger device not found")


def hwmon_read(spec):
    """Read a hwmon value by chip name, e.g. 'k10temp/temp1' or 'it8689/fan1' (hwmonN numbering isn't stable)."""
    chip, attr = spec.split("/")
    for h in glob.glob("/sys/class/hwmon/hwmon*"):
        try:
            with open(f"{h}/name") as f:
                if f.read().strip() != chip:
                    continue
            with open(f"{h}/{attr}_input") as f:
                return int(f.read())
        except OSError:
            continue
    raise LookupError(f"sensor {spec} not found")


def live(temp_spec, fan_spec, fahrenheit):
    dev = None
    while True:
        try:
            temp = hwmon_read(temp_spec) / 1000
        except LookupError as e:
            temp = 0
            print(e, file=sys.stderr)
        try:
            rpm = hwmon_read(fan_spec)
        except LookupError as e:
            rpm = 0
            print(e, file=sys.stderr)
        frame = build_frame(cpu_temp=temp, rpm=rpm, fahrenheit=fahrenheit, mode=2)
        try:
            if dev is None:
                dev = open_device()
            for p in frame:
                dev.send_feature_report(p)
                time.sleep(0.01)
        except (OSError, SystemExit) as e:  # unplugged / not yet present: retry next tick
            print(f"device: {e}", file=sys.stderr)
            if dev is not None:
                try:
                    dev.close()
                except OSError:
                    pass
            dev = None
        time.sleep(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--version", action="store_true")
    ap.add_argument("--live", action="store_true", help="stream CPU temp + fan RPM from hwmon every second")
    ap.add_argument("--temp-sensor", default="k10temp/temp1", help="hwmon chip/attr for --live (Intel: coretemp/temp1)")
    ap.add_argument("--fan-sensor", default="it8689/fan1", help="hwmon chip/attr for --live")
    for k in ("cpu-temp", "gpu-temp", "cpu-usage", "gpu-usage", "cpu-power", "gpu-power",
              "cpu-clock", "gpu-clock", "rpm"):
        ap.add_argument(f"--{k}", type=float, default=0)
    ap.add_argument("--fahrenheit", action="store_true")
    ap.add_argument("--mode", type=int, default=2, help="layout: 0=clock 1=CPU temp only 2=fan RPM + CPU temp")
    a = ap.parse_args()

    if a.live:
        return live(a.temp_sensor, a.fan_sensor, a.fahrenheit)

    if a.version:
        dev = open_device()
        dev.send_feature_report(bytes([REPORT_ID]) + bytes(63))
        time.sleep(0.016)
        r = dev.get_feature_report(REPORT_ID, 64)
        print(f"firmware version: 0x{r[1] << 8 | r[2]:04x}  raw={bytes(r[:8]).hex(' ')}")
        return

    frame = build_frame(a.cpu_temp, a.gpu_temp, a.cpu_usage, a.gpu_usage, a.cpu_power, a.gpu_power,
                        a.cpu_clock, a.gpu_clock, a.rpm, a.fahrenheit, a.mode)
    for p in frame:
        print(p[:8].hex(" "))
    if a.dry_run:
        return
    dev = open_device()
    for p in frame:
        dev.send_feature_report(p)  # 0x41 bytes: ID + 64, same length as the app
        time.sleep(0.01)


if __name__ == "__main__":
    main()
