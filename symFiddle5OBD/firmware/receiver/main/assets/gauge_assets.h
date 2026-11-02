#pragma once
#include "lvgl.h"

typedef struct {
    const char *name;
    const lv_image_dsc_t *img;
} gauge_asset_t;

extern const gauge_asset_t gauge_assets[];
extern const size_t gauge_asset_count;

extern const lv_image_dsc_t bg_day_img;
extern const lv_image_dsc_t bg_night_img;
