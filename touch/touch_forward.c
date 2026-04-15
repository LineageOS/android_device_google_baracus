/*
 * Copyright (C) 2024 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * touch_forward - Multitouch forwarding for Google Jamboard
 *
 * Reads touch events from the hazelred RAPT Touch controller and forwards
 * them to the USB HID gadget (/dev/hidg0). Forwarding is gated by the
 * vendor.scaler.input_source property published by scalerd.
 *
 * When input source is HDMI (3), touch events are forwarded to the host PC.
 * When input source is anything else, forwarding is disabled.
 *
 * Architecture:
 *   Hazelred -> USB -> Tegra evdev -> touch_forward -> /dev/hidg0 -> Host PC
 *   scalerd (UART) -> vendor.scaler.input_source property -> touch_forward
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_TAG "touch_forward"
#include <log/log.h>
#include <cutils/properties.h>

#define HIDG_DEV              "/dev/hidg0"
#define HAZELRED_PHYS         "usb-70090000.xusb"

#define MAX_CONTACTS    24
#define REPORT_SIZE     340
#define REPORT_ID       0xF1
#define CONTACT_BYTES   14
#define COUNT_OFFSET    (1 + MAX_CONTACTS * CONTACT_BYTES)

#define PROP_INPUT_SOURCE     "vendor.scaler.input_source"
#define INPUT_ANDROID         0

struct contact {
    int active;
    int x, y, width, height, pressure;
};

static struct contact contacts[MAX_CONTACTS];
static int current_slot;
static volatile int running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

static int get_input_source(void) {
    char val[PROPERTY_VALUE_MAX] = {0};
    property_get(PROP_INPUT_SOURCE, val, "0");
    return atoi(val);
}

/*
 * Enable all RAPT Touch input devices.
 */
static void enable_touch_devices(void) {
    DIR *dir = opendir("/sys/class/input");
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        char name_path[256], name[256] = {0}, enable_path[256];
        snprintf(name_path, sizeof(name_path),
                 "/sys/class/input/%s/device/name", ent->d_name);

        int nfd = open(name_path, O_RDONLY);
        if (nfd < 0) continue;
        read(nfd, name, sizeof(name) - 1);
        close(nfd);

        if (strstr(name, "RAPT") != NULL) {
            snprintf(enable_path, sizeof(enable_path),
                     "/sys/class/input/%s/device/enabled", ent->d_name);
            int efd = open(enable_path, O_WRONLY);
            if (efd >= 0) {
                write(efd, "1", 1);
                close(efd);
                ALOGI("Enabled %s", ent->d_name);
            }
        }
    }
    closedir(dir);
}

/*
 * Find and open the hazelred touch input device.
 */
static int find_touch_device(void) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        char path[256], phys[256] = {0};
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys);
        if (strstr(phys, HAZELRED_PHYS) == NULL) {
            close(fd);
            continue;
        }

        unsigned char abs_bits[(ABS_MAX + 7) / 8];
        memset(abs_bits, 0, sizeof(abs_bits));
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
        if (!(abs_bits[ABS_MT_POSITION_X / 8] & (1 << (ABS_MT_POSITION_X % 8)))) {
            close(fd);
            continue;
        }

        int flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

        /* Exclusively grab the device so Android's InputReader
         * doesn't also process these touch events */
        if (ioctl(fd, EVIOCGRAB, 1) < 0)
            ALOGW("EVIOCGRAB failed: %s (touch may duplicate)", strerror(errno));

        char name[256] = "Unknown";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        ALOGI("Found: %s (%s) phys=%s [grabbed]", path, name, phys);
        closedir(dir);
        return fd;
    }
    closedir(dir);
    return -1;
}

static void build_report(unsigned char *report) {
    memset(report, 0, REPORT_SIZE);
    report[0] = REPORT_ID;

    int count = 0;
    for (int i = 0; i < MAX_CONTACTS; i++) {
        int off = 1 + i * CONTACT_BYTES;
        struct contact *c = &contacts[i];
        report[off]      = c->active ? 0x01 : 0x00;
        report[off + 1]  = (unsigned char)i;
        report[off + 2]  = c->x & 0xFF;
        report[off + 3]  = (c->x >> 8) & 0xFF;
        report[off + 4]  = c->y & 0xFF;
        report[off + 5]  = (c->y >> 8) & 0xFF;
        report[off + 6]  = c->width & 0xFF;
        report[off + 7]  = (c->width >> 8) & 0xFF;
        report[off + 8]  = c->height & 0xFF;
        report[off + 9]  = (c->height >> 8) & 0xFF;
        report[off + 10] = c->pressure & 0xFF;
        report[off + 11] = (c->pressure >> 8) & 0xFF;
        if (c->active) count++;
    }
    report[COUNT_OFFSET] = (unsigned char)count;
}

static void process_event(const struct input_event *ev) {
    if (ev->type != EV_ABS) return;

    switch (ev->code) {
    case ABS_MT_SLOT:
        if (ev->value >= 0 && ev->value < MAX_CONTACTS)
            current_slot = ev->value;
        break;
    case ABS_MT_TRACKING_ID:
        if (ev->value >= 0)
            contacts[current_slot].active = 1;
        else
            memset(&contacts[current_slot], 0, sizeof(struct contact));
        break;
    case ABS_MT_POSITION_X:
        contacts[current_slot].x = ev->value;
        break;
    case ABS_MT_POSITION_Y:
        contacts[current_slot].y = ev->value;
        break;
    case ABS_MT_TOUCH_MAJOR:
    case ABS_MT_WIDTH_MAJOR:
        contacts[current_slot].width = ev->value;
        break;
    case ABS_MT_TOUCH_MINOR:
    case ABS_MT_WIDTH_MINOR:
        contacts[current_slot].height = ev->value;
        break;
    case ABS_MT_PRESSURE:
        contacts[current_slot].pressure = ev->value;
        break;
    }
}

int main(void) {
    ALOGI("touch_forward starting (property-gated mode)");
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Wait for /dev/hidg0 */
    int hidg_fd = -1;
    for (int i = 0; i < 50 && running; i++) {
        hidg_fd = open(HIDG_DEV, O_WRONLY);
        if (hidg_fd >= 0) break;
        usleep(200000);
    }
    if (hidg_fd < 0) {
        ALOGE("Failed to open %s: %s", HIDG_DEV, strerror(errno));
        return 1;
    }
    ALOGI("Opened %s", HIDG_DEV);

    /* Enable touch devices */
    enable_touch_devices();

    /* Find hazelred touch device */
    int touch_fd = -1;
    for (int i = 0; i < 30 && running; i++) {
        touch_fd = find_touch_device();
        if (touch_fd >= 0) break;
        ALOGI("Waiting for touch device...");
        enable_touch_devices();
        sleep(2);
    }
    if (touch_fd < 0) {
        ALOGE("Touch device not found");
        close(hidg_fd);
        return 1;
    }

    int forwarding = 0;
    int last_source = -1;

    /* Check initial state from scalerd */
    int source = get_input_source();
    forwarding = (source != INPUT_ANDROID);
    ALOGI("Initial input source: %d, forwarding: %s", source,
          forwarding ? "ON" : "OFF");

    struct input_event ev;
    unsigned char report[REPORT_SIZE];
    struct pollfd pfd = {.fd = touch_fd, .events = POLLIN};

    while (running) {
        int ret = poll(&pfd, 1, 500); /* 500ms timeout to check property */
        if (ret < 0) {
            if (errno == EINTR) continue;
            ALOGE("poll: %s", strerror(errno));
            break;
        }

        /* Check property for input source changes */
        source = get_input_source();
        if (source != last_source) {
            int new_fwd = (source != INPUT_ANDROID);
            if (new_fwd != forwarding) {
                forwarding = new_fwd;
                ALOGI("Input source: %d -> forwarding %s",
                      source, forwarding ? "ON" : "OFF");
                if (forwarding)
                    enable_touch_devices();
            }
            last_source = source;
        }

        if (ret == 0) continue;

        /* Read touch events */
        if (pfd.revents & POLLIN) {
            ssize_t n = read(touch_fd, &ev, sizeof(ev));
            if (n != sizeof(ev)) {
                if (n < 0 && errno == EINTR) continue;
                ALOGE("read: %s", strerror(errno));
                break;
            }

            process_event(&ev);

            if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                if (forwarding) {
                    build_report(report);
                    if (write(hidg_fd, report, REPORT_SIZE) < 0) {
                        if (errno == EAGAIN || errno == EINTR) continue;
                        ALOGE("write: %s", strerror(errno));
                        usleep(100000);
                    }
                }
            }
        }
    }

    ALOGI("touch_forward stopping");
    close(touch_fd);
    close(hidg_fd);
    return 0;
}
