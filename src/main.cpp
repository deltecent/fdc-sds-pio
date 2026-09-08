/*
 * main.cpp — app entry point and boot sequence for the FDC+ Serial Disk Server.
 *
 * M0: bring up the console, print the version banner, run the LED lamp test
 * (DESIGN.md §12 steps 1-2). M1: start the serial CLI task. M2: mount the SD card
 * (§12 step 3) and load config from NVS (§12 step 4). Later milestones add the
 * disk module, the FDC engine, and networking to app_main.
 */
#include <stddef.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "cli.h"
#include "config.h"
#include "disk.h"
#include "fdc.h"
#include "net.h"
#include "pins.h"
#include "sd.h"
#include "version.h"

static const char *TAG = "main";

/* Drives 0..3 in order, so a sweep visibly walks left-to-right. */
static const gpio_num_t kDriveLeds[] = {
    (gpio_num_t)PIN_LED_DRIVE0,
    (gpio_num_t)PIN_LED_DRIVE1,
    (gpio_num_t)PIN_LED_DRIVE2,
    (gpio_num_t)PIN_LED_DRIVE3,
};
#define DRIVE_LED_COUNT (sizeof(kDriveLeds) / sizeof(kDriveLeds[0]))

/* Configure the status + drive LEDs as outputs, all off. */
static void leds_init(void)
{
    uint64_t mask = (1ULL << PIN_LED_STATUS);
    for (size_t i = 0; i < DRIVE_LED_COUNT; ++i) {
        mask |= (1ULL << kDriveLeds[i]);
    }

    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    gpio_set_level((gpio_num_t)PIN_LED_STATUS, 0);
    for (size_t i = 0; i < DRIVE_LED_COUNT; ++i) {
        gpio_set_level(kDriveLeds[i], 0);
    }
}

/* Blink each drive LED in sequence once; status LED stays idle (DESIGN.md §12). */
static void led_lamp_test(void)
{
    for (size_t i = 0; i < DRIVE_LED_COUNT; ++i) {
        gpio_set_level(kDriveLeds[i], 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(kDriveLeds[i], 0);
    }
}

static void print_banner(void)
{
    printf("\r\n%s %s\r\n\r\n", FDCSDS_PRODUCT, FDCSDS_VERSION_STRING);
}

extern "C" void app_main(void)
{
    print_banner();

    /* Quiet the console early (§13): drop the IDF INFO default to WARN before the
     * subsystems below start logging. The saved level is re-applied once config
     * loads; the banner above is printf, so it prints regardless. */
    esp_log_level_set("*", ESP_LOG_WARN);

    leds_init();
    led_lamp_test();

    /* §12 step 3: mount SD (non-fatal — file commands report if absent). */
    if (sd_mount() != ESP_OK) {
        ESP_LOGW(TAG, "SD not mounted; file commands unavailable");
    }

    /* §12 step 4: load persistent config (defaults if NVS is empty). */
    ESP_ERROR_CHECK(config_init());

    /* Apply the saved console log level now that config is loaded (§13). */
    esp_log_level_set("*", (esp_log_level_t)config_get()->log_level);

    /* §12 step 5: mount configured drives (SD-backed; tnfs:// deferred to WiFi, M9). */
    ESP_ERROR_CHECK(disk_init());
    disk_automount();

    /* §12: bring up the FDC+ engine (UART2 + high-prio task on core 1) before the CLI. */
    ESP_ERROR_CHECK(fdc_init());

    ESP_LOGI(TAG, "boot complete; starting serial CLI");

    cli_init();
    cli_serial_start();

    /* §12 step 9: networking (WiFi STA + raw-TCP console + mDNS + NTP). Event-driven;
     * connects only if enabled with an SSID set. Non-fatal — serial CLI stays up. */
    if (net_init() != ESP_OK) {
        ESP_LOGW(TAG, "networking init failed; serial console still available");
    }
}
