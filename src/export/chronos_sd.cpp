#include "export_fs.h"           // chronos::exportfs_set_fs(&SD)
#include "src/drivers/hal_panel.h"
#include "chronos_sd.h"
#include "../drivers/hal_i2c_executor.h"
#include "../drivers/hal_i2c_manager.h" // fallback

static bool s_sd_ready = false;
static bool s_cs_low   = false;
static int  s_cs_depth = 0;
static bool s_cs_dir_set = false;

bool chronos_sd_preinit() {
  // No-op: HAL owns I2C/expander. Kept for sketch compatibility.
  return true;
}

bool chronos_sd_is_ready() {
  return s_sd_ready;
}

/* ---------- CS helpers (CH422G via HAL) ---------------------------------- */

// Executor request contexts for expander writes
struct expander_setdir_ctx {
  esp_io_expander_handle_t h;
  uint32_t mask;
  io_expander_dir_t dir;
  bool result;
};
static esp_err_t expander_setdir_op(void* vctx) {
  expander_setdir_ctx* ctx = static_cast<expander_setdir_ctx*>(vctx);
  if (!ctx || !ctx->h) return ESP_ERR_INVALID_ARG;
  esp_err_t r = esp_io_expander_set_dir(ctx->h, ctx->mask, ctx->dir);
  ctx->result = (r == ESP_OK);
  return r;
}

struct expander_setlevel_ctx {
  esp_io_expander_handle_t h;
  uint32_t mask;
  int level;
  bool result;
};
static esp_err_t expander_setlevel_op(void* vctx) {
  expander_setlevel_ctx* ctx = static_cast<expander_setlevel_ctx*>(vctx);
  if (!ctx || !ctx->h) return ESP_ERR_INVALID_ARG;
  esp_err_t r = esp_io_expander_set_level(ctx->h, ctx->mask, ctx->level);
  ctx->result = (r == ESP_OK);
  return r;
}

static inline void cs_drive(bool low) {
  if (!hal::expander_wait_ready(800)) return;

  esp_io_expander_handle_t h = hal::expander_get_handle();
  if (!h) return;

  // Configure SD_CS direction once (reduces I2C traffic and failure rate)
  if (!s_cs_dir_set) {
    for (int i = 0; i < 3; ++i) {
      expander_setdir_ctx ctx{ h, (1u << SD_CS), IO_EXPANDER_OUTPUT, false };
      esp_err_t r = hal_i2c_exec_sync(expander_setdir_op, &ctx, 120);
      if (r == ESP_OK && ctx.result) { s_cs_dir_set = true; break; }
      // fallback quick retry using mutex if executor busy
      if (hal::i2c_lock(40)) {
        bool ok = hal::expander_pinMode(SD_CS, /*output*/ true);
        hal::i2c_unlock();
        if (ok) { s_cs_dir_set = true; break; }
      }
      delay(2);
    }
  }

  // LOW selects, HIGH de-selects (retry a few times in case I2C is busy)
  const int level = low ? 0 : 1;
  for (int i = 0; i < 3; ++i) {
    expander_setlevel_ctx ctx{ h, (1u << SD_CS), level, false };
    esp_err_t r = hal_i2c_exec_sync(expander_setlevel_op, &ctx, 120);
    if (r == ESP_OK && ctx.result) break;

    // fallback to mutex-free attempt
    if (hal::i2c_lock(40)) {
      bool ok = hal::expander_digitalWrite(SD_CS, level ? true : false);
      hal::i2c_unlock();
      if (ok) break;
    }
    delay(2);
  }

  delayMicroseconds(2);
}