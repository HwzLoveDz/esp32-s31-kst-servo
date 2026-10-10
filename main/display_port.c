/* SPDX-License-Identifier: MIT */
#include "fui.h"
#include <string.h>
#include "dev_display_lcd.h"
#include "dev_lcd_touch.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"

static const char *TAG = "kst_display";

esp_err_t display_port_start(void)
{
    ESP_RETURN_ON_ERROR(esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD),
                        TAG, "LCD init");
    ESP_RETURN_ON_ERROR(esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_TOUCH),
                        TAG, "touch init");
    dev_display_lcd_handles_t *lcd = NULL;
    dev_display_lcd_config_t *cfg = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD,
                                                           (void **)&lcd), TAG, "LCD handle");
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD,
                                                           (void **)&cfg), TAG, "LCD config");
    ESP_RETURN_ON_FALSE(lcd && cfg && cfg->lcd_width == 800 && cfg->lcd_height == 480,
                        ESP_ERR_NOT_SUPPORTED, TAG, "expected 800x480 Korvo display");
    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_stack = 16 * 1024;
    port.task_affinity = 0;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port), TAG, "LVGL init");
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd->io_handle, .panel_handle = lcd->panel_handle,
        .buffer_size = 800 * 240, .double_buffer = true, .hres = 800, .vres = 480,
        .rotation = {.swap_xy = cfg->swap_xy, .mirror_x = cfg->mirror_x,
                     .mirror_y = cfg->mirror_y},
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_dma = false, .buff_spiram = true},
    };
    lv_display_t *display = NULL;
#ifdef CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT
    if (strcmp(cfg->sub_type, "rgb") == 0) {
        const lvgl_port_display_rgb_cfg_t rgb = {
            .flags = {.bb_mode = cfg->sub_cfg.rgb.panel_config.bounce_buffer_size_px > 0,
                      .avoid_tearing = cfg->sub_cfg.rgb.panel_config.num_fbs > 1},
        };
        disp_cfg.flags.direct_mode = rgb.flags.avoid_tearing;
        display = lvgl_port_add_disp_rgb(&disp_cfg, &rgb);
    }
#endif
    if (!display) display = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(display, ESP_FAIL, TAG, "LVGL display");
    dev_lcd_touch_handles_t *touch = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH,
                                                           (void **)&touch), TAG, "touch handle");
    ESP_RETURN_ON_FALSE(touch && touch->touch_handle, ESP_FAIL, TAG, "touch unavailable");
    const lvgl_port_touch_cfg_t touch_cfg = {.disp = display, .handle = touch->touch_handle};
    ESP_RETURN_ON_FALSE(lvgl_port_add_touch(&touch_cfg), ESP_FAIL, TAG, "LVGL touch");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(5000), ESP_ERR_TIMEOUT, TAG, "LVGL lock");
    fui_create(lv_display_get_screen_active(display));
    lvgl_port_unlock();
    return ESP_OK;
}
