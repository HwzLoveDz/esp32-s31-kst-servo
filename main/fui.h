/* SPDX-License-Identifier: MIT */
#pragma once
#include "lvgl.h"
#include "app_model.h"

/* Call from the LVGL thread, after the controller and display are initialized.
 * The screen is designed at the Korvo's native 800 x 480 landscape resolution.
 * fui_create owns a 200 ms LVGL refresh timer; deleting its root stops the timer.
 * No ESP-IDF facilities are used here: this exact source also renders headlessly.
 */
void fui_create(lv_obj_t *parent);
void fui_refresh(const app_snapshot_t *snapshot);

/* Useful to applications, accessibility navigation and the headless preview. */
typedef enum { FUI_MONITOR, FUI_MOTION, FUI_SERVICE, FUI_LINK, FUI_LOG } fui_page_t;
void fui_show_page(fui_page_t page);

