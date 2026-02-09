/*
 * waveshare_pcf85063a.cpp
 * OLD IDF I2C driver version with HAL I²C executor integration
 * [Updated: 2026-02-08] Added executor + mutex protection
 */

#include "waveshare_pcf85063a.h"
#include <Arduino.h>
#include <time.h>
#include "../drivers/hal_i2c_manager.h"
#include "../drivers/hal_i2c_executor.h"

static inline uint8_t dec2bcd(uint8_t v){ return (uint8_t)(((v/10)<<4) | (v%10)); }
static inline uint8_t bcd2dec(uint8_t v){ return (uint8_t)(((v>>4)*10) + (v & 0x0F)); }

void datetime_to_str(char *datetime_str, datetime_t time) {
  snprintf(datetime_str, 48, "%04d.%d.%d %d %02d:%02d:%02d",
           time.year, time.month, time.day, time.dotw,
           time.hour, time.min, time.sec);
}

/* ----- Executor contexts ----- */
struct rtc_read_ctx {
  uint8_t reg;
  uint8_t* data;
  size_t len;
};

struct rtc_write_ctx {
  uint8_t reg;
  const uint8_t* data;
  size_t len;
};

/* ----- Executor functions (run on I²C executor task) ----- */
static esp_err_t rtc_read_executor(void* ctx) {
  rtc_read_ctx* c = (rtc_read_ctx*)ctx;
  
  if (!hal::i2c_lock(50)) {
    return ESP_ERR_TIMEOUT;
  }
  
  esp_err_t res = i2c_master_write_read_device(
    I2C_RTC_PORT, PCF85063A_ADDRESS, 
    &c->reg, 1, c->data, c->len, pdMS_TO_TICKS(50)
  );
  
  hal::i2c_unlock();
  return res;
}

static esp_err_t rtc_write_executor(void* ctx) {
  rtc_write_ctx* c = (rtc_write_ctx*)ctx;
  
  if (!hal::i2c_lock(50)) {
    return ESP_ERR_TIMEOUT;
  }
  
  uint8_t buf[1+8];
  if (c->len > 8) {
    hal::i2c_unlock();
    return ESP_ERR_INVALID_ARG;
  }
  
  buf[0] = c->reg;
  for (size_t i = 0; i < c->len; i++) {
    buf[1+i] = c->data[i];
  }
  
  esp_err_t res = i2c_master_write_to_device(
    I2C_RTC_PORT, PCF85063A_ADDRESS, 
    buf, 1 + c->len, pdMS_TO_TICKS(50)
  );
  
  hal::i2c_unlock();
  return res;
}

/* ----- Public LL helpers (now use executor) ----- */
bool PCF85063A_ReadRegs(uint8_t reg, uint8_t *data, size_t len) {
  rtc_read_ctx ctx = { reg, data, len };
  return hal_i2c_exec_sync(rtc_read_executor, &ctx, 200) == ESP_OK;
}

bool PCF85063A_WriteRegs(uint8_t reg, const uint8_t *data, size_t len) {
  rtc_write_ctx ctx = { reg, data, len };
  return hal_i2c_exec_sync(rtc_write_executor, &ctx, 200) == ESP_OK;
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
  }

  e = i2c_driver_install(I2C_RTC_PORT, I2C_MODE_MASTER, 0, 0, 0);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
    printf("[PCF] i2c_driver_install(I2C0) err=0x%x — will try to reuse existing driver\n", e);
  }

  // Sanity probe: read seconds register
  uint8_t probe = 0xFF;
  if (!PCF85063A_ReadRegs(RTC_SECOND_ADDR, &probe, 1)) {
    printf("[PCF] probe read failed on I2C0 (sec reg). Check pins 8/9 and port usage.\n");
    return false;
  }

  printf("[PCF] RTC hooks installed (OLD I2C on I2C0 SDA=8 SCL=9).\n");
  s_inited = true;
  return true;
}

void PCF85063A_Reset(void) {
  uint8_t v = RTC_CTRL_1_SR;
  PCF85063A_WriteRegs(RTC_CTRL_1_ADDR, &v, 1);
  delay(10);
  v = RTC_CTRL_1_DEFAULT;
  PCF85063A_WriteRegs(RTC_CTRL_1_ADDR, &v, 1);
  v = RTC_CTRL_2_DEFAULT;
  PCF85063A_WriteRegs(RTC_CTRL_2_ADDR, &v, 1);
}

void PCF85063A_Set_Time(datetime_t time) {
  uint8_t buf[3];
  buf[0] = dec2bcd(time.sec);
  buf[1] = dec2bcd(time.min);
  buf[2] = dec2bcd(time.hour);
  PCF85063A_WriteRegs(RTC_SECOND_ADDR, buf, 3);
}

void PCF85063A_Set_Date(datetime_t date) {
  uint8_t buf[4];
  buf[0] = dec2bcd(date.day);
  buf[1] = date.dotw;
  buf[2] = dec2bcd(date.month);
  buf[3] = dec2bcd((uint8_t)(date.year - 2000));
  PCF85063A_WriteRegs(RTC_DAY_ADDR, buf, 4);
}

void PCF85063A_Set_All(datetime_t time) {
  uint8_t buf[7];
  buf[0] = dec2bcd(time.sec);
  buf[1] = dec2bcd(time.min);
  buf[2] = dec2bcd(time.hour);
  buf[3] = dec2bcd(time.day);
  buf[4] = time.dotw;
  buf[5] = dec2bcd(time.month);
  buf[6] = dec2bcd((uint8_t)(time.year - 2000));
  PCF85063A_WriteRegs(RTC_SECOND_ADDR, buf, 7);
}

void PCF85063A_Read_now(datetime_t *time) {
  uint8_t buf[7];
  if (!PCF85063A_ReadRegs(RTC_SECOND_ADDR, buf, 7)) {
    memset(time, 0, sizeof(*time));
    return;
  }
  time->sec   = bcd2dec(buf[0] & 0x7F);
  time->min   = bcd2dec(buf[1] & 0x7F);
  time->hour  = bcd2dec(buf[2] & 0x3F);
  time->day   = bcd2dec(buf[3] & 0x3F);
  time->dotw  = buf[4] & 0x07;
  time->month = bcd2dec(buf[5] & 0x1F);
  time->year  = 2000 + bcd2dec(buf[6]);
}

void PCF85063A_Enable_Alarm(void) {
  uint8_t v = RTC_CTRL_2_AIE;
  PCF85063A_WriteRegs(RTC_CTRL_2_ADDR, &v, 1);
}

uint8_t PCF85063A_Get_Alarm_Flag(void) {
  uint8_t v = 0;
  PCF85063A_ReadRegs(RTC_CTRL_2_ADDR, &v, 1);
  return (v & RTC_CTRL_2_AF) ? 1 : 0;
}