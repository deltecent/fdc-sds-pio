/*
 * fdc.c — FDC+ serial protocol engine + task (DESIGN.md §6, plan M4).
 *
 * A single high-priority task (pinned to core 1) owns UART2 and is the sole responder
 * to the FDC. It blocks on RX, validates the 10-byte command block's checksum, and
 * dispatches:
 *
 *   STAT  -> reply "STAT" with the drive-mount bitmap (Word2). Code is ignored by the FDC.
 *   READ  -> disk_read_track(), then stream <len> data bytes + a 16-bit checksum. No
 *            response header precedes the data (the FDC expects the payload directly).
 *            An unmounted / out-of-range drive gets no reply — the FDC times out & retries.
 *   WRIT  -> reply "WRIT" OK/NOT_READY; if OK, receive <len> data + checksum, write it,
 *            then reply "WSTA" with the final code (0 OK / 2 checksum / 3 write error).
 *
 * All offset/length math is 32-bit and lives in protocol.h/disk.c (the old firmware's
 * signed-int track bug is called out in DESIGN.md §6.5). SD track I/O goes through the
 * `disk` module's mutex, so the CLI/FTP paths never collide with this task on the card.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "fdc.h"
#include "config.h"
#include "disk.h"
#include "pins.h"
#include "protocol.h"

static const char *TAG = "fdc";

/* Per-call receive windows (DESIGN.md §6.4): ~1 s for a command, ~7 s for track data. */
#define FDC_CMD_TIMEOUT_MS   1000
#define FDC_DATA_TIMEOUT_MS  7000
/* Idle RX poll: how often the task wakes to apply a baud change / loopback request. */
#define FDC_IDLE_POLL_MS      200
/* Status LED: lit on a valid command, auto-off after this (DESIGN.md §6.7). */
#define FDC_STATUS_LED_MS      75
/* Loopback pattern length (DESIGN.md §6.8). */
#define FDC_LOOPBACK_LEN      256

#define FDC_TASK_STACK  4096
#define FDC_TASK_PRIO     10  /* well above the CLI/net tasks (prio 5) */
#define FDC_TASK_CORE      1  /* APP core, off the WiFi/lwIP core (DESIGN.md §5.2) */

/* RX ring must hold a full incoming write track (8192 + 2 checksum) with headroom. */
#define FDC_UART_RX_BUF  (DISK_TRACK_BUF_SIZE + 512)
/* UART event queue depth: enough to catch error events between task poll cycles. */
#define FDC_UART_EVT_QUEUE 24

static const uart_port_t s_port = FDC_UART_NUM;

/* Drive-activity LEDs, indexed by drive number (DESIGN.md §6.7). */
static const gpio_num_t s_drive_leds[DISK_MAX_DRIVE] = {
    (gpio_num_t)PIN_LED_DRIVE0,
    (gpio_num_t)PIN_LED_DRIVE1,
    (gpio_num_t)PIN_LED_DRIVE2,
    (gpio_num_t)PIN_LED_DRIVE3,
};

static TaskHandle_t       s_task;
static esp_timer_handle_t s_led_timer;

/* UART hardware event queue (FIFO overflow, framing errors), drained by the task. */
static QueueHandle_t      s_uart_evt_q;

/* Live baud + a pending change queued by fdc_set_baud(), applied by the task. */
static uint32_t          s_baud;
static volatile uint32_t s_pending_baud;

/* Stats, guarded so the CLI can read them while the task writes. */
static fdc_stats_t       s_stats;
static SemaphoreHandle_t s_stats_lock;

/* Loopback request/response, serialized by s_lb_lock; s_lb_done wakes the caller. */
static SemaphoreHandle_t s_lb_lock;
static SemaphoreHandle_t s_lb_done;
static volatile bool     s_lb_req;
static fdc_loopback_t    s_lb_result;

/* Receive scratch: one command block + one full write track. */
static uint8_t s_cmd[FDC_BLOCK_LEN];
static uint8_t s_resp[FDC_BLOCK_LEN];
static uint8_t s_data[DISK_TRACK_BUF_SIZE];

/* ---- LEDs ------------------------------------------------------------------ */

static void led_timer_cb(void *arg)
{
    (void)arg;
    gpio_set_level((gpio_num_t)PIN_LED_STATUS, 0);
}

/* Light the status LED and (re)arm the one-shot that clears it (DESIGN.md §6.7). */
static void led_status_blip(void)
{
    gpio_set_level((gpio_num_t)PIN_LED_STATUS, 1);
    esp_timer_stop(s_led_timer); /* harmless if not running */
    esp_timer_start_once(s_led_timer, FDC_STATUS_LED_MS * 1000);
}

/* Clear all drive LEDs, then light `drive` if it is a real drive number. */
static void led_drive_select(int drive)
{
    for (int i = 0; i < DISK_MAX_DRIVE; ++i) {
        gpio_set_level(s_drive_leds[i], (i == drive) ? 1 : 0);
    }
}

static void led_drive_clear(void)
{
    for (int i = 0; i < DISK_MAX_DRIVE; ++i) {
        gpio_set_level(s_drive_leds[i], 0);
    }
}

/* ---- stats ----------------------------------------------------------------- */

static inline void stat_lock(void)   { xSemaphoreTake(s_stats_lock, portMAX_DELAY); }
static inline void stat_unlock(void) { xSemaphoreGive(s_stats_lock); }

static void set_last_op(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_last_op(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    stat_lock();
    vsnprintf(s_stats.last_op, sizeof s_stats.last_op, fmt, ap);
    stat_unlock();
    va_end(ap);
}

void fdc_get_stats(fdc_stats_t *out)
{
    if (!out) {
        return;
    }
    stat_lock();
    *out = s_stats;
    stat_unlock();
}

void fdc_clear_stats(void)
{
    stat_lock();
    memset(&s_stats, 0, sizeof s_stats);
    stat_unlock();
}

/* ---- UART framing ---------------------------------------------------------- */

/*
 * Read up to `need` bytes into `buf` within `timeout_ms`, returning how many arrived.
 * The caller checks the count against what it required (a short read is a timeout).
 */
static size_t recv(uint8_t *buf, size_t need, uint32_t timeout_ms)
{
    size_t got = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (got < need) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            break;
        }
        int r = uart_read_bytes(s_port, buf + got, need - got, deadline - now);
        if (r > 0) {
            got += (size_t)r;
        } else if (r < 0) {
            break;
        }
    }
    return got;
}

static void send(const uint8_t *buf, size_t len)
{
    uart_write_bytes(s_port, (const char *)buf, len);
}

/* Build a 10-byte response block (mnemonic + two words + checksum) into s_resp. */
static void build_resp(const char cmd[FDC_CMD_LEN], uint16_t word1, uint16_t word2)
{
    memcpy(&s_resp[FDC_OFF_CMD], cmd, FDC_CMD_LEN);
    fdc_write_le16(&s_resp[FDC_OFF_WORD1], word1);
    fdc_write_le16(&s_resp[FDC_OFF_WORD2], word2);
    fdc_block_finalize(s_resp);
}

/* ---- command handlers ------------------------------------------------------ */

static void handle_stat(void)
{
    uint16_t word1 = fdc_read_le16(&s_cmd[FDC_OFF_WORD1]);
    uint8_t  sel   = (uint8_t)(word1 & 0xFF);   /* selected drive, 0xff = none */
    uint8_t  head  = (uint8_t)(word1 >> 8);     /* nonzero = head loaded */

    /* Reflect the selected drive on the LEDs while the head is loaded (§6.7). */
    if (head && sel < DISK_MAX_DRIVE) {
        led_drive_select(sel);
    } else if (sel == 0xFF) {
        led_drive_clear();
    }

    uint16_t bitmap = disk_status_bitmap();
    build_resp("STAT", FDC_RESP_OK, bitmap);
    send(s_resp, FDC_BLOCK_LEN);

    stat_lock();
    s_stats.stat++;
    stat_unlock();
    set_last_op("STAT sel=%u map=0x%04x", sel, bitmap);
}

/* Last drive/track a READ served, to flag the FDC re-reading the same track (§6.4). */
static int      s_last_read_drive = -1;
static uint16_t s_last_read_track;

static void handle_read(void)
{
    uint16_t word1 = fdc_read_le16(&s_cmd[FDC_OFF_WORD1]);
    uint16_t len   = fdc_read_le16(&s_cmd[FDC_OFF_WORD2]);
    int      drive = fdc_word1_drive(word1);
    uint16_t track = fdc_word1_track(word1);

    led_drive_select(drive);

    /* A READ repeating the previous one means the FDC rejected our last response
     * (bad CRC on its side, or a timeout) and is re-reading — the only signal the
     * passive server has for a failed READ, since the FDC sends no error packet. */
    if (drive == s_last_read_drive && track == s_last_read_track) {
        stat_lock();
        s_stats.read_retry++;
        stat_unlock();
    }
    s_last_read_drive = drive;
    s_last_read_track = track;

    esp_err_t err = disk_read_track(drive, track, len);
    if (err != ESP_OK) {
        /* Passive responder: no reply on a not-ready/out-of-range READ; FDC retries. */
        stat_lock();
        s_stats.not_ready++;
        stat_unlock();
        set_last_op("READ d%d t%u len%u not-ready", drive, track, len);
        return;
    }

    int      d;
    uint32_t t;
    size_t   l;
    const uint8_t *buf = disk_track_buffer(&d, &t, &l);

    /* Payload = <len> data bytes followed by their 16-bit LE checksum (§6.2). */
    send(buf, len);
    uint8_t ck[FDC_CHECKSUM_LEN];
    fdc_write_le16(ck, fdc_checksum16(buf, len));
    send(ck, sizeof ck);

    stat_lock();
    s_stats.read++;
    stat_unlock();
    set_last_op("READ d%d t%u len%u ok", drive, track, len);
}

static void handle_writ(void)
{
    uint16_t word1 = fdc_read_le16(&s_cmd[FDC_OFF_WORD1]);
    uint16_t len   = fdc_read_le16(&s_cmd[FDC_OFF_WORD2]);
    int      drive = fdc_word1_drive(word1);
    uint16_t track = fdc_word1_track(word1);

    led_drive_select(drive);

    /* Ready iff the drive is mounted read-write and the length fits our track buffer.
     * A read-only (remote) drive answers Not Ready to WRIT (DESIGN.md §10.1). */
    bool ready = (len > 0 && len <= DISK_TRACK_BUF_SIZE && disk_is_mounted(drive) &&
                  !disk_is_readonly(drive));
    build_resp("WRIT", ready ? FDC_RESP_OK : FDC_RESP_NOT_READY, 0);
    send(s_resp, FDC_BLOCK_LEN);

    if (!ready) {
        stat_lock();
        s_stats.not_ready++;
        stat_unlock();
        set_last_op("WRIT d%d t%u len%u not-ready", drive, track, len);
        return;
    }

    /* Receive the track payload + its 16-bit checksum (§6.2, longer data window). */
    if (recv(s_data, len, FDC_DATA_TIMEOUT_MS) != len) {
        uart_flush_input(s_port);
        stat_lock();
        s_stats.data_timeout++;
        stat_unlock();
        set_last_op("WRIT d%d t%u len%u data-timeout", drive, track, len);
        return;
    }
    uint8_t ck[FDC_CHECKSUM_LEN];
    if (recv(ck, sizeof ck, FDC_CMD_TIMEOUT_MS) != sizeof ck) {
        uart_flush_input(s_port);
        stat_lock();
        s_stats.data_timeout++;
        stat_unlock();
        set_last_op("WRIT d%d t%u len%u csum-timeout", drive, track, len);
        return;
    }

    uint8_t code;
    if (fdc_read_le16(ck) != fdc_checksum16(s_data, len)) {
        code = FDC_RESP_CSUM_ERR; /* bad write data -> code 2, not silent (§6.4) */
        stat_lock();
        s_stats.data_csum++;
        stat_unlock();
        set_last_op("WRIT d%d t%u len%u csum-err", drive, track, len);
    } else if (disk_write_track(drive, track, len, s_data) != ESP_OK) {
        code = FDC_RESP_WRITE_ERR;
        stat_lock();
        s_stats.not_ready++;
        stat_unlock();
        set_last_op("WRIT d%d t%u len%u write-err", drive, track, len);
    } else {
        code = FDC_RESP_OK;
        stat_lock();
        s_stats.writ++;
        stat_unlock();
        set_last_op("WRIT d%d t%u len%u ok", drive, track, len);
    }

    build_resp("WSTA", code, 0);
    send(s_resp, FDC_BLOCK_LEN);
}

static void dispatch(void)
{
    led_status_blip();
    if (memcmp(s_cmd, "STAT", FDC_CMD_LEN) == 0) {
        handle_stat();
    } else if (memcmp(s_cmd, "READ", FDC_CMD_LEN) == 0) {
        handle_read();
    } else if (memcmp(s_cmd, "WRIT", FDC_CMD_LEN) == 0) {
        handle_writ();
    } else {
        stat_lock();
        s_stats.unknown++;
        stat_unlock();
        set_last_op("unknown '%.4s'", (const char *)s_cmd);
        ESP_LOGD(TAG, "unknown command '%.4s'", (const char *)s_cmd);
    }
}

/* ---- loopback self-test (run on the task so it owns the UART, §6.8) --------- */

static void run_loopback(void)
{
    uint8_t tx[FDC_LOOPBACK_LEN];
    uint8_t rx[FDC_LOOPBACK_LEN];
    for (int i = 0; i < FDC_LOOPBACK_LEN; ++i) {
        tx[i] = (uint8_t)i;
    }

    uart_flush_input(s_port);
    send(tx, sizeof tx);
    uart_wait_tx_done(s_port, pdMS_TO_TICKS(1000));
    size_t got = recv(rx, sizeof rx, 1000);

    uint32_t mism = 0, first_bad = FDC_LOOPBACK_LEN;
    for (int i = 0; i < FDC_LOOPBACK_LEN; ++i) {
        bool bad = (i >= (int)got) || (rx[i] != tx[i]);
        if (bad) {
            ++mism;
            if (first_bad == FDC_LOOPBACK_LEN) {
                first_bad = (uint32_t)i;
            }
        }
    }

    s_lb_result.ran = true;
    s_lb_result.sent = FDC_LOOPBACK_LEN;
    s_lb_result.received = (uint32_t)got;
    s_lb_result.mismatches = mism;
    s_lb_result.first_bad = first_bad;
}

esp_err_t fdc_loopback(fdc_loopback_t *out)
{
    if (!s_task || !out) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lb_lock, portMAX_DELAY);
    s_lb_req = true;
    esp_err_t err;
    /* Task services the request between commands, within one idle-poll window + I/O. */
    if (xSemaphoreTake(s_lb_done, pdMS_TO_TICKS(3000)) == pdTRUE) {
        *out = s_lb_result;
        err = ESP_OK;
    } else {
        s_lb_req = false;
        err = ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_lb_lock);
    return err;
}

/* ---- task ------------------------------------------------------------------ */

static void apply_pending_baud(void)
{
    uint32_t pb = s_pending_baud;
    if (pb) {
        s_pending_baud = 0;
        if (uart_set_baudrate(s_port, pb) == ESP_OK) {
            s_baud = pb;
            uart_flush_input(s_port);
            ESP_LOGI(TAG, "FDC+ link baud set to %lu", (unsigned long)pb);
        }
    }
}

/*
 * Drain the UART driver's event queue, counting hardware-level RX faults so `stats`
 * can name the mechanism behind a protocol checksum/timeout: a FIFO/ring overflow
 * (bytes lost because the ISR was starved) vs a framing error (bits mis-sampled, i.e.
 * a baud mismatch or electrical/noise problem). Called between transactions, so a
 * flush on overflow only discards an already-corrupt stream (resync, DESIGN.md §6.4).
 */
static void drain_uart_events(void)
{
    uart_event_t ev;
    while (xQueueReceive(s_uart_evt_q, &ev, 0) == pdTRUE) {
        switch (ev.type) {
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            uart_flush_input(s_port);
            stat_lock();
            s_stats.fifo_ovf++;
            stat_unlock();
            break;
        case UART_FRAME_ERR:
        case UART_PARITY_ERR:
            stat_lock();
            s_stats.frame_err++;
            stat_unlock();
            break;
        default: /* UART_DATA, UART_BREAK, etc.: not a fault, ignore */
            break;
        }
    }
}

/*
 * Install and configure UART2 for the FDC+ link. Called from the fdc task so the
 * driver's RX interrupt is allocated on core 1 (APP), off the WiFi/lwIP core where the
 * ISR could be delayed long enough to overrun the 128-byte HW FIFO (DESIGN.md §5.2).
 */
static esp_err_t fdc_uart_start(void)
{
    uart_config_t uc = {
        .baud_rate = (int)s_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(s_port, FDC_UART_RX_BUF, 0,
                                        FDC_UART_EVT_QUEUE, &s_uart_evt_q, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(s_port, &uc);
    if (err != ESP_OK) {
        return err;
    }
    return uart_set_pin(s_port, PIN_FDC_UART_TX, PIN_FDC_UART_RX,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static void fdc_task(void *arg)
{
    (void)arg;
    if (fdc_uart_start() != ESP_OK) {
        ESP_LOGE(TAG, "FDC+ UART start failed; task exiting");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "FDC+ engine up on UART%d (RX=%d TX=%d) core %d @ %lu baud", s_port,
             PIN_FDC_UART_RX, PIN_FDC_UART_TX, xPortGetCoreID(), (unsigned long)s_baud);

    for (;;) {
        apply_pending_baud();
        drain_uart_events();

        if (s_lb_req) {
            run_loopback();
            s_lb_req = false;
            xSemaphoreGive(s_lb_done);
            continue;
        }

        /* Idle-poll for the first byte so we stay responsive to baud/loopback. */
        int r = uart_read_bytes(s_port, s_cmd, 1, pdMS_TO_TICKS(FDC_IDLE_POLL_MS));
        if (r <= 0) {
            continue;
        }

        /* Complete the 10-byte command block within the command window. */
        if (recv(&s_cmd[1], FDC_BLOCK_LEN - 1, FDC_CMD_TIMEOUT_MS) != FDC_BLOCK_LEN - 1) {
            uart_flush_input(s_port);
            stat_lock();
            s_stats.cmd_timeout++;
            stat_unlock();
            continue;
        }

        /* Ignore a bad command checksum (the FDC will retry), §6.4. */
        if (!fdc_block_valid(s_cmd)) {
            stat_lock();
            s_stats.cmd_csum++;
            stat_unlock();
            continue;
        }

        dispatch();
    }
}

/* ---- init / config --------------------------------------------------------- */

esp_err_t fdc_set_baud(uint32_t baud)
{
    s_pending_baud = baud; /* picked up by the task */
    return ESP_OK;
}

uint32_t fdc_baud(void)
{
    return s_baud;
}

esp_err_t fdc_init(void)
{
    s_baud = config_get()->baud_rate;

    s_stats_lock = xSemaphoreCreateMutex();
    s_lb_lock = xSemaphoreCreateMutex();
    s_lb_done = xSemaphoreCreateBinary();
    if (!s_stats_lock || !s_lb_lock || !s_lb_done) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t targs = {
        .callback = led_timer_cb,
        .name = "fdc-led",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_led_timer));

    /* UART2 is installed by the task itself (fdc_uart_start) so its RX interrupt lands
     * on core 1, off the WiFi/lwIP core (DESIGN.md §5.2/§6.1). */
    BaseType_t ok = xTaskCreatePinnedToCore(fdc_task, "fdc", FDC_TASK_STACK, NULL,
                                            FDC_TASK_PRIO, &s_task, FDC_TASK_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
