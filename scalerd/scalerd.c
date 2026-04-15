/*
 * Copyright (C) 2024 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * scalerd - MStar scaler UART daemon for Google Jamboard
 *
 * Central daemon that owns the UART connection to the MST9U23T1 display
 * scaler (/dev/ttyTHS1). Provides:
 *   - Boot-time state sync (queries brightness, input source, speaker, FW version)
 *   - Input source change notifications (published via system properties)
 *   - Brightness control (watches /sys/class/backlight/scaler/brightness)
 *   - Input source switching (watches vendor.scaler.input_source.target property)
 *   - Speaker always-ON enforcement on boot
 *
 * See UART_PROTOCOL.md for the complete protocol specification.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "scalerd"
#include <log/log.h>
#include <cutils/properties.h>

/* UART configuration */
#define SCALER_UART           "/dev/ttyTHS1"
#define SCALER_BAUD           B9600

/* Backlight sysfs node (created by scaler_backlight kernel driver) */
#define BACKLIGHT_PATH        "/sys/class/backlight/scaler/brightness"
#define BACKLIGHT_DIR         "/sys/class/backlight/scaler"

/* System properties */
#define PROP_INPUT_SOURCE     "vendor.scaler.input_source"
#define PROP_INPUT_TARGET     "vendor.scaler.input_source.target"
#define PROP_BRIGHTNESS       "vendor.scaler.brightness"
#define PROP_SPEAKER_STATE    "vendor.scaler.speaker_state"
#define PROP_FIRMWARE_VERSION "vendor.scaler.firmware_version"

/* Protocol constants */
#define FRAME_LEN             10

/* Command frame: 6E 51 86 VV F5 DD CC PP DD XX */
#define MAGIC_0               0x6E
#define MAGIC_1               0x51
#define MAGIC_2               0x86
#define MAGIC_4               0xF5
#define DIR_COMMAND           0xDD  /* Tegra → Scaler */
#define DIR_NOTIFICATION      0xDC  /* Scaler → Tegra */

/* GET version byte */
#define VER_GET               0x01
/* SET version byte */
#define VER_SET               0x03

/* Command types */
#define CMD_NOTIF_INPUT       0x82
#define CMD_GET_BRIGHTNESS    0x83
#define CMD_SET_BRIGHTNESS    0x85
#define CMD_GET_FIRMWARE      0x86
#define CMD_GET_INPUT         0xE2
#define CMD_SET_INPUT         0xE2
#define CMD_GET_SPEAKER       0xE4
#define CMD_SET_SPEAKER       0xE4

/* GET param byte */
#define PARAM_GET             0x01
/* SET param bytes */
#define PARAM_SET_BRIGHT      0xFE
#define PARAM_SET_INPUT       0xFE
#define PARAM_SET_SPEAKER     0xFF

/* Response frame: C2 3D SS CC 01 00 LL [data] XX */
#define RESP_MAGIC_0          0xC2
#define RESP_MAGIC_1          0x3D
#define RESP_STATUS_OK        0x01
#define RESP_MAX_LEN          32

/* Input source IDs */
#define INPUT_ANDROID         0
#define INPUT_DISPLAYPORT     1
#define INPUT_HDMI1_SIDE      2
#define INPUT_HDMI2_BACK      3

static volatile int running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

/* ── UART helpers ─────────────────────────────────────────────── */

static int uart_open(void) {
    int fd = open(SCALER_UART, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        ALOGE("Failed to open %s: %s", SCALER_UART, strerror(errno));
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) < 0) {
        ALOGE("tcgetattr: %s", strerror(errno));
        close(fd);
        return -1;
    }

    cfmakeraw(&tio);
    cfsetispeed(&tio, SCALER_BAUD);
    cfsetospeed(&tio, SCALER_BAUD);
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        ALOGE("tcsetattr: %s", strerror(errno));
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);

    ALOGI("Opened %s at 9600 baud", SCALER_UART);
    return fd;
}

static unsigned char xor_checksum(const unsigned char *buf, int len) {
    unsigned char cs = 0;
    for (int i = 0; i < len; i++)
        cs ^= buf[i];
    return cs;
}

/* Build and send a SET command frame */
static int uart_send_set(int fd, unsigned char cmd, unsigned char param,
                         unsigned char data) {
    unsigned char frame[FRAME_LEN];
    frame[0] = MAGIC_0;
    frame[1] = MAGIC_1;
    frame[2] = MAGIC_2;
    frame[3] = VER_SET;
    frame[4] = MAGIC_4;
    frame[5] = DIR_COMMAND;
    frame[6] = cmd;
    frame[7] = param;
    frame[8] = data;
    frame[9] = xor_checksum(frame, 9);

    ssize_t n = write(fd, frame, FRAME_LEN);
    if (n != FRAME_LEN) {
        ALOGE("UART write failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* Build and send a GET command frame */
static int uart_send_get(int fd, unsigned char cmd) {
    unsigned char frame[FRAME_LEN];
    frame[0] = MAGIC_0;
    frame[1] = MAGIC_1;
    frame[2] = MAGIC_2;
    frame[3] = VER_GET;
    frame[4] = MAGIC_4;
    frame[5] = DIR_COMMAND;
    frame[6] = cmd;
    frame[7] = PARAM_GET;
    frame[8] = 0x00;
    frame[9] = xor_checksum(frame, 9);

    ssize_t n = write(fd, frame, FRAME_LEN);
    if (n != FRAME_LEN) {
        ALOGE("UART write failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Read a response frame from the scaler.
 * Response format: C2 3D SS CC 01 00 LL [data...] XX
 * Returns data length on success, -1 on failure/timeout.
 * Response data is written to resp_data (up to resp_max bytes).
 */
static int uart_read_response(int fd, unsigned char expected_cmd,
                              unsigned char *resp_data, int resp_max) {
    unsigned char buf[RESP_MAX_LEN];
    int pos = 0;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};

    /* Wait up to 500ms for response */
    for (int attempts = 0; attempts < 10; attempts++) {
        int ret = poll(&pfd, 1, 50);
        if (ret <= 0) continue;

        ssize_t n = read(fd, buf + pos, sizeof(buf) - pos);
        if (n <= 0) continue;
        pos += n;

        /* Minimum response: C2 3D SS CC 01 00 LL XX = 8 bytes */
        if (pos < 8) continue;

        /* Verify magic */
        if (buf[0] != RESP_MAGIC_0 || buf[1] != RESP_MAGIC_1)
            return -1;

        /* Check status */
        if (buf[2] != RESP_STATUS_OK) {
            ALOGW("Response error: status=0x%02X cmd=0x%02X", buf[2], buf[3]);
            return -1;
        }

        /* Verify command echo */
        if (buf[3] != expected_cmd)
            return -1;

        int data_len = buf[6];
        int total_len = 7 + data_len + 1; /* header + data + checksum */

        if (pos < total_len) continue; /* need more bytes */

        /* Verify checksum */
        unsigned char cs = xor_checksum(buf, total_len - 1);
        if (cs != buf[total_len - 1]) {
            ALOGW("Response checksum mismatch");
            return -1;
        }

        /* Copy data */
        int copy_len = data_len < resp_max ? data_len : resp_max;
        if (copy_len > 0)
            memcpy(resp_data, buf + 7, copy_len);

        return data_len;
    }

    return -1; /* timeout */
}

/* ── Notification parsing ─────────────────────────────────────── */

/*
 * Parse a 10-byte notification frame.
 * Returns the input source value (0-3) on success, -1 on invalid.
 */
static int parse_notification(const unsigned char *buf, int len) {
    if (len < FRAME_LEN) return -1;

    if (buf[0] != MAGIC_0 || buf[1] != MAGIC_1 || buf[2] != MAGIC_2 ||
        buf[3] != VER_SET || buf[4] != MAGIC_4)
        return -1;

    if (buf[5] != DIR_NOTIFICATION || buf[6] != CMD_NOTIF_INPUT)
        return -1;

    unsigned char cs = xor_checksum(buf, FRAME_LEN - 1);
    if (cs != buf[FRAME_LEN - 1]) {
        ALOGW("Notification checksum mismatch");
        return -1;
    }

    return buf[8];
}

/* ── High-level commands ──────────────────────────────────────── */

static const char *input_source_name(int source) {
    switch (source) {
    case INPUT_ANDROID:     return "Android";
    case INPUT_DISPLAYPORT: return "DisplayPort";
    case INPUT_HDMI1_SIDE:  return "HDMI1 (Side)";
    case INPUT_HDMI2_BACK:  return "HDMI2 (Back)";
    default:                return "Unknown";
    }
}

static int query_brightness(int fd) {
    if (uart_send_get(fd, CMD_GET_BRIGHTNESS) < 0) return -1;
    unsigned char data;
    if (uart_read_response(fd, CMD_GET_BRIGHTNESS, &data, 1) < 1) return -1;
    return data;
}

static int query_input_source(int fd) {
    if (uart_send_get(fd, CMD_GET_INPUT) < 0) return -1;
    unsigned char data;
    if (uart_read_response(fd, CMD_GET_INPUT, &data, 1) < 1) return -1;
    return data;
}

static int query_speaker_state(int fd) {
    if (uart_send_get(fd, CMD_GET_SPEAKER) < 0) return -1;
    unsigned char data;
    if (uart_read_response(fd, CMD_GET_SPEAKER, &data, 1) < 1) return -1;
    return data;
}

static int query_firmware_version(int fd, char *version, int max_len) {
    if (uart_send_get(fd, CMD_GET_FIRMWARE) < 0) return -1;
    unsigned char data[16];
    int len = uart_read_response(fd, CMD_GET_FIRMWARE, data, sizeof(data) - 1);
    if (len < 1) return -1;
    data[len] = '\0';
    snprintf(version, max_len, "%s", (char *)data);
    return 0;
}

static int set_brightness(int fd, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return uart_send_set(fd, CMD_SET_BRIGHTNESS, PARAM_SET_BRIGHT,
                         (unsigned char)percent);
}

static int set_input_source(int fd, int source) {
    if (source < 0 || source > 3) return -1;
    return uart_send_set(fd, CMD_SET_INPUT, PARAM_SET_INPUT,
                         (unsigned char)source);
}

static int set_speaker_state(int fd, int enabled) {
    return uart_send_set(fd, CMD_SET_SPEAKER, PARAM_SET_SPEAKER,
                         enabled ? 0x01 : 0x00);
}

/* ── Backlight sysfs reading ──────────────────────────────────── */

static int read_backlight_brightness(void) {
    int fd = open(BACKLIGHT_PATH, O_RDONLY);
    if (fd < 0) return -1;
    char buf[16] = {0};
    read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return atoi(buf);
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(void) {
    ALOGI("scalerd starting");
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Open UART */
    int uart_fd = -1;
    for (int i = 0; i < 30 && running; i++) {
        uart_fd = uart_open();
        if (uart_fd >= 0) break;
        sleep(1);
    }
    if (uart_fd < 0) {
        ALOGE("Could not open UART, exiting");
        return 1;
    }

    /* ── Boot-time state sync ── */
    usleep(500000); /* Let scaler settle */

    /* Query and publish input source */
    int source = query_input_source(uart_fd);
    if (source >= 0) {
        char val[PROPERTY_VALUE_MAX];
        snprintf(val, sizeof(val), "%d", source);
        property_set(PROP_INPUT_SOURCE, val);
        ALOGI("Input source: %s (%d)", input_source_name(source), source);
    } else {
        property_set(PROP_INPUT_SOURCE, "0");
        ALOGW("Could not query input source, assuming Android");
    }

    /* Query and publish brightness */
    int brightness = query_brightness(uart_fd);
    if (brightness >= 0) {
        char val[PROPERTY_VALUE_MAX];
        snprintf(val, sizeof(val), "%d", brightness);
        property_set(PROP_BRIGHTNESS, val);
        ALOGI("Brightness: %d%%", brightness);
    }

    /* Ensure speaker is ON */
    int speaker = query_speaker_state(uart_fd);
    if (speaker == 0) {
        ALOGI("Speaker was OFF, enabling");
        set_speaker_state(uart_fd, 1);
        speaker = 1;
    }
    property_set(PROP_SPEAKER_STATE, speaker ? "1" : "0");
    ALOGI("Speaker: %s", speaker ? "ON" : "OFF");

    /* Query firmware version */
    char fw_version[32] = "unknown";
    if (query_firmware_version(uart_fd, fw_version, sizeof(fw_version)) == 0) {
        property_set(PROP_FIRMWARE_VERSION, fw_version);
        ALOGI("Firmware: %s", fw_version);
    }

    int last_bl_value = -1; /* Track to avoid redundant UART sends */
    int last_input_target = -1;

    /* ── Main loop ── */
    /* Note: sysfs doesn't reliably generate inotify events, so we poll
     * the backlight value on each loop iteration (every 500ms). */
    struct pollfd pfds[1];
    pfds[0].fd = uart_fd;
    pfds[0].events = POLLIN;

    /* Notification frame accumulator */
    unsigned char notif_buf[FRAME_LEN];
    int notif_pos = 0;

    while (running) {
        int ret = poll(pfds, 1, 500); /* 500ms poll for UART + property/sysfs checks */
        if (ret < 0) {
            if (errno == EINTR) continue;
            ALOGE("poll: %s", strerror(errno));
            break;
        }

        /* ── Handle UART data (notifications) ── */
        if (pfds[0].revents & POLLIN) {
            unsigned char buf[64];
            ssize_t n = read(uart_fd, buf, sizeof(buf));
            if (n > 0) {
                for (int i = 0; i < n; i++) {
                    if (notif_pos == 0) {
                        if (buf[i] == MAGIC_0)
                            notif_buf[notif_pos++] = buf[i];
                    } else {
                        notif_buf[notif_pos++] = buf[i];
                        if (notif_pos >= FRAME_LEN) {
                            int src = parse_notification(notif_buf, notif_pos);
                            if (src >= 0) {
                                char val[PROPERTY_VALUE_MAX];
                                snprintf(val, sizeof(val), "%d", src);
                                property_set(PROP_INPUT_SOURCE, val);
                                ALOGI("Input source changed: %s (%d)",
                                      input_source_name(src), src);
                            }
                            notif_pos = 0;
                        }
                    }
                }
            }
        }

        /* ── Poll backlight sysfs for brightness changes ── */
        {
            int bl = read_backlight_brightness();
            if (bl >= 0 && bl != last_bl_value) {
                /* Map 0-255 (Android) to 0-100 (scaler) */
                int pct = (bl * 100 + 127) / 255;
                set_brightness(uart_fd, pct);
                last_bl_value = bl;

                char val[PROPERTY_VALUE_MAX];
                snprintf(val, sizeof(val), "%d", pct);
                property_set(PROP_BRIGHTNESS, val);
            }
        }

        /* ── Check for input source target change (via file IPC) ── */
        {
            int tfd = open("/data/misc/scalerd/input_target", O_RDONLY);
            if (tfd >= 0) {
                char val[8] = {0};
                read(tfd, val, sizeof(val) - 1);
                close(tfd);
                if (val[0] >= '0' && val[0] <= '3') {
                    int target = val[0] - '0';
                    if (target != last_input_target) {
                        ALOGI("Switching input to %s (%d)",
                              input_source_name(target), target);
                        set_input_source(uart_fd, target);
                        last_input_target = target;
                    }
                }
            }
        }

    }

    ALOGI("scalerd stopping");
    close(uart_fd);
    return 0;
}
