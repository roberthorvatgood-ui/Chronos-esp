/*
 * PCF85063A wrapper using hal_i2c_executor for physical I2C transactions.
 */

#include <Arduino.h>
#include "waveshare_pcf85063a.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "../drivers/hal_i2c_manager.h"
#include "../drivers/hal_i2c_executor.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "PCF85063A";

#ifndef I2C_RTC_PORT
#define I2C_RTC_PORT I2C_NUM_0
#endif

#ifndef PCF85063A_ADDRESS
#define PCF85063A_ADDRESS 0x51
#endif

// Driver install unchanged (not an I2C transaction)
static esp_err_t ensure_i2c_driver_installed()
{
    esp_err_t e = i2c_driver_install(I2C_RTC_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (e == ESP_OK || e == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "i2c_driver_install(I2C%d) err=0x%x", (int)I2C_RTC_PORT, e);
    return e;
}

// Executor request context for PCF write
struct pcf_write_ctx {
  uint8_t buf[16];
  size_t  len;
  esp_err_t result;
};
static esp_err_t pcf_write_op(void* vctx) {
  pcf_write_ctx* ctx = static_cast<pcf_write_ctx*>(vctx);
  if (!ctx || ctx->len == 0) return ESP_ERR_INVALID_ARG;
  // perform IDF write
  esp_err_t r = i2c_master_write_to_device(I2C_RTC_PORT, PCF85063A_ADDRESS, ctx->buf, ctx->len, pdMS_TO_TICKS(200));
  ctx->result = r;
  return r;
}

esp_err_t pcf_write_bytes(uint8_t reg, const uint8_t *data, size_t len, TickType_t to_ticks)
{
    if (ensure_i2c_driver_installed() != ESP_OK) {
        return ESP_FAIL;
    }

    if (len + 1 > sizeof(((pcf_write_ctx*)0)->buf)) return ESP_ERR_INVALID_ARG;

    pcf_write_ctx ctx;
    ctx.len = len + 1;
    ctx.buf[0] = reg;
    if (len) memcpy(&ctx.buf[1], data, len);
    ctx.result = ESP_FAIL;

    // run on executor
    esp_err_t r = hal_i2c_exec_sync(pcf_write_op, &ctx, 300);
    return (r == ESP_OK) ? ctx.result : r;
}

// Executor request context for PCF read (write reg then read)
struct pcf_read_ctx {
  uint8_t reg;
  uint8_t* out;
  size_t len;
  esp_err_t result;
};
static esp_err_t pcf_read_op(void* vctx) {
  pcf_read_ctx* ctx = static_cast<pcf_read_ctx*>(vctx);
  if (!ctx || !ctx->out) return ESP_ERR_INVALID_ARG;
  esp_err_t r = i2c_master_write_read_device(I2C_RTC_PORT, PCF85063A_ADDRESS, &ctx->reg, 1, ctx->out, ctx->len, pdMS_TO_TICKS(200));
  ctx->result = r;
  return r;
}

esp_err_t pcf_read_bytes(uint8_t reg, uint8_t *data, size_t len, TickType_t to_ticks)
{
    if (ensure_i2c_driver_installed() != ESP_OK) {
        return ESP_FAIL;
    }

    pcf_read_ctx ctx;
    ctx.reg = reg;
    ctx.out = data;
    ctx.len = len;
    ctx.result = ESP_FAIL;

    esp_err_t r = hal_i2c_exec_sync(pcf_read_op, &ctx, 300);
    return (r == ESP_OK) ? ctx.result : r;
}

esp_err_t pcf_write_reg(uint8_t reg, uint8_t val)
{
    return pcf_write_bytes(reg, &val, 1, pdMS_TO_TICKS(50));
}

esp_err_t pcf_read_reg(uint8_t reg, uint8_t *out)
{
    return pcf_read_bytes(reg, out, 1, pdMS_TO_TICKS(50));
}