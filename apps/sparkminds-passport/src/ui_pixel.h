#pragma once

#include "lvgl.h"

#define UI_SKY        0x1689E8
#define UI_SKY_DARK   0x0872C9
#define UI_INK        0x17202A
#define UI_PAPER      0xF4F4EA
#define UI_GRASS      0x82BE2D
#define UI_GRASS_DARK 0x55951D
#define UI_YELLOW     0xFFD928
#define UI_ORANGE     0xFFB23E
#define UI_RED        0xff5252
#define UI_MUTED      0xD9E7EC

// Cyberpunk Dark Theme Colors
#define UI_BG            0x1a1a2e
#define UI_PANEL         0x2d2d44
#define UI_CYAN          0x00d4ff
#define UI_GREEN         0x69f0ae
#define UI_GOLD          0xffd700
#define UI_TEXT          0xe0e0e0
#define UI_TEXT_DIM      0x888888
#define UI_BORDER        0x33334d

lv_obj_t *ui_pixel_screen_create(const char *title);
lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color);
lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color);
lv_obj_t *ui_pixel_mascot_create(lv_obj_t *parent, int x, int y);
void ui_pixel_mascot_jump(lv_obj_t *mascot);
void ui_pixel_set_selected(lv_obj_t *panel, bool selected, bool enabled);
