/*
 * pins.h — ESP32 GPIO assignments. WIRE CONTRACT (DESIGN.md §2.1).
 *
 * These must match the hardware and the FDC+/SD wiring exactly. Do not change
 * without updating DESIGN.md §2.1 and the board.
 *
 *   FDC+ UART  = UART2 (RX GPIO16 from FDC+ TXD, TX GPIO17 to FDC+ RXD)
 *   microSD    = VSPI  (CS 5, CLK 18, MISO 19, MOSI 23)
 *   Drive LEDs = active HIGH (GPIO27/14/12/13 for drives 0..3)
 *   Status LED = built-in (GPIO2)
 *   Console    = UART0 over USB @115200 8N1
 */
#ifndef FDCSDS_PINS_H
#define FDCSDS_PINS_H

/* FDC+ serial link — UART2. */
#define PIN_FDC_UART_RX 16 /* from FDC+ TXD */
#define PIN_FDC_UART_TX 17 /* to   FDC+ RXD */
#define FDC_UART_NUM 2

/* microSD over VSPI. */
#define PIN_SD_CS 5
#define PIN_SD_CLK 18
#define PIN_SD_MISO 19
#define PIN_SD_MOSI 23

/* Drive-activity LEDs (active HIGH), drives 0..3. */
#define PIN_LED_DRIVE0 27
#define PIN_LED_DRIVE1 14
#define PIN_LED_DRIVE2 12
#define PIN_LED_DRIVE3 13

/* Status LED (built-in). */
#define PIN_LED_STATUS 2

#endif /* FDCSDS_PINS_H */
