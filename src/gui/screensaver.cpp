
#include <lvgl.h>
#include "screensaver.h"
#include "../drivers/hal_panel.h"
#include "../intl/i18n.h"
#include "src/assets/fonts/ui_font_16.h"

#if __has_include("../assets/welcome_screen.h")
 #include "../assets/welcome_screen.h"
#elif __has_include("assets/welcome_screen.h")
 #include "assets/welcome_screen.h"
#else
 #error "welcome_screen.h not found. Place it under src/assets/ and include here."
#endif

// ─────────────────────────────────────────────────────────────
// Build-time tuning
// ─────────────────────────────────────────────────────────────
#ifndef SCREENSAVER_FORCE_FULL_REFRESH
#define SCREENSAVER_FORCE_FULL_REFRESH 0
#endif

#ifndef SCREENSAVER_REFRESH_MS
#define SCREENSAVER_REFRESH_MS 16 // ~60 Hz
#endif

// [2026-01-25 12:30 CET] AP-web hold configuration
// While AP-web hold is active, screensaver_show() is a no-op.
static bool s_apweb_hold = false;

// ─────────────────────────────────────────────────────────────
// Internal state
// ─────────────────────────────────────────────────────────────
static lv_obj_t*   s_overlay    = nullptr; // full-screen container on lv_layer_top()
static lv_obj_t*   s_img        = nullptr; // welcome image child (zoomed, drifting)
static lv_timer_t* s_refr_timer = nullptr; // optional full-frame invalidation timer
static bool        s_active     = false;   // logical saver state
static lv_obj_t*   s_hint_label = nullptr; // Localized "Touch to wake" label

// Drift animations (3 axes: X, Y, micro Y via style translate)
static lv_anim_t   s_animX, s_animY, s_animMicro;

// Forward declaration for core hide (used by hold gate and public hide)
static void ss_hide_core_internal(void);

// style opacity animator for fades (overlay-level fade)
static void set_obj_opa(void* obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
}

// image move anim exec callbacks (absolute position for X/Y drift)
static void anim_set_x(void* obj, int32_t v) { lv_obj_set_x((lv_obj_t*)obj, v); }
static void anim_set_y(void* obj, int32_t v) { lv_obj_set_y((lv_obj_t*)obj, v); }

// micro-motion: additive vertical offset using style translate_y (composes with Y)
static void anim_set_translate_y(void* obj, int32_t v) {
    lv_obj_set_style_translate_y((lv_obj_t*)obj, v, 0 /* LV_PART_MAIN|LV_STATE_DEFAULT */);
}

static inline bool obj_valid(lv_obj_t* o) {
#if defined(LVGL_VERSION_MAJOR) && LVGL_VERSION_MAJOR >= 8
    return o && lv_obj_is_valid(o);
#else
    return o != nullptr;
#endif
}

void screensaver_refresh_language() {
    if (obj_valid(s_hint_label)) {
        lv_label_set_text(s_hint_label, tr("touch_to_wake"));
    }
}

// ─────────────────────────────────────────────────────────────
// AP-web HOLD gate implementation
// ─────────────────────────────────────────────────────────────
// Called from web_export.cpp:
//  - hold = true  → saver fully disabled; hide if active.
//  - hold = false → saver allowed again.
void screensaver_set_apweb_hold(bool hold)
{
    if (hold == s_apweb_hold) return; // no change

    s_apweb_hold = hold;

    if (hold) {
        // Immediately hide if active so AP web can safely use SD/CH422G.
        if (s_active) {
            ss_hide_core_internal();
        }
        Serial.println("[Screensaver] AP-web HOLD ON");
    } else {
        Serial.println("[Screensaver] AP-web HOLD OFF");
    }
}

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────
static void start_fullframe_refresh_timer() {
#if SCREENSAVER_FORCE_FULL_REFRESH
    if (!s_refr_timer) {
        s_refr_timer = lv_timer_create(
            [](lv_timer_t* /*t*/) {
                if (obj_valid(s_overlay)) lv_obj_invalidate(s_overlay);
            },
            SCREENSAVER_REFRESH_MS,
            nullptr
        );
        lv_timer_set_repeat_count(s_refr_timer, -1);
    }
#endif
}

static void stop_fullframe_refresh_timer() {
#if SCREENSAVER_FORCE_FULL_REFRESH
    if (s_refr_timer) {
        lv_timer_del(s_refr_timer);
        s_refr_timer = nullptr;
    }
#endif
}

static void start_drift_animations(int driftX, int driftY) {
    // X axis
    lv_anim_init(&s_animX);
    lv_anim_set_var(&s_animX, s_img);
    lv_anim_set_exec_cb(&s_animX, anim_set_x);
    lv_anim_set_values(&s_animX, -driftX, driftX);
    lv_anim_set_time(&s_animX, 6000);
    lv_anim_set_playback_time(&s_animX, 6000);
    lv_anim_set_repeat_count(&s_animX, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&s_animX, lv_anim_path_ease_in_out);
    lv_anim_start(&s_animX);

    // Y axis
    lv_anim_init(&s_animY);
    lv_anim_set_var(&s_animY, s_img);
    lv_anim_set_exec_cb(&s_animY, anim_set_y);
    lv_anim_set_values(&s_animY, -driftY, driftY);
    lv_anim_set_time(&s_animY, 8000);
    lv_anim_set_playback_time(&s_animY, 8000);
    lv_anim_set_repeat_count(&s_animY, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&s_animY, lv_anim_path_ease_in_out);
    lv_anim_start(&s_animY);

    // Micro motion
    lv_anim_init(&s_animMicro);
    lv_anim_set_var(&s_animMicro, s_img);
    lv_anim_set_exec_cb(&s_animMicro, anim_set_translate_y);
    lv_anim_set_values(&s_animMicro, -2, 2);
    lv_anim_set_time(&s_animMicro, 1500);
    lv_anim_set_playback_time(&s_animMicro, 1500);
    lv_anim_set_repeat_count(&s_animMicro, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&s_animMicro, lv_anim_path_ease_in_out);
    lv_anim_start(&s_animMicro);
}

static void stop_drift_animations() {
    if (obj_valid(s_img)) {
        lv_anim_del(s_img, nullptr);
        lv_obj_set_style_translate_y(s_img, 0, 0);
    }
}

// ─────────────────────────────────────────────────────────────
// Show the screensaver overlay
// ─────────────────────────────────────────────────────────────
// NOTE: This function performs backlight control which may interact with the
// CH422G expander on I2C. When called from LVGL callbacks or screen transitions,
// avoid holding hal::i2c_lock() for extended periods to prevent screen jumps
// or delays when I2C is busy with SD/RTC operations.
void screensaver_show(void)
{
    // If AP web holds the saver, do absolutely nothing:
    // no overlay, no backlight changes, no CH422G traffic.
    if (s_apweb_hold) {
        // Optional: Serial.println("[Screensaver] show() ignored: AP-web HOLD");
        return;
    }

    if (s_active) return;

    // 1) Ensure overlay exists (parent = lv_layer_top)
    if (!obj_valid(s_overlay)) {
        s_overlay = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_overlay);
        lv_obj_set_size(s_overlay, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
        lv_obj_set_scroll_dir(s_overlay, LV_DIR_NONE);
        lv_obj_set_scrollbar_mode(s_overlay, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

        // Image child
        s_img = lv_img_create(s_overlay);
        lv_img_set_src(s_img, &welcome_screen);
        const int Z = 320; // 320/256 ≈ 1.25x zoom
        lv_img_set_pivot(s_img, welcome_screen.header.w / 2, welcome_screen.header.h / 2);
        lv_img_set_zoom (s_img, Z);
        lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);

        // "Touch to wake" label
        s_hint_label = lv_label_create(s_overlay);
        lv_label_set_text(s_hint_label, tr("touch_to_wake"));
        lv_obj_set_style_text_color(s_hint_label, lv_color_white(), 0);
        lv_obj_set_style_text_opa  (s_hint_label, LV_OPA_60, 0);
        lv_obj_set_style_text_font (s_hint_label, &ui_font_16, 0);
        lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -12);

        // Compute safe drift area
        const int imgW    = (welcome_screen.header.w * Z) / 256;
        const int imgH    = (welcome_screen.header.h * Z) / 256;
        const int marginX = LV_MAX(0, (imgW - LV_HOR_RES) / 2);
        const int marginY = LV_MAX(0, (imgH - LV_VER_RES) / 2);
        const int driftX  = LV_MIN(56, LV_MAX(10, marginX - 2));
        const int driftY  = LV_MIN(56, LV_MAX(10, marginY - 2));

        start_drift_animations(driftX, driftY);
    } else {
        // Reuse overlay
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        if (!obj_valid(s_img)) {
            s_img = lv_img_create(s_overlay);
            lv_img_set_src(s_img, &welcome_screen);
            lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);
        } else {
            lv_obj_set_style_translate_y(s_img, 0, 0);
            lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);
        }

        if (!obj_valid(s_hint_label)) {
            s_hint_label = lv_label_create(s_overlay);
            lv_label_set_text(s_hint_label, tr("touch_to_wake"));
            lv_obj_set_style_text_color(s_hint_label, lv_color_white(), 0);
            lv_obj_set_style_text_opa  (s_hint_label, LV_OPA_60, 0);
            lv_obj_set_style_text_font (s_hint_label, &ui_font_16, 0);
            lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -12);
        }

        stop_drift_animations();
        start_drift_animations(24, 24);
    }

#if defined(HAS_HAL_BACKLIGHT_SET)
    hal::backlight_set(20); // dim if AP3032 CTRL is wired
#else
    hal::backlight_on();
#endif

    // Fade-in
    lv_obj_set_style_opa(s_overlay, LV_OPA_TRANSP, 0);
    {
        lv_anim_t a; 
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_overlay);
        lv_anim_set_exec_cb(&a, set_obj_opa);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&a, 300);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }

    start_fullframe_refresh_timer();

    s_active = true;
    Serial.println("[Screensaver] Overlay activated (top layer, easing, forced refresh "
#if SCREENSAVER_FORCE_FULL_REFRESH
                   "ON"
#else
                   "OFF"
#endif
                   ")");
}

// ─────────────────────────────────────────────────────────────
// Core hide (no screen switch): fade out overlay, stop anims, hide overlay
// ─────────────────────────────────────────────────────────────
// NOTE: This function performs backlight control which may interact with the
// CH422G expander on I2C. Called asynchronously via lv_async_call to ensure
// safe LVGL context. Avoid performing blocking I2C operations inside LVGL
// callbacks to prevent screen transition delays.
static void ss_hide_core_internal(void)
{
    if (!s_active) return;

    stop_drift_animations();
    stop_fullframe_refresh_timer();

    // brightness back
    hal::backlight_on();

    if (obj_valid(s_overlay)) {
        lv_anim_t a; 
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_overlay);
        lv_anim_set_exec_cb(&a, set_obj_opa);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&a, 260);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_ready_cb(&a, [](lv_anim_t* /*anim*/) {
            if (obj_valid(s_overlay)) {
                if (obj_valid(s_img)) {
                    lv_obj_set_style_translate_y(s_img, 0, 0);
                }
                lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_opa(s_overlay, LV_OPA_COVER, 0); // reset for next time
            }
        });
        lv_anim_start(&a);
    }

    s_active = false;
    Serial.println("[Screensaver] Overlay hide scheduled (no screen switch)");
}

// Public wrappers
void screensaver_hide(void) { ss_hide_core_internal(); }

static void ss_async_hide_cb(void* /*ud*/) { ss_hide_core_internal(); }

void screensaver_hide_async(void) { lv_async_call(ss_async_hide_cb, NULL); }
