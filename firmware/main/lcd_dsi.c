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
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "lcd";

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_line[2];     // a strip packed to 565 the picture's way, one a core (cv->helper): the hand path only
static uint16_t *s_col;         // the same turned by hand: DISP_STRIP wide, GFX_PW rows, when the PPA is not there
static uint16_t *s_fb;          // the frame, the panel's: 720 across, 1280 down
static ppa_client_handle_t s_ppa; // the P4's pixel processing accelerator: turns the strip into the frame
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
    s_line[0] = heap_caps_aligned_alloc(64, GFX_PW * DISP_STRIP * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_line[1] = heap_caps_aligned_alloc(64, GFX_PW * DISP_STRIP * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_col  = heap_caps_malloc(GFX_PW * DISP_STRIP * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_line[0] || !s_line[1] || !s_col) {
        ESP_LOGE(TAG, "no memory for a strip");
        s_panel = NULL;
        return;
    }
    /* the P4 has a block turner in hardware (the PPA): it reads the strip, turns it and writes it
     * into the frame, cache and all, while this core goes on. without it the strip is turned by
     * hand and the driver copies it in line by line, 1280 lines a strip */
    void *fb = NULL;
    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM };
    if (esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb) == ESP_OK && fb && ppa_register_client(&pc, &s_ppa) == ESP_OK) {
        s_fb = fb;
    } else {
        s_ppa = NULL;
        ESP_LOGW(TAG, "no PPA: the strips are turned by hand");
    }
    esp_lcd_panel_disp_on_off(s_panel, true);
    bsp_display_brightness_set(0);                     // the package set the backlight's PWM up with the panel                     // main.c brings it up once the first frames are in
    ESP_LOGI(TAG, "MIPI-DSI panel, the glass %dx%d standing, the picture %dx%d lying, turned by %s", BSP_LCD_H_RES, BSP_LCD_V_RES,
             GFX_PW, GFX_PH, s_ppa ? "the PPA" : "hand");
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
    const int h = cv->h, k = cv->helper ? 1 : 0;
    if (s_ppa) {
        /* the PPA reads the canvas as it is, 0x00RRGGBB a pixel, and writes 565 into the frame: the
         * packing (and its dither) is spared, 40 ms a frame at this width. it turns counter-
         * clockwise: 270 puts the picture's left edge at the top of the glass, 90 the other way
         * round. the block it writes is h across and GFX_PW down, at column x0. blocking, a few ms a
         * frame: the core draws the next strip into this same canvas the moment this returns */
        ppa_srm_oper_config_t op = {
            .in = { .buffer = cv->px, .pic_w = GFX_PW, .pic_h = (uint32_t)h, .block_w = GFX_PW, .block_h = (uint32_t)h,
                    .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_ARGB8888 },
            .out = { .buffer = s_fb, .buffer_size = (uint32_t)BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
                     .pic_w = BSP_LCD_H_RES, .pic_h = BSP_LCD_V_RES,
                     .block_offset_x = (uint32_t)(s_flip ? cv->y0 : BSP_LCD_H_RES - cv->y0 - h), .block_offset_y = 0,
                     .srm_cm = PPA_SRM_COLOR_MODE_RGB565 },
            .rotation_angle = s_flip ? PPA_SRM_ROTATION_ANGLE_90 : PPA_SRM_ROTATION_ANGLE_270,
            .scale_x = 1.0f, .scale_y = 1.0f,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        esp_err_t e = ppa_do_scale_rotate_mirror(s_ppa, &op);
        if (e == ESP_OK) return;
        ESP_LOGW(TAG, "the PPA refused a strip: %s, turning by hand from now on", esp_err_to_name(e));
        s_ppa = NULL;
    }
    uint16_t *line = s_line[k];
    gfx_pack565(cv, line);                             // h rows of GFX_PW pixels, the picture's way
    /* the picture's row y goes to the frame's column 719 - y and its x down the frame: the rows of
     * the strip fill columns 720 - y0 - h .. 719 - y0, the last row leftmost. turned over, row y
     * goes to column y and x up the frame. x outer: s_col is written in runs of h, s_line read
     * with a stride, the cheaper way round for a cache */
    int x0;
    if (!s_flip) {
        x0 = BSP_LCD_H_RES - cv->y0 - h;
        for (int x = 0; x < GFX_PW; x++) {
            uint16_t *to = s_col + x * h + h - 1;
            const uint16_t *from = line + x;
            for (int r = 0; r < h; r++, from += GFX_PW) *to-- = *from;
        }
    } else {
        x0 = cv->y0;
        for (int x = 0; x < GFX_PW; x++) {
            uint16_t *to = s_col + (GFX_PW - 1 - x) * h;
            const uint16_t *from = line + x;
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
