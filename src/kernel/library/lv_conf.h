/**
 * @file lv_conf.h
 * Equinox OS-specific LVGL v8.3 configuration.
 *
 * Copy this file NEXT TO lvgl/src/  (i.e. kernel/library/lv_conf.h)
 * so the compiler finds it via  -I$(KERNEL_DIR)/library
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH          32
#define LV_COLOR_32_SWAP        0
#define LV_COLOR_SCREEN_TRANSP  0
#define LV_COLOR_MIX_ROUND_OFS  (LV_COLOR_DEPTH == 32 ? 0 : 128)
/* LV_COLOR_CHROMA_KEY: hardcoded so we don't use the LV_COLOR_MAKE macro
 * (which expands to `{{...}}` without a cast -> an error in plain C gcc).
 * lv_color_t di LV_COLOR_DEPTH=32 = union lv_color32_t { struct {blue,green,red,alpha} ch; uint32_t full; }
 * Jadi `(lv_color_t){{0, 0, 0, 0xff}}` = compound literal valid C99. */
#define LV_COLOR_CHROMA_KEY     ((lv_color_t){{0x00, 0x00, 0x00, 0xff}})

/*====================
   MEMORY
 *====================*/
/* Use LVGL built-in memory manager (static buffer).
 * This bypasses Equinox OS's bump allocator which can't free. */
#define LV_MEM_CUSTOM       0
#define LV_MEM_SIZE         (384U * 1024U)   /* 384 KB internal pool */
#define LV_MEM_ADR          0
#define LV_MEM_BUF_MAX_NUM  16

/*====================
   HAL — TICK
 *====================*/
/* We call lv_tick_inc() manually from our timer ISR. */
#define LV_TICK_CUSTOM       0

/*====================
   DISPLAY
 *====================*/
#define LV_DISP_DEF_REFR_PERIOD  10     /* ms between refreshes */
#define LV_DISP_HOR_RES          1024   /* default, overridden at runtime */
#define LV_DISP_VER_RES          768

/*====================
   INPUT DEVICE
 *====================*/
#define LV_INDEV_DEF_READ_PERIOD  10     /* ms */
#define LV_INDEV_DEF_DRAG_LIMIT   10
#define LV_INDEV_DEF_DRAG_THROW   10
#define LV_INDEV_DEF_LONG_PRESS_TIME  400
#define LV_INDEV_DEF_LONG_PRESS_REP_TIME  100
/* LV_INDEV_DEF_SCROLL_LIMIT — let default (10) from lv_hal_indev.h */
#define LV_INDEV_DEF_SCROLL_DIR       LV_DIR_VER
#define LV_INDEV_DEF_ZOOM_FACTOR      5

/*====================
   FEATURE CONFIGURATION
 *====================*/
#define LV_USE_ANIMIMG     1
#define LV_USE_ARC          1
#define LV_USE_BAR          1
#define LV_USE_BTN          1
#define LV_USE_BTNMATRIX    1
#define LV_USE_CALENDAR     0
#define LV_USE_CANVAS       1
#define LV_USE_CHECKBOX     1
#define LV_USE_DROPDOWN     1
#define LV_USE_IMG          1
#define LV_USE_IMGBTN       1
#define LV_USE_KEYBOARD     1
#define LV_USE_LABEL        1
#define LV_USE_LINE         1
#define LV_USE_ROLLER       1
#define LV_USE_SLIDER       1
#define LV_USE_SWITCH       1
#define LV_USE_TEXTAREA     1
#define LV_USE_TABLE        1
#define LV_USE_TABVIEW      1
#define LV_USE_TILEVIEW     0
#define LV_USE_WIN          1
#define LV_USE_SPAN         0
#define LV_USE_SPINBOX      0
#define LV_USE_SPINNER      0
#define LV_USE_CHART        1
#define LV_USE_COLORWHEEL   0
#define LV_USE_IMGBTN       1
#define LV_USE_LED          1
#define LV_USE_METER        1
#define LV_USE_MSGBOX       1
#define LV_USE_SPAN         0
#define LV_USE_LIST         1   /* file manager widget */
#define LV_USE_MENU         0
#define LV_USE_ROLLER       1
#define LV_USE_OBJX_SETTINGS 1

/*====================
   LAYOUTS
 *====================*/
#define LV_USE_FLEX         1   /* flex layout (auto arrange widgets) */
#define LV_USE_GRID         1   /* grid layout */

/*====================
   DRAW FEATURES
 *====================*/
/* Complex draw = shadow + gradient + rounded corners + masks.
 * MUST be 1 if you want the fancy UI look (rounded cards, drop shadows). */
#define LV_DRAW_COMPLEX     1
#if LV_DRAW_COMPLEX != 0
    #define LV_SHADOW_CACHE_SIZE 0
#endif

/*====================
   THINGS WE DON'T NEED
 *====================*/
#define LV_USE_GPU          0
#define LV_USE_GPU_STM32_DMA2D  0
#define LV_USE_GPU_NXP_PXP     0
#define LV_USE_GPU_NXP_VG_LITE 0
#define LV_USE_GPU_SDL         0

#define LV_USE_FILESYSTEM       0
#define LV_USE_FS_STDIO         0
#define LV_USE_FS_FATFS         0
#define LV_USE_FS_POSIX         0

#define LV_USE_SNAPSHOT    0
#define LV_USE_MONKEY      0
#define LV_USE_FONT_LOADER 0
#define LV_USE_FONT_FMT_TXT 1
#define LV_USE_FONT_SUBPX   0

/*====================
   TEXT SHADOW / OUTLINE
 *====================*/
#define LV_TXT_ENC              LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS      " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN  0
#define LV_TXT_COLOR_CMD        "#"

/*====================
   FONTS
 *====================*/
#define LV_FONT_DEFAULT         &lv_font_montserrat_14
#define LV_FONT_MONTSERRAT_8    0
#define LV_FONT_MONTSERRAT_10   0
#define LV_FONT_MONTSERRAT_12   1
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_MONTSERRAT_18   0
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_MONTSERRAT_22   0
#define LV_FONT_MONTSERRAT_24   1
#define LV_FONT_MONTSERRAT_26   0
#define LV_FONT_MONTSERRAT_28   0
#define LV_FONT_MONTSERRAT_30   0
#define LV_FONT_MONTSERRAT_32   0
#define LV_FONT_MONTSERRAT_34   0
#define LV_FONT_MONTSERRAT_36   0
#define LV_FONT_MONTSERRAT_38   0
#define LV_FONT_MONTSERRAT_40   0
#define LV_FONT_MONTSERRAT_42   0
#define LV_FONT_MONTSERRAT_44   0
#define LV_FONT_MONTSERRAT_46   0
#define LV_FONT_MONTSERRAT_48   0

#define LV_FONT_MONTSERRAT_12_SUBPX       0
#define LV_FONT_MONTSERRAT_28_COMPRESSED   0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW   0
#define LV_FONT_SIMSUN_16_CJK            0
#define LV_FONT_UNSCII_8                   0
#define LV_FONT_UNSCII_16                  0

/* Bitmap fonts from your own font8x16 — not used by LVGL, */
/* LVGL uses its own vector-font-derived bitmaps.       */

/*====================
   LOGS
 *====================*/
/* Disable LVGL logging — printf goes to our VESA framebuffer
 * which conflicts with LVGL's own rendering. */
#define LV_USE_LOG              0
/* LV_LOG_LEVEL not set here — lv_conf_internal.h sets it to NONE when LV_USE_LOG=0 */
#define LV_LOG_TRACE_MEM        0
#define LV_LOG_TRACE_TIMER      0
#define LV_LOG_TRACE_INDEV      0
#define LV_LOG_TRACE_DISP_REFR  0
#define LV_LOG_TRACE_EVENT      0
#define LV_LOG_TRACE_OBJ_CREATE 0
#define LV_LOG_TRACE_LAYOUT     0
#define LV_LOG_TRACE_ANIM       0

/*====================
   ASSERTS
 *====================*/
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC       1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

/*====================
   ANIMATION
 *====================*/
#define LV_ANIM_DEF_TIME    400   /* ms */

/*====================
   MISC
 *====================*/
#define LV_ATTRIBUTE_FAST_MEM
#define LV_USE_LARGE_COORD    0
#define LV_USE_FLOAT          0

#endif /* LV_CONF_H */
