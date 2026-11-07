/*
 * touch, four kinds:
 *  XPT2046 bit-banged on its own pins (the 2.8" CYD), or on the LCD's SPI bus (3.5" CYD).
 *    check pressure (Z1, Z2) first, a light press counts as nothing, otherwise the resistive
 *    panel invents taps. then 5 reads per axis, median.
 *  CST816 capacitive on I2C (round board), FT6336 the same way (ES3C28P). their axes may not
 *    match the screen, that's fine, the 5 cross alignment in ui.c sorts out swaps and mirrors
 *    for every kind
 *  GT911 capacitive on I2C: the 3.5" CYD's C version, asked for before the XPT2046 of the R
 *    version, and the 5" boards, where it is the only one
 * boards without touch get tp_read() that never presses
 */
#include "touch.h"
#include "board.h"
#include "esp_log.h"

static const char *TAG = "touch";

#if TOUCH_GT911_TOO || TOUCH_GT911
/* GT911, capacitive (board.h). 16-bit register addresses, MSB first. its address is 0x5D or 0x14,
 * picked by the INT level at power on: both are asked, and "911" at 0x8140 tells it is really one.
 * it counts in screen pixels: portrait 320x480 on the 3.5", 800x480 lying down on the 5" */
#include "i2c_bus.h"
#include "esp_timer.h"

static i2c_master_dev_handle_t s_gt;
static int      s_gt_down, s_gt_x, s_gt_y;
static int64_t  s_gt_seen;

static int gt_reg(uint16_t reg, uint8_t *buf, int n)
{
    uint8_t r[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return i2c_master_transmit_receive(s_gt, r, 2, buf, (size_t)n, 20) == ESP_OK;
}

static int gt_find(void)
{
    const uint8_t addr[2] = { 0x5D, 0x14 };
    for (int i = 0; i < 2; i++) {
        if (!i2c_probe(addr[i])) continue;
        s_gt = i2c_dev(addr[i]);
        uint8_t id[4] = { 0 };
        if (gt_reg(0x8140, id, 4) && id[0] == '9' && id[1] == '1' && id[2] == '1') {
            ESP_LOGI(TAG, "GT911 at 0x%02x", addr[i]);
            return 1;
        }
        i2c_master_bus_rm_device(s_gt);              // something else answered, not ours
        s_gt = NULL;
    }
    return 0;
}

/* status 0x814E: bit 7 = fresh data, low bits = how many fingers. after reading it wants a 0 back.
 * while a finger is on it reports about every 10 ms, so a silence of 300 ms counts as lifted */
static int gt_read(int *rx, int *ry)
{
    uint8_t st = 0;
    int64_t now = esp_timer_get_time();
    if (gt_reg(0x814E, &st, 1) && (st & 0x80)) {
        uint8_t p[4];
        if ((st & 0x0F) && gt_reg(0x8150, p, 4)) {
            s_gt_x = p[0] | p[1] << 8;
            s_gt_y = p[2] | p[3] << 8;
            s_gt_down = 1;
        } else {
            s_gt_down = 0;
        }
        const uint8_t clr[3] = { 0x81, 0x4E, 0 };
        i2c_master_transmit(s_gt, clr, 3, 20);
        s_gt_seen = now;
    } else if (s_gt_down && now - s_gt_seen > 300000) {
        s_gt_down = 0;
    }
    if (s_gt_down) { *rx = s_gt_x; *ry = s_gt_y; }
    return s_gt_down;
}
#endif

#if TOUCH_XPT_BITBANG || TOUCH_XPT_SPI
#include "driver/gpio.h"

#define Z_MIN 350            // below this its not a press

static int xfer(uint8_t cmd);
static void chip_select(int on);

/* the pen interrupt: the XPT2046 pulls T_IRQ low while the panel is pressed (in power down with
 * the pen interrupt on, which is how tp_read leaves it). while it is high there is nothing to
 * read. on the 3.5" board that keeps the touch off the LCD's bus: every read there waited for a
 * strip to go out and held the next one back, a third of the frame. a pin that is not wired or
 * lies is found out: it reads low at the start, or a read every 20th sample finds a press it
 * missed. then every sample asks the chip, as before */
static int s_irq;

static void irq_init(void)
{
    gpio_config_t in = { .pin_bit_mask = 1ULL << PIN_T_IRQ, .mode = GPIO_MODE_INPUT };
    gpio_config(&in);
    chip_select(1);
    xfer(0xD0);                                      // power down, pen interrupt on
    chip_select(0);
    s_irq = gpio_get_level(PIN_T_IRQ) == 1;
    ESP_LOGI(TAG, "pen interrupt on GPIO %d: %s", PIN_T_IRQ, s_irq ? "the chip is read only while pressed" : "low, every sample asks the chip");
}

#if TOUCH_XPT_BITBANG
#include "esp_rom_sys.h"

void tp_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_T_CLK) | (1ULL << PIN_T_MOSI) | (1ULL << PIN_T_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_T_MISO) | (1ULL << PIN_T_IRQ),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);
    gpio_set_level(PIN_T_CS, 1);
    gpio_set_level(PIN_T_CLK, 0);
    ESP_LOGI(TAG, "XPT2046 on its own pins");
    irq_init();
}

static void chip_select(int on) { gpio_set_level(PIN_T_CS, !on); }

// send command byte, then clock in 16 bits: busy bit, 12 data bits, 3 zeros
static int xfer(uint8_t cmd)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(PIN_T_MOSI, (cmd >> i) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_T_CLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_T_CLK, 0);
    }
    gpio_set_level(PIN_T_MOSI, 0);
    unsigned v = 0;
    for (int i = 0; i < 16; i++) {
        esp_rom_delay_us(1);
        gpio_set_level(PIN_T_CLK, 1);
        esp_rom_delay_us(1);
        v = (v << 1) | (unsigned)gpio_get_level(PIN_T_MISO);
        gpio_set_level(PIN_T_CLK, 0);
    }
    return (int)((v >> 3) & 0xFFF);
}

#else   /* XPT2046 as a second device on the LCD bus (bus is set up by disp_init) */
#include "driver/spi_master.h"
#include "display.h"

static spi_device_handle_t s_dev;

void tp_init(void)
{
#if TOUCH_GT911_TOO
    if (gt_find()) return;
    i2c_bus_free();                                  // nobody there: 33 is the XPT2046's chip select again
#endif
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 2 * 1000 * 1000,       // the XPT2046 wants it slow
        .mode = 0,
        .spics_io_num = PIN_T_CS,
        .queue_size = 1,
    };
    if (spi_bus_add_device(SPI2_HOST, &dev, &s_dev) != ESP_OK) ESP_LOGE(TAG, "XPT2046 not added to the bus");
    irq_init();
}

static void chip_select(int on) { (void)on; }         // the SPI driver does CS for every transfer

static int xfer(uint8_t cmd)
{
    spi_transaction_t t = { .length = 24, .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA };
    t.tx_data[0] = cmd;
    /* the bus is the screen's too: the strip on its way goes out first, the touch never asks for
     * the bus against it. lcd_spi.c found that waiting in the driver's bus lock can hang */
    disp_wait_idle();
    if (!s_dev || spi_device_polling_transmit(s_dev, &t) != ESP_OK) return 0;
    return (int)(((t.rx_data[1] << 8) | t.rx_data[2]) >> 3) & 0xFFF;
}
#endif

static int median5(int *v)
{
    for (int i = 1; i < 5; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) { int t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    return v[2];
}

int tp_read(int *rx, int *ry)
{
#if TOUCH_GT911_TOO
    if (s_gt) return gt_read(rx, ry);
#endif
    static uint32_t n;
    int idle = s_irq && gpio_get_level(PIN_T_IRQ);
    if (idle && ++n % 20) return 0;                  // not pressed, says the pin. every 20th: ask anyway
    chip_select(1);
    int z1 = xfer(0xB1), z2 = xfer(0xC1);
    int z = z1 + 4095 - z2;
    int pressed = z1 > 0 && z >= Z_MIN;
    if (pressed) {
        int xs[5], ys[5];
        for (int i = 0; i < 5; i++) xs[i] = xfer(0xD1);
        for (int i = 0; i < 5; i++) ys[i] = xfer(0x91);
        *rx = median5(xs);
        *ry = median5(ys);
        pressed = *rx > 50 && *rx < 4050 && *ry > 50 && *ry < 4050;   // values at the very edge are junk
    }
    xfer(0xD0);                                  // power down, pen interrupt on
    chip_select(0);
    if (pressed && idle) {
        s_irq = 0;
        ESP_LOGW(TAG, "a press the pen interrupt did not show: every sample asks the chip from now on");
    }
    return pressed;
}

#elif TOUCH_GT911
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void tp_init(void)
{
#ifdef PIN_T_RST
    /* its reset is a GPIO of its own: INT low through it picks 0x5D. on the CrowPanel the board's
     * own chip has done this by now (lcd_rgb.c) */
    gpio_config_t out = { .pin_bit_mask = 1ULL << PIN_T_RST | 1ULL << PIN_T_INT, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(PIN_T_INT, 0);
    gpio_set_level(PIN_T_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_T_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_config_t in = { .pin_bit_mask = 1ULL << PIN_T_INT, .mode = GPIO_MODE_INPUT };
    gpio_config(&in);
    vTaskDelay(pdMS_TO_TICKS(60));
#endif
    if (!gt_find()) ESP_LOGW(TAG, "no GT911, going on without touch (BOOT button still works)");
}

int tp_read(int *rx, int *ry) { return s_gt ? gt_read(rx, ry) : 0; }

#elif TOUCH_CST816 || TOUCH_FT6336
/* the CST816 (round board) and the FT6336 (ES3C28P, CrowPanel Advance) give the first finger
 * the same way: from register 0x02 on, the count of fingers, X high and low, Y high and low */
#include "i2c_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if TOUCH_FT6336
#include "driver/gpio.h"
#define TOUCH_ADDR FT6336_ADDR
#define TOUCH_CHIP "FT6336"
#else
#define TOUCH_ADDR CST816_ADDR
#define TOUCH_CHIP "CST816"
#endif

static i2c_master_dev_handle_t s_dev;

void tp_init(void)
{
#if TOUCH_FT6336
    // its reset is a GPIO of its own: held low a moment, then up, and the chip wants a while
    // after that before it answers
    gpio_config_t rst = { .pin_bit_mask = 1ULL << PIN_T_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(PIN_T_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_T_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
#else
    // the expander let the touch out of reset in disp_init, give it a moment to wake
    vTaskDelay(pdMS_TO_TICKS(50));
#endif
    if (!i2c_probe(TOUCH_ADDR)) {
        ESP_LOGW(TAG, "nothing at 0x%02x, going on without touch (BOOT button still works)", TOUCH_ADDR);
        return;
    }
    s_dev = i2c_dev(TOUCH_ADDR);
#if TOUCH_CST816
    // the CST816 dozes off after a few idle seconds and then ignores I2C untill touched. 0xFE = 1 keeps it awake
    i2c_master_transmit(s_dev, (const uint8_t[]){ 0xFE, 0x01 }, 2, 50);
#endif
    ESP_LOGI(TAG, TOUCH_CHIP " at 0x%02x", TOUCH_ADDR);
}

int tp_read(int *rx, int *ry)
{
    if (!s_dev) return 0;
    uint8_t reg = 0x02, b[5];                     // finger count, X high, X low, Y high, Y low
    if (i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof b, 20) != ESP_OK) return 0;
    const int fingers = b[0] & 0x0F;
    if (!fingers) return 0;
#if TOUCH_FT6336
    if (fingers > 2) return 0;                    // it knows two at most: more is a register not ready yet
#endif
    *rx = ((b[1] & 0x0F) << 8) | b[2];
    *ry = ((b[3] & 0x0F) << 8) | b[4];
    return 1;
}

#elif TOUCH_BSP
/* the Tab5: the board package found the touch chip with the panel (lcd_dsi.c) and reads it through
 * esp_lcd_touch. it counts the standing glass's pixels, 720 x 1280; the 5 cross alignment turns them */
#include "esp_lcd_touch.h"      // before the package's touch.h, which wants its type
#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"

static esp_lcd_touch_handle_t s_tp;

void tp_init(void)
{
    esp_err_t e = bsp_touch_new(NULL, &s_tp);
    if (e != ESP_OK) {
        s_tp = NULL;
        ESP_LOGW(TAG, "no touch: %s, going on without", esp_err_to_name(e));
    }
}

int tp_read(int *rx, int *ry)
{
    if (!s_tp) return 0;
    esp_lcd_touch_point_data_t p;
    uint8_t n = 0;
    esp_lcd_touch_read_data(s_tp);
    if (esp_lcd_touch_get_data(s_tp, &p, &n, 1) != ESP_OK || !n) return 0;
    *rx = p.x;
    *ry = p.y;
    return 1;
}

#else

void tp_init(void) { ESP_LOGI(TAG, "this board has no touch"); }
int tp_read(int *rx, int *ry) { (void)rx; (void)ry; return 0; }

#endif

int tp_raw_span(void)
{
#if TOUCH_GT911_TOO
    if (s_gt) return 480;                            // the GT911 counts screen pixels
#endif
    return TOUCH_RAW_SPAN;
}
