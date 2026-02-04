
/* 
 * LVGL v8 port for ESP_Panel + Arduino
 *
 * Clean file: no GUI code, no font macros, no HTML entities, UTF-8 without BOM.
 */
#include "esp_timer.h"
#include "lvgl_v8_port.h"
using namespace esp_panel::drivers;

// -----------------------------------------------------------------------------
// LVGL Port State
// -----------------------------------------------------------------------------
static SemaphoreHandle_t lvgl_mux = nullptr; // LVGL recursive mutex
static TaskHandle_t lvgl_task_handle = nullptr; // LVGL task

#if !LV_TICK_CUSTOM
static esp_timer_handle_t lvgl_tick_timer = nullptr; // LVGL tick timer
#endif

#if LVGL_PORT_AVOID_TEAR
// In avoid-tear modes (RGB/MIPI-DSI), buffers come from the panel
#else
// In non-avoid-tear mode, we allocate 2 small line buffers
static void *lvgl_buf[LVGL_PORT_BUFFER_NUM] = { nullptr, nullptr };
#endif

// -----------------------------------------------------------------------------
// LVGL Display Flush (draw)
// -----------------------------------------------------------------------------
#if LVGL_PORT_AVOID_TEAR
// For RGB/MIPI-DSI, LVGL can operate in direct/full refresh and panel
// drives the frame buffers; LVGL ends flush via driver update callback.
static void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  // For RGB/MIPI direct mode, LVGL will call lv_disp_flush_ready() via
  // the driver's draw-finish callback (configured below). Nothing to do here.
  (void)drv; (void)area; (void)color_map;
}
#else
// Generic SPI/Parallel panels: push a rectangle of pixels
static void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  LCD *lcd = (LCD *)drv->user_data;
  const int x = area->x1;
  const int y = area->y1;
  const int w = area->x2 - area->x1 + 1;
  const int h = area->y2 - area->y1 + 1;

  // Write bitmap (lv_color_t is 16-bit RGB565)
  lcd->drawBitmap(x, y, w, h, (const uint8_t *)color_map);

  // Notify LVGL the flush is done
  lv_disp_flush_ready(drv);
}
#endif

// -----------------------------------------------------------------------------
// Align flush area to bus requirements
// -----------------------------------------------------------------------------
static void rounder_callback(lv_disp_drv_t *drv, lv_area_t *area)
{
  LCD *lcd = (LCD *)drv->user_data;
  uint8_t x_align = lcd->getBasicAttributes().basic_bus_spec.x_coord_align;
  uint8_t y_align = lcd->getBasicAttributes().basic_bus_spec.y_coord_align;

  if (x_align > 1) {
    area->x1 &= ~(x_align - 1);
    area->x2 = (area->x2 & ~(x_align - 1)) + x_align - 1;
  }
  if (y_align > 1) {
    area->y1 &= ~(y_align - 1);
    area->y2 = (area->y2 & ~(y_align - 1)) + y_align - 1;
  }
}

// -----------------------------------------------------------------------------
// Rotation update
// -----------------------------------------------------------------------------
static void update_callback(lv_disp_drv_t *drv)
{
  LCD *lcd = (LCD *)drv->user_data;
  auto t = lcd->getTransformation();

  const bool mx = t.mirror_x;
  const bool my = t.mirror_y;
  const bool sw = t.swap_xy;

  switch (drv->rotated) {
    case LV_DISP_ROT_NONE:
      lcd->swapXY(sw);
      lcd->mirrorX(mx);
      lcd->mirrorY(my);
      break;

    case LV_DISP_ROT_90:
      lcd->swapXY(!sw);
      lcd->mirrorX(mx);
      lcd->mirrorY(!my);
      break;

    case LV_DISP_ROT_180:
      lcd->swapXY(sw);
      lcd->mirrorX(!mx);
      lcd->mirrorY(!my);
      break;

    case LV_DISP_ROT_270:
      lcd->swapXY(!sw);
      lcd->mirrorX(!mx);
      lcd->mirrorY(my);
      break;
  }
}

// -----------------------------------------------------------------------------
// Touch read
// -----------------------------------------------------------------------------

extern void gui_note_user_activity();
extern bool gScreenSaverActive; // defined in your .ino

static bool s_swallow_until_release = false;

static void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
  Touch *tp = (Touch *)indev_drv->user_data;
  TouchPoint p;

  // Use thread-safe touch read via I2C executor (runs on core 0)
  int n = hal::touch_read_points_safe(&p, 1, 50);

  if (n > 0) {
    const bool was_ss = gScreenSaverActive;
    gui_note_user_activity();
    if (was_ss) s_swallow_until_release = true;

    if (s_swallow_until_release) {
      data->state = LV_INDEV_STATE_RELEASED;
      return;
    }

    data->point.x = p.x;
    data->point.y = p.y;
    data->state = LV_INDEV_STATE_PRESSED;
  }
  else {
    data->state = LV_INDEV_STATE_RELEASED;
    if (s_swallow_until_release) s_swallow_until_release = false;
  }
}

// -----------------------------------------------------------------------------
// Display driver registration
// -----------------------------------------------------------------------------
static lv_disp_t *display_init(LCD *lcd)
{
  if (!lcd || !lcd->getRefreshPanelHandle()) return nullptr;

  static lv_disp_draw_buf_t disp_buf;
  static lv_disp_drv_t disp_drv;

  const int lcd_w = lcd->getFrameWidth();
  const int lcd_h = lcd->getFrameHeight();

#if LVGL_PORT_AVOID_TEAR
  // Direct/full refresh: LVGL uses panel FBs
  void *buf0 = lcd->getFrameBufferByIndex(0);
  void *buf1 = lcd->getFrameBufferByIndex(1);
  const int buffer_size = lcd_w * lcd_h;

  lv_disp_draw_buf_init(&disp_buf, buf0, buf1, buffer_size);

#else
  // Line buffers for partial flush path
  const int buffer_size = lcd_w * LVGL_PORT_BUFFER_SIZE_HEIGHT;
  for (int i = 0; i < LVGL_PORT_BUFFER_NUM; ++i) {
    lvgl_buf[i] = heap_caps_malloc(buffer_size * sizeof(lv_color_t),
                                   LVGL_PORT_BUFFER_MALLOC_CAPS);
    if (!lvgl_buf[i]) return nullptr;
  }

  lv_disp_draw_buf_init(&disp_buf, lvgl_buf[0], lvgl_buf[1], buffer_size);
#endif

  lv_disp_drv_init(&disp_drv);
  disp_drv.flush_cb = flush_callback;

// rotation resolution
#if (LVGL_PORT_ROTATION_DEGREE == 90) || (LVGL_PORT_ROTATION_DEGREE == 270)
  disp_drv.hor_res = lcd_h;
  disp_drv.ver_res = lcd_w;
#else
  disp_drv.hor_res = lcd_w;
  disp_drv.ver_res = lcd_h;
#endif

#if LVGL_PORT_AVOID_TEAR
  #if LVGL_PORT_FULL_REFRESH
    disp_drv.full_refresh = 1;
  #elif LVGL_PORT_DIRECT_MODE
    disp_drv.direct_mode = 1;
  #endif
#else
  const auto &spec = lcd->getBasicAttributes().basic_bus_spec;
  if (spec.isFunctionValid(LCD::BasicBusSpecification::FUNC_SWAP_XY) &&
      spec.isFunctionValid(LCD::BasicBusSpecification::FUNC_MIRROR_X) &&
      spec.isFunctionValid(LCD::BasicBusSpecification::FUNC_MIRROR_Y)) {
    disp_drv.drv_update_cb = update_callback;
  } else {
    disp_drv.sw_rotate = 1;
  }
#endif

  disp_drv.draw_buf = &disp_buf;
  disp_drv.user_data = (void *)lcd;

  const auto &spec2 = lcd->getBasicAttributes().basic_bus_spec;
  if (spec2.x_coord_align > 1 || spec2.y_coord_align > 1) {
    disp_drv.rounder_cb = rounder_callback;
  }

  return lv_disp_drv_register(&disp_drv);
}

static lv_indev_t *indev_init(Touch *tp)
{
  if (!tp || !tp->getPanelHandle()) return nullptr;

  static lv_indev_drv_t indev_drv_tp;
  lv_indev_drv_init(&indev_drv_tp);

  indev_drv_tp.type = LV_INDEV_TYPE_POINTER;
  indev_drv_tp.read_cb = touchpad_read;
  indev_drv_tp.user_data = (void *)tp;

  return lv_indev_drv_register(&indev_drv_tp);
}

// -----------------------------------------------------------------------------
// LVGL Tick Timer
// -----------------------------------------------------------------------------
#if !LV_TICK_CUSTOM
static void tick_increment(void *arg)
{
  (void)arg;
  lv_tick_inc(LVGL_PORT_TICK_PERIOD_MS);
}

static bool tick_init(void)
{
  const esp_timer_create_args_t args = {
    .callback = &tick_increment,
    .arg = nullptr,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "lvgl_tick"
  };

  if (esp_timer_create(&args, &lvgl_tick_timer) != ESP_OK) return false;
  if (esp_timer_start_periodic(lvgl_tick_timer, LVGL_PORT_TICK_PERIOD_MS * 1000) != ESP_OK)
    return false;

  return true;
}

static bool tick_deinit(void)
{
  if (!lvgl_tick_timer) return true;
  esp_timer_stop(lvgl_tick_timer);
  esp_timer_delete(lvgl_tick_timer);
  lvgl_tick_timer = nullptr;
  return true;
}
#endif

// -----------------------------------------------------------------------------
// LVGL task
// -----------------------------------------------------------------------------
static void lvgl_port_task(void *arg)
{
  (void)arg;
  uint32_t delay_ms = LVGL_PORT_TASK_MAX_DELAY_MS;

  for (;;) {
    if (lvgl_port_lock(-1)) {
      delay_ms = lv_timer_handler();
      lvgl_port_unlock();
    }

    if (delay_ms > LVGL_PORT_TASK_MAX_DELAY_MS)
      delay_ms = LVGL_PORT_TASK_MAX_DELAY_MS;
    else if (delay_ms < LVGL_PORT_TASK_MIN_DELAY_MS)
      delay_ms = LVGL_PORT_TASK_MIN_DELAY_MS;

    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
bool lvgl_port_init(LCD *lcd, Touch *tp)
{
  if (!lcd) return false;

  lv_init();

#if !LV_TICK_CUSTOM
  if (!tick_init()) return false;
#endif

  lv_disp_t *disp = display_init(lcd);
  if (!disp) return false;

  lv_disp_set_rotation(disp, LV_DISP_ROT_NONE);

  auto bus_type = lcd->getBus()->getBasicAttributes().type;
  if (bus_type != ESP_PANEL_BUS_TYPE_RGB) {
    lcd->attachDrawBitmapFinishCallback(
      [](void *user_data) -> bool {
        lv_disp_drv_t *drv = (lv_disp_drv_t *)user_data;
        lv_disp_flush_ready(drv);
        return false;
      },
      (void *)disp->driver
    );
  }

  if (tp) {
    lv_indev_t *indev = indev_init(tp);
    if (!indev) return false;
  }

  lvgl_mux = xSemaphoreCreateRecursiveMutex();
  if (!lvgl_mux) return false;

  BaseType_t core = (LVGL_PORT_TASK_CORE < 0)
                      ? tskNO_AFFINITY
                      : LVGL_PORT_TASK_CORE;

  BaseType_t ok = xTaskCreatePinnedToCore(
      lvgl_port_task,
      "lvgl",
      LVGL_PORT_TASK_STACK_SIZE,
      nullptr,
      LVGL_PORT_TASK_PRIORITY,
      &lvgl_task_handle,
      core
  );

  if (ok != pdPASS) return false;

  return true;
}

bool lvgl_port_deinit(void)
{
#if !LV_TICK_CUSTOM
  tick_deinit();
#endif

  if (lvgl_task_handle) {
    vTaskDelete(lvgl_task_handle);
    lvgl_task_handle = nullptr;
  }

#if LV_ENABLE_GC && !LV_MEM_CUSTOM
  lv_deinit();
#endif

#if !LVGL_PORT_AVOID_TEAR
  for (int i = 0; i < LVGL_PORT_BUFFER_NUM; ++i) {
    if (lvgl_buf[i]) {
      free(lvgl_buf[i]);
      lvgl_buf[i] = nullptr;
    }
  }
#endif

  if (lvgl_mux) {
    vSemaphoreDelete(lvgl_mux);
    lvgl_mux = nullptr;
  }

  return true;
}

bool lvgl_port_lock(int timeout_ms)
{
  if (!lvgl_mux) return false;

  TickType_t ticks = (timeout_ms < 0)
                        ? portMAX_DELAY
                        : pdMS_TO_TICKS(timeout_ms);

  return (xSemaphoreTakeRecursive(lvgl_mux, ticks) == pdTRUE);
}

bool lvgl_port_unlock(void)
{
  if (!lvgl_mux) return false;
  xSemaphoreGiveRecursive(lvgl_mux);
  return true;
}
