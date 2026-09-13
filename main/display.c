#include "display.h"

#include "board_pins.h"
#include "esp_heap_caps.h"
#include "mem_utils.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static lv_display_t *s_disp;

static void lv_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(5);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int x1 = area->x1;
    const int y1 = area->y1;
    const int x2 = area->x2;
    const int y2 = area->y2;
    esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, px_map);
    lv_display_flush_ready(disp);
}

void display_blit_rgb565(const uint8_t *buf, int x, int y, int w, int h)
{
    if (!s_panel || !buf) {
        return;
    }
    esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, buf);
}

esp_err_t display_init(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * PARTIAL_BUF_LINES * sizeof(uint16_t) + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &s_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        /* LVGL / RLE buffers are native LE RGB565; IDF DMA sends memory order.
         * Arduino bit-banged high-byte-first instead. BIG endian here only sets
         * ST7789 RAMCTRL and leaves the wire bytes swapped, so colors look wrong. */
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    lv_init();

    const size_t partial_bytes = LCD_W * PARTIAL_BUF_LINES * sizeof(lv_color_t);
    /* Prefer SPIRAM only: BT controller needs contiguous internal DRAM. */
    lv_color_t *buf1 = psram_malloc(partial_bytes);
    lv_color_t *buf2 = psram_malloc(partial_bytes);
    if (!buf1 || !buf2) {
        ESP_LOGW(TAG, "PSRAM alloc failed, trying internal (may affect BLE)");
        if (!buf1) {
            buf1 = dram_malloc(partial_bytes);
        }
        if (!buf2) {
            buf2 = dram_malloc(partial_bytes);
        }
    }
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "LVGL buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    s_disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_buffers(s_disp, buf1, buf2, partial_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    const esp_timer_create_args_t tick_args = {
        .callback = lv_tick_cb,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick, 5000));

    ESP_LOGI(TAG, "ST7789 + LVGL ready");
    return ESP_OK;
}
