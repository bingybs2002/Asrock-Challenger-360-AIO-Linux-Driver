/* challenger.c - stream CPU temp + fan RPM to the Challenger cooler display (see PROTOCOL.md).
 *
 *   make
 *   ./challenger --dry-run              print the 5 packets for the current sensor values
 *   ./challenger --version              query firmware version
 *   ./challenger                        send a frame every second (display layout: big RPM, small CPU temp)
 *
 * Sensors are hwmon "chip/attr" specs, looked up by chip name since hwmonN numbering isn't stable.
 */
#define _GNU_SOURCE /* memmem */
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <linux/hidraw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define REPORT_ID 0x07
#define PKT_LEN   65 /* report ID + 64 data bytes, like HidD_SetFeature(h, buf, 0x41) in the vendor app */
#define USAGE_PAGE_FF01 "\x06\x01\xff"

static const unsigned ids[][2] = {{0x1a2c, 0x4c86}, {0x26ce, 0x0a13}};

/* level = index of first threshold >= x, +1 (tables from DeviceDriver.exe) */
static const int temp_steps[] = {50, 100, 150, 200, 250, 300, 350, 400, 450, 500,
                                 550, 600, 650, 700, 750, 800, 850, 900, 1000}; /* CPU temp x10 */
static const int rpm_steps[] = {50, 100, 150, 200, 300, 450, 600, 800, 1000, 1200,
                                1400, 1600, 1800, 2000, 2200, 2400, 2600, 2800, 3000};

static int digit(int x, int place) { return (x / place) % 10; }

static int step_lvl(int x, const int *steps, int n)
{
    for (int i = 0; i < n; i++)
        if (x <= steps[i])
            return i + 1;
    return n + 1;
}

static int clamp(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

/* Only CPU temp and RPM are filled in; GPU/usage/power/clock fields stay zero.
 * pkt-4 mode byte 2 = big fan RPM + small CPU temp (tested on fw 0x0023). */
static void build_frame(uint8_t f[5][PKT_LEN], double cpu_temp, long rpm, int fahrenheit)
{
    int T = clamp((int)(cpu_temp * 10), 0, 9999);
    int tlvl = T > 0 ? step_lvl(T, temp_steps, 19) : 1;
    if (fahrenheit) /* the app converts after computing levels */
        T = clamp((int)(T * 1.8 + 320), 0, 9999);
    int R = clamp((int)rpm, 0, 9999);

    memset(f, 0, 5 * PKT_LEN);
    for (int i = 0; i < 5; i++) {
        f[i][0] = REPORT_ID;
        f[i][1] = i;
    }
    /* pkt 0: temps as decimal digits, GPU in high nibble (0 here), CPU in low nibble */
    f[0][2] = digit(T, 1000);
    f[0][3] = digit(T, 100);
    f[0][4] = digit(T, 10);
    f[0][5] = digit(T, 1);
    f[0][6] = fahrenheit;
    f[0][7] = tlvl & 0xf;
    f[1][7] = 1; /* constant in the vendor app */
    /* pkt 4: RPM digits as whole bytes, then mode + level */
    f[4][2] = digit(R, 1000);
    f[4][3] = digit(R, 100);
    f[4][4] = digit(R, 10);
    f[4][5] = digit(R, 1);
    f[4][6] = 2;
    f[4][7] = R > 0 ? step_lvl(R, rpm_steps, 19) : 0;
}

static int read_file(const char *path, char *buf, size_t len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;
    size_t n = fread(buf, 1, len - 1, fp);
    fclose(fp);
    buf[n] = 0;
    return (int)n;
}

/* The cooler exposes several hidraw nodes; pick the one whose report descriptor has usage page 0xFF01. */
static int open_device(void)
{
    glob_t g;
    int fd = -1;
    if (glob("/sys/class/hidraw/hidraw*", 0, NULL, &g) != 0)
        return -1;
    for (size_t i = 0; i < g.gl_pathc && fd < 0; i++) {
        char path[512], buf[4096];
        unsigned bus, vid, pid;
        snprintf(path, sizeof path, "%s/device/uevent", g.gl_pathv[i]);
        if (read_file(path, buf, sizeof buf) < 0)
            continue;
        char *id = strstr(buf, "HID_ID=");
        if (!id || sscanf(id, "HID_ID=%x:%x:%x", &bus, &vid, &pid) != 3)
            continue;
        int match = 0;
        for (size_t k = 0; k < sizeof ids / sizeof ids[0]; k++)
            match |= ids[k][0] == vid && ids[k][1] == pid;
        if (!match)
            continue;
        snprintf(path, sizeof path, "%s/device/report_descriptor", g.gl_pathv[i]);
        int n = read_file(path, buf, sizeof buf);
        if (n < 3 || !memmem(buf, n, USAGE_PAGE_FF01, 3))
            continue;
        snprintf(path, sizeof path, "/dev/%s", strrchr(g.gl_pathv[i], '/') + 1);
        fd = open(path, O_RDWR);
    }
    globfree(&g);
    return fd;
}

static int send_frame(int fd, uint8_t f[5][PKT_LEN])
{
    for (int i = 0; i < 5; i++) {
        if (ioctl(fd, HIDIOCSFEATURE(PKT_LEN), f[i]) < 0)
            return -1;
        usleep(10000);
    }
    return 0;
}

/* spec = "chip/attr", e.g. "k10temp/temp1" -> .../hwmonN/temp1_input where hwmonN/name == "k10temp" */
static int hwmon_read(const char *spec, long *out)
{
    char chip[64], attr[64], path[512], buf[64];
    glob_t g;
    int ok = -1;
    if (sscanf(spec, "%63[^/]/%63s", chip, attr) != 2)
        return -1;
    if (glob("/sys/class/hwmon/hwmon*", 0, NULL, &g) != 0)
        return -1;
    for (size_t i = 0; i < g.gl_pathc && ok < 0; i++) {
        snprintf(path, sizeof path, "%s/name", g.gl_pathv[i]);
        if (read_file(path, buf, sizeof buf) < 0)
            continue;
        buf[strcspn(buf, "\n")] = 0;
        if (strcmp(buf, chip) != 0)
            continue;
        snprintf(path, sizeof path, "%s/%s_input", g.gl_pathv[i], attr);
        if (read_file(path, buf, sizeof buf) > 0) {
            *out = strtol(buf, NULL, 10);
            ok = 0;
        }
    }
    globfree(&g);
    return ok;
}

static void read_sensors(const char *temp_spec, const char *fan_spec, double *temp, long *rpm)
{
    static int warned_temp, warned_fan; /* log a missing sensor once, not every second */
    long v;
    if (hwmon_read(temp_spec, &v) == 0) {
        *temp = v / 1000.0;
        warned_temp = 0;
    } else {
        *temp = 0;
        if (!warned_temp++)
            fprintf(stderr, "sensor %s not found\n", temp_spec);
    }
    if (hwmon_read(fan_spec, &v) == 0) {
        *rpm = v;
        warned_fan = 0;
    } else {
        *rpm = 0;
        if (!warned_fan++)
            fprintf(stderr, "sensor %s not found\n", fan_spec);
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--dry-run | --version] [--temp-sensor CHIP/ATTR] [--fan-sensor CHIP/ATTR] [--fahrenheit]\n"
            "  defaults: --temp-sensor k10temp/temp1 (Intel: coretemp/temp1) --fan-sensor it8689/fan1\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *temp_spec = "k10temp/temp1", *fan_spec = "it8689/fan1";
    int dry_run = 0, version = 0, fahrenheit = 0;
    static const struct option opts[] = {
        {"dry-run", no_argument, NULL, 'n'},     {"version", no_argument, NULL, 'v'},
        {"temp-sensor", required_argument, NULL, 't'}, {"fan-sensor", required_argument, NULL, 'f'},
        {"fahrenheit", no_argument, NULL, 'F'},  {"help", no_argument, NULL, 'h'},
        {0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'n': dry_run = 1; break;
        case 'v': version = 1; break;
        case 't': temp_spec = optarg; break;
        case 'f': fan_spec = optarg; break;
        case 'F': fahrenheit = 1; break;
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }

    uint8_t f[5][PKT_LEN];
    double temp;
    long rpm;

    if (dry_run) {
        read_sensors(temp_spec, fan_spec, &temp, &rpm);
        printf("cpu temp %.1f, fan %ld rpm\n", temp, rpm);
        build_frame(f, temp, rpm, fahrenheit);
        for (int i = 0; i < 5; i++) {
            for (int j = 0; j < 8; j++)
                printf("%02x%c", f[i][j], j < 7 ? ' ' : '\n');
        }
        return 0;
    }

    if (version) {
        int fd = open_device();
        if (fd < 0) {
            fprintf(stderr, "Challenger device not found (or no permission)\n");
            return 1;
        }
        uint8_t buf[64] = {REPORT_ID}; /* 64 bytes including the report ID */
        if (ioctl(fd, HIDIOCSFEATURE(sizeof buf), buf) < 0)
            return perror("set feature"), 1;
        usleep(16000);
        memset(buf, 0, sizeof buf);
        buf[0] = REPORT_ID;
        if (ioctl(fd, HIDIOCGFEATURE(sizeof buf), buf) < 0)
            return perror("get feature"), 1;
        printf("firmware version: 0x%04x\n", buf[1] << 8 | buf[2]);
        close(fd);
        return 0;
    }

    /* live: reopen the device whenever it disappears (unplug, boot ordering) */
    int fd = -1, warned_dev = 0;
    for (;;) {
        read_sensors(temp_spec, fan_spec, &temp, &rpm);
        build_frame(f, temp, rpm, fahrenheit);
        if (fd < 0)
            fd = open_device();
        if (fd >= 0 && send_frame(fd, f) == 0) {
            warned_dev = 0;
        } else {
            if (!warned_dev++)
                fprintf(stderr, "Challenger device not available, retrying\n");
            if (fd >= 0)
                close(fd);
            fd = -1;
        }
        sleep(1);
    }
}
