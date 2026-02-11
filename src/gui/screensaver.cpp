/*****
 * screensaver.cpp — Chronos screensaver with zoomed + drifting welcome image
 * [Updated: 2026-02-08] Added gate input pause/resume coordination
 *****/

#include <Arduino.h>
#include "screensaver.h"
#include "gui.h"
#include "../drivers/hal_panel.h"
#include "../intl/i18n.h"
#include <lvgl.h>
#include "../assets/welcome_screen.h"
#include "../io/input.h"  // input_pause, input_resume

// ─────────────────────────────────────────────────────────────
// Config macros (can override in platformio.ini or here)
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
        // Pause gate input polling to free I²C bus for AP-web operations
        input_pause();
        
        // Immediately hide if active so AP web can safely use SD/CH422G.
        if (s_active) {
            ss_hide_core_internal();
        }
        Serial.println("[Screensaver] AP-web HOLD ON - gate polling PAUSED");
    } else {
        // Resume gate input polling
        input_resume();
        Serial.println("[Screensaver] AP-web HOLD OFF - gate polling RESUMED");
    }
}

// ─────────────────────────────────────────────────────────────
// Helpers
// ──────���──────────────────────────────────────────────────────
static void start_fullframe_refresh_timer() {
#if SCREENSAVER_FORCE_FULL_REFRESH
    if (s_refr_timer) return;
    auto timer_cb = [](lv_timer_t* t) {
        lv_obj_invalidate(lv_scr_act());
    };
    s_refr_timer = lv_timer_create(timer_cb, SCREENSAVER_REFRESH_MS, nullptr);
    if (s_refr_timer) lv_timer_set_repeat_count(s_refr_timer, -1);
#endif
}

static void stop_fullframe_refresh_timer() {
#if SCREENSAVER_FORCE_FULL_REFRESH
    if (!s_refr_timer) return;
    lv_timer_del(s_refr_timer);
    s_refr_timer = nullptr;
#endif
}

// Animation delete helpers (guards)
static void del_anim(lv_anim_t* a) {
    if (a && a->var) {
        lv_anim_del(a->var, nullptr);
        memset(a, 0, sizeof(*a));
    }
}

// ─────────────────────────────────────────────────────────────
// Core hide implementation (no resume – used by hold gate)
// ─────────────────────────────────────────────────────────────
static void ss_hide_core_internal(void)
{
    if (!s_active) return; // not shown

    // Stop animations
    del_anim(&s_animX);
    del_anim(&s_animY);
    del_anim(&s_animMicro);

    stop_fullframe_refresh_timer();

    // Delete overlay + children
    if (obj_valid(s_overlay)) {
        lv_obj_del(s_overlay);
        s_overlay    = nullptr;
        s_img        = nullptr;
        s_hint_label = nullptr;
    }

    s_active = false;
    
    // DON'T auto-resume gate polling here
    // Caller controls when to resume (after backlight settles)
    if (!s_apweb_hold) {
        Serial.println("[Screensaver] Deactivated (gate polling still paused - caller will resume)");
    }
}

// ─────────────────────────────────────────────────────────────
// Public show
// ─────────────────────────────────────────────────────────────
void screensaver_show()
{
    // AP-web hold gate: if hold is active, saver is disabled.
    if (s_apweb_hold) return;

    if (s_active) return; // already shown

    // Pause gate input polling to reduce I²C bus contention
    input_pause();
    Serial.println("[Screensaver] Activating - gate polling PAUSED");

    // 1) Create overlay on lv_layer_top (always above normal screens)
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);

    // 2) Fade-in overlay from transparent
    lv_obj_set_style_opa(s_overlay, LV_OPA_TRANSP, 0);
    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, s_overlay);
    lv_anim_set_exec_cb(&fade_in, set_obj_opa);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&fade_in, 300);
    lv_anim_set_path_cb(&fade_in, lv_anim_path_ease_in);
    lv_anim_start(&fade_in);

    // 3) Image (zoomed, centered initially)
    // No need for LV_IMG_DECLARE, it's in welcome_screen.h
    s_img = lv_img_create(s_overlay);
    lv_img_set_src(s_img, &welcome_screen);  // CHANGED: chronos_welcome -> welcome_screen

    lv_obj_center(s_img);

    // Zoom to ~130% (LVGL zoom is scaled by 256: 256=100%)
    lv_img_set_zoom(s_img, (uint16_t)(1.3 * 256));

    // 4) Drift animation on X axis
    lv_anim_init(&s_animX);
    lv_anim_set_var(&s_animX, s_img);
    lv_anim_set_exec_cb(&s_animX, anim_set_x);
    lv_anim_set_values(&s_animX, -50, 50);
    lv_anim_set_time(&s_animX, 8000);
    lv_anim_set_playback_time(&s_animX, 8000);
    lv_anim_set_repeat_count(&s_animX, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&s_animX, lv_anim_path_ease_in_out);
    lv_anim_start(&s_animX);

    // 5) Drift animation on Y axis
    lv_anim_init(&s_animY);
    lv_anim_set_var(&s_animY, s_img);
    lv_anim_set_exec_cb(&s_animY, anim_set_y);
    lv_anim_set_values(&s_animY, -30, 30);
    lv_anim_set_time(&s_animY, 10000);
    lv_anim_set_playback_time(&s_animY, 10000);
    lv_anim_set_repeat_count(&s_animY, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&s_animY, lv_anim_path_ease_in_out);
    lv_anim_start(&s_animY);

    // 6) Micro-motion: additive vertical jitter using style translate_y
    lv_anim_init(&s_animMicro);
    lv_anim_set_var(&s_animMicro, s_img);
    lv_anim_set_exec_cb(&s_animMicro, anim_set_translate_y);
    lv_anim_set_values(&s_animMicro, -8, 8);
    lv_anim_set_time(&s_animMicro, 3000);
    lv_anim_set_playback_time(&s_animMicro, 3000);
    lv_anim_set_repeat_count(&s_animMicro, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&s_animMicro, lv_anim_path_linear);
    lv_anim_start(&s_animMicro);

    // 7) Hint label (localized)
    s_hint_label = lv_label_create(s_overlay);
    lv_label_set_text(s_hint_label, tr("touch_to_wake"));
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x808080), 0);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -20);

    // 8) Optional full-frame refresh timer (for smoother animation)
    start_fullframe_refresh_timer();

    s_active = true;
    Serial.println("[Screensaver] Overlay activated (top layer, easing, forced refresh OFF)");
}

// ─────────────────────────────────────────────────────────────
// Public hide
// ─────────────────────────────────────────────────────────────
void screensaver_hide()
{
    ss_hide_core_internal();
}

// ─────────────────────────────────────────────────────────────
// Async hide (for touch ISR context; does NOT resume gate polling)
// ─────────────────────────────────────────────────────────────
void screensaver_hide_async()
{
    if (!s_active) return;
    
    // Flag for main loop to call screensaver_hide()
    // This is safer than calling LVGL operations from ISR
    extern bool g_screensaver_hide_requested;
    g_screensaver_hide_requested = true;
}

bool screensaver_is_active()
{
    return s_active;
}