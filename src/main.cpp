/*
 * main.cpp — app entry point and boot sequence for the FDC+ Serial Disk Server.
 *
 * M0 skeleton (build plan): bring up the console, print the version banner, and
 * run the LED lamp test (DESIGN.md §12 steps 1-2). Later milestones add SD,
 * config, the disk module, the FDC engine, and networking to app_main.
 */
#include <stddef.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "pins.h"
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

    leds_init();
    led_lamp_test();

    ESP_LOGI(TAG, "M0 skeleton boot complete");
}
