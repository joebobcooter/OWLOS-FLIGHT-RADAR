/*
 * the M5Stack Tab5: a 720x1280 panel standing on the ESP32-P4's MIPI-DSI, through Espressif's board
 * package (espressif/m5stack_tab5), which tells the three revisions of the panel apart at power on
 * and brings up the one it finds. as on the RGB-bus boards (lcd_rgb.c) the chip reads a whole frame
 * out of the PSRAM all the time, and a strip is only put in that frame. but here the glass stands
 * and the radar lies: a strip of the picture's rows is turned on its side into a column of the
 * frame, h pixels wide and the picture's width tall, and the driver copies that in (and keeps the
 * P4's cache straight, which a write into the frame by hand would not).
 * written on a Tab5 with the ST7121 panel (board V1.0); the ILI9881C and ST7123 ones not seen
 */
#include "display.h"
#if DISP_LCD_DSI

#include <string.h>
#include "bsp/m5stack_tab5.h"
#include "bsp/display.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "lcd";

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_line;        // a strip packed to 565 the picture's way: GFX_PW wide, DISP_STRIP rows
static uint16_t *s_col;         // the same turned: DISP_STRIP wide, GFX_PW rows, what the driver takes
static int s_flip, s_up;

/* the package's I2C bus and the switches on the board's IO expanders. the radio's is thrown here
 * because this is the one file that knows the board: app_main calls it before the WiFi starts */
void disp_board_up(void)
{
    if (s_up) return;
    s_up = 1;
    esp_err_t e = bsp_i2c_init();
    if (e != ESP_OK) ESP_LOGE(TAG, "the board's I2C bus did not come up: %s", esp_err_to_name(e));
    e = bsp_feature_enable(BSP_FEATURE_WIFI, true);
    if (e != ESP_OK) ESP_LOGW(TAG, "the radio's power switch: %s", esp_err_to_name(e));
}

void disp_backlight(int percent)
{
    if (!s_panel) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    bsp_display_brightness_set(percent);
}

void disp_init(void)
{
    disp_board_up();
    const bsp_display_config_t cfg = {
        .dsi_bus = { .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT, .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS },
    };
    bsp_lcd_handles_t h = { 0 };
    /* a board the package does not know ends here: said in the log and the rest runs, the site can
     * still reach the board */
    esp_err_t e = bsp_display_new_with_handles(&cfg, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "no DSI panel: %s", esp_err_to_name(e));
        return;
    }
    s_panel = h.panel;
    s_line = heap_caps_malloc(GFX_PW * DISP_STRIP * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    s_col  = heap_caps_malloc(GFX_PW * DISP_STRIP * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_line || !s_col) {
        ESP_LOGE(TAG, "no memory for a strip");
        s_panel = NULL;
        return;
    }
    esp_lcd_panel_disp_on_off(s_panel, true);
    bsp_display_brightness_set(0);                     // the package set the backlight's PWM up with the panel                     // main.c brings it up once the first frames are in
    ESP_LOGI(TAG, "MIPI-DSI panel, the glass %dx%d standing, the picture %dx%d lying", BSP_LCD_H_RES, BSP_LCD_V_RES, GFX_PW, GFX_PH);
}

void disp_flip(int on) { s_flip = on ? 1 : 0; }

/* into the frame, turned. the frame stands: BSP_LCD_H_RES (720) across, GFX_PW (1280) down. the
 * strip's h rows (the picture's rows cv->y0 .. cv->y0 + h - 1) become h columns of it, side by
 * side, and the driver takes them as one rectangle h wide and GFX_PW tall: in s_col each of its
 * GFX_PW rows holds the h pixels of one column, left to right. the picture's left edge goes to
 * the top of the glass; turned over (disp_flip) it goes the other way, as on every other board */
static void put(const canvas_t *cv)
{
    if (!s_panel) return;
    const int h = cv->h;
    gfx_pack565(cv, s_line);                           // h rows of GFX_PW pixels, the picture's way
    /* the picture's row y goes to the frame's column 719 - y and its x down the frame: the rows of
     * the strip fill columns 720 - y0 - h .. 719 - y0, the last row leftmost. turned over, row y
     * goes to column y and x up the frame. x outer: s_col is written in runs of h, s_line read
     * with a stride, the cheaper way round for a cache */
    int x0;
    if (!s_flip) {
        x0 = BSP_LCD_H_RES - cv->y0 - h;
        for (int x = 0; x < GFX_PW; x++) {
            uint16_t *to = s_col + x * h + h - 1;
            const uint16_t *from = s_line + x;
            for (int r = 0; r < h; r++, from += GFX_PW) *to-- = *from;
        }
    } else {
        x0 = cv->y0;
        for (int x = 0; x < GFX_PW; x++) {
            uint16_t *to = s_col + (GFX_PW - 1 - x) * h;
            const uint16_t *from = s_line + x;
            for (int r = 0; r < h; r++, from += GFX_PW) *to++ = *from;
        }
    }
    esp_lcd_panel_draw_bitmap(s_panel, x0, 0, x0 + h, GFX_PW, s_col);
}

void disp_strip(const canvas_t *cv) { put(cv); }
/* the second core's strip goes in by itself, nothing is in flight on this bus to wait for */
void disp_pack_under(const canvas_t *under) { put(under); }
void disp_strip2(const canvas_t *cv, int under_rows) { (void)under_rows; put(cv); }
int disp_rows(int rows) { (void)rows; return 1; }

void disp_frame_end(void) { }
uint32_t disp_frame_ms(void) { return 0; }
void disp_full_next(void) { }
uint32_t disp_lost(void) { return 0; }
void disp_wait_idle(void) { }

#endif
