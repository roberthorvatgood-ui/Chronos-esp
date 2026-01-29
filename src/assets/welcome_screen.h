
// src/assets/welcome_screen.h
#pragma once

// Match the include strategy used by welcome_screen.c
#ifdef __has_include
  #if __has_include("lvgl.h")
    #ifndef LV_LVGL_H_INCLUDE_SIMPLE
      #define LV_LVGL_H_INCLUDE_SIMPLE
    #endif
  #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
  #include "lvgl.h"
#else
  #include "lvgl/lvgl.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Descriptor defined in welcome_screen.c
extern const lv_img_dsc_t welcome_screen;

#ifdef __cplusplus
}
#endif
