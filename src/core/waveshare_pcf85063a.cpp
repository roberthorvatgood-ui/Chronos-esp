/******************************************************************************
 * PCF85063A – OLD IDF I2C driver implementation (no Wire, no driver_ng)
 * Tolerant init: reuse I2C0 if already installed; do not abort on install error
 *
 * Updated: use central I2C executor for physical I2C transactions so
 *          reads/writes are serialized on core 0 and will not block ISRs / LVGL.
 ******************************************************************************/

#include "waveshare_pcf85063a.h"
#include <stdio.h>
#include "esp_err.h"
#include "../drivers/hal_i2c_executor.h"
#include "driver/i2c.h"
#include <string.h>

static inline uint8_t dec2bcd(uint8_t v){ return (uint8_t)(((v/10)<<4)|(v%10)); }
static inline uint8_t bcd2dec(uint8_t v){ return (uint8_t)(((v>>4)*10) + (v & 0x0F)); }

/* ----- OLD driver helpers on I2C0 (now executor-backed) ----- */

// Context for write operation
struct i2c_wr_ctx {
  uint8_t buf[1 + 8];
  size_t len;
  esp_err_t result;
};

static esp_err_t i2c_wr_op(void* vctx) {
  if (!vctx) return ESP_ERR_INVALID_ARG;
  i2c_wr_ctx* ctx = static_cast<i2c_wr_ctx*>(vctx);
  if (!ctx || ctx->len == 0) return ESP_ERR_INVALID_ARG;
  esp_err_t r = i2c_master_write_to_device(I2C_RTC_PORT, PCF85063A_ADDRESS, ctx->buf, ctx->len, pdMS_TO_TICKS(200));
  ctx->result = r;
  return r;
}

static inline esp_err_t i2c_wr_reg(uint8_t reg, const uint8_t* data, size_t len, TickType_t to_ticks = pdMS_TO_TICKS(50)) {
  if (len > 8) return ESP_ERR_INVALID_ARG;
  i2c_wr_ctx ctx;
  ctx.len = len + 1;
  ctx.buf[0] = reg;
  if (len) memcpy(&ctx.buf[1], data, len);
  ctx.result = ESP_FAIL;
  // Execute on executor (sync)
  esp_err_t r = hal_i2c_exec_sync(i2c_wr_op, &ctx, (uint32_t)to_ticks == 0 ? 300 : pdTICKS_TO_MS(to_ticks));
  // hal_i2c_exec_sync returns esp_err_t from the op wrapper; use ctx.result for underlying call result
  return (r == ESP_OK) ? ctx.result : r;
}

// Context for read operation (write reg then read)
struct i2c_rd_ctx {
  uint8_t reg;
  uint8_t* out;
  size_t len;
  esp_err_t result;
};

static esp_err_t i2c_rd_op(void* vctx) {
  if (!vctx) return ESP_ERR_INVALID_ARG;
  i2c_rd_ctx* ctx = static_cast<i2c_rd_ctx*>(vctx);
  if (!ctx || !ctx->out) return ESP_ERR_INVALID_ARG;
  esp_err_t r = i2c_master_write_read_device(I2C_RTC_PORT, PCF85063A_ADDRESS, &ctx->reg, 1, ctx->out, ctx->len, pdMS_TO_TICKS(200));
  ctx->result = r;
  return r;
}

static inline esp_err_t i2c_rd_reg(uint8_t reg, uint8_t* data, size_t len, TickType_t to_ticks = pdMS_TO_TICKS(50)) {
  i2c_rd_ctx ctx;
  ctx.reg = reg;
  ctx.out = data;
  ctx.len = len;
  ctx.result = ESP_FAIL;
  esp_err_t r = hal_i2c_exec_sync(i2c_rd_op, &ctx, (uint32_t)to_ticks == 0 ? 300 : pdTICKS_TO_MS(to_ticks));
  return (r == ESP_OK) ? ctx.result : r;
}

/* ----- Public LL helpers (preserve original API) ----- */
bool PCF85063A_ReadRegs(uint8_t reg, uint8_t *data, size_t len) {
  return i2c_rd_reg(reg, data, len) == ESP_OK;
}
bool PCF85063A_WriteRegs(uint8_t reg, const uint8_t *data, size_t len) {
  return i2c_wr_reg(reg, data, len) == ESP_OK;
}

/* ----- Init / Reset (tolerant) ----- */
bool PCF85063A_Init(void) {
  static bool s_inited = false;
  if (s_inited) return true;

  // Configure I2C0 as master (ignore invalid-state; it may be configured already)
  i2c_config_t cfg = {};
  cfg.mode = I2C_MODE_MASTER;
  cfg.sda_io_num = I2C_RTC_SDA_GPIO;
  cfg.scl_io_num = I2C_RTC_SCL_GPIO;
  cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
  cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
  cfg.master.clk_speed = I2C_RTC_FREQ_HZ;
  cfg.clk_flags = 0;

  esp_err_t e = i2c_param_config(I2C_RTC_PORT, &cfg);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
    printf("[PCF] i2c_param_config(I2C0) err=0x%x\n", e);
    // continue; another component may have already configured it
  }

  // Install driver; if it fails, do NOT abort — try to use the existing driver
  e = i2c_driver_install(I2C_RTC_PORT, I2C_MODE_MASTER, 0, 0, 0);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
    printf("[PCF] i2c_driver_install(I2C0) err=0x%x — will try to reuse existing driver\n", e);
  }

  // Sanity probe: read seconds register; if it works, we’re OK
  uint8_t probe = 0xFF;
  if (i2c_rd_reg(RTC_SECOND_ADDR, &probe, 1) != ESP_OK) {
    printf("[PCF] probe read failed on I2C0 (sec reg). Check pins 8/9 and port usage.\n");
    return false;
  }

  // CTRL1: 24h + 12.5pF, ensure running
  uint8_t v = (RTC_CTRL_1_DEFAULT | RTC_CTRL_1_CAP_SEL) & ~RTC_CTRL_1_STOP;
  PCF85063A_WriteRegs(RTC_CTRL_1_ADDR, &v, 1);

  s_inited = true;
  return true;
}

void PCF85063A_Reset(void) {
  uint8_t v = RTC_CTRL_1_DEFAULT | RTC_CTRL_1_CAP_SEL | RTC_CTRL_1_SR;
  PCF85063A_WriteRegs(RTC_CTRL_1_ADDR, &v, 1);
}

/* ----- Setters ----- */
void PCF85063A_Set_Time(datetime_t t) {
  uint8_t b[3] = {
    (uint8_t)(dec2bcd(t.sec)  & 0x7F),
    (uint8_t)(dec2bcd(t.min)  & 0x7F),
    (uint8_t)(dec2bcd(t.hour) & 0x3F)
  };
  PCF85063A_WriteRegs(RTC_SECOND_ADDR, b, sizeof b);
}

void PCF85063A_Set_Date(datetime_t d) {
  uint8_t b[4] = {
    (uint8_t)(dec2bcd(d.day)   & 0x3F),
    (uint8_t)(dec2bcd(d.dotw)  & 0x07),
    (uint8_t)(dec2bcd(d.month) & 0x1F),
    (uint8_t) dec2bcd((uint8_t)(d.year - YEAR_OFFSET))
  };
  PCF85063A_WriteRegs(RTC_DAY_ADDR, b, sizeof b);
}

void PCF85063A_Set_All(datetime_t t) {
  uint8_t b[7] = {
    (uint8_t)(dec2bcd(t.sec)  & 0x7F),
    (uint8_t)(dec2bcd(t.min)  & 0x7F),
    (uint8_t)(dec2bcd(t.hour) & 0x3F),
    (uint8_t)(dec2bcd(t.day)  & 0x3F),
    (uint8_t)(dec2bcd(t.dotw) & 0x07),
    (uint8_t)(dec2bcd(t.month)& 0x1F),
    (uint8_t) dec2bcd((uint8_t)(t.year - YEAR_OFFSET))
  };
  PCF85063A_WriteRegs(RTC_SECOND_ADDR, b, sizeof b);
}

/* ----- Getters ----- */
void PCF85063A_Read_now(datetime_t *t) {
  if (!t) return;
  uint8_t b[7] = {0};
  PCF85063A_ReadRegs(RTC_SECOND_ADDR, b, sizeof b);
  t->sec   = bcd2dec(b[0] & 0x7F);
  t->min   = bcd2dec(b[1] & 0x7F);
  t->hour  = bcd2dec(b[2] & 0x3F);
  t->day   = bcd2dec(b[3] & 0x3F);
  t->dotw  = bcd2dec(b[4] & 0x07);
  t->month = bcd2dec(b[5] & 0x1F);
  t->year  = bcd2dec(b[6]) + YEAR_OFFSET;
}

void PCF85063A_Enable_Alarm(void) {
  uint8_t v = (RTC_CTRL_2_DEFAULT | RTC_CTRL_2_AIE) & ~RTC_CTRL_2_AF;
  PCF85063A_WriteRegs(RTC_CTRL_2_ADDR, &v, 1);
}

uint8_t PCF85063A_Get_Alarm_Flag(void) {
  uint8_t v=0; PCF85063A_ReadRegs(RTC_CTRL_2_ADDR,&v,1);
  return (uint8_t)(v & (RTC_CTRL_2_AF | RTC_CTRL_2_AIE));
}

void PCF85063A_Set_Alarm(datetime_t t) {
  uint8_t b[5] = {
    (uint8_t)(dec2bcd(t.sec)  & ~RTC_ALARM),
    (uint8_t)(dec2bcd(t.min)  & ~RTC_ALARM),
    (uint8_t)(dec2bcd(t.hour) & ~RTC_ALARM),
    RTC_ALARM, RTC_ALARM
  };
  PCF85063A_WriteRegs(RTC_SECOND_ALARM, b, sizeof b);
}

void PCF85063A_Read_Alarm(datetime_t *t) {
  if (!t) return;
  uint8_t b[5] = {0};
  PCF85063A_ReadRegs(RTC_SECOND_ALARM, b, sizeof b);
  t->sec  = bcd2dec(b[0] & 0x7F);
  t->min  = bcd2dec(b[1] & 0x7F);
  t->hour = bcd2dec(b[2] & 0x3F);
  t->day  = bcd2dec(b[3] & 0x3F);
  t->dotw = bcd2dec(b[4] & 0x07);
}

/* ----- Utils ----- */
void datetime_to_str(char *s, datetime_t t) {
  sprintf(s, " %d.%d.%d %d %02d:%02d:%02d ",
          (int)t.year, (int)t.month, (int)t.day, (int)t.dotw,
          (int)t.hour, (int)t.min, (int)t.sec);
}