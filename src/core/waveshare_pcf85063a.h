
/*
 * waveshare_pcf85063a.h
 * OLD IDF I2C driver version (no Wire, no i2c_master_bus_ng)
 */

#ifndef INC_PCF85063A_H_
#define INC_PCF85063A_H_

#include <stdint.h>
#include <stddef.h>
#include "driver/i2c.h"          // OLD driver API

/* -------- Pins per your schematic -------- */
#define I2C_RTC_PORT        I2C_NUM_0      // use controller 2
#define I2C_RTC_SDA_GPIO    (gpio_num_t)8
#define I2C_RTC_SCL_GPIO    (gpio_num_t)9
#define I2C_RTC_FREQ_HZ     400000
#define RTC_MASTER_INT_IO   6

/* PCF85063A (7-bit addr) */
#define PCF85063A_ADDRESS   0x51

/* Register map */
#define RTC_CTRL_1_ADDR     0x00
#define RTC_CTRL_2_ADDR     0x01
#define RTC_OFFSET_ADDR     0x02
#define RTC_RAM_by_ADDR     0x03
#define RTC_SECOND_ADDR     0x04  // bit7 = OS/VL
#define RTC_MINUTE_ADDR     0x05
#define RTC_HOUR_ADDR       0x06
#define RTC_DAY_ADDR        0x07
#define RTC_WDAY_ADDR       0x08
#define RTC_MONTH_ADDR      0x09
#define RTC_YEAR_ADDR       0x0A
#define RTC_SECOND_ALARM    0x0B
#define RTC_MINUTE_ALARM    0x0C
#define RTC_HOUR_ALARM      0x0D
#define RTC_DAY_ALARM       0x0E
#define RTC_WDAY_ALARM      0x0F
#define RTC_TIMER_VAL       0x10
#define RTC_TIMER_MODE      0x11

/* Bits */
#define RTC_ALARM             0x80
#define RTC_CTRL_1_CAP_SEL    0x01        // 12.5pF
#define RTC_CTRL_1_12_24      0x02        // 0=24h
#define RTC_CTRL_1_SR         0x10        // software reset
#define RTC_CTRL_1_STOP       0x20
#define RTC_CTRL_1_DEFAULT    0x00

#define RTC_CTRL_2_TF         0x08
#define RTC_CTRL_2_HMI        0x10
#define RTC_CTRL_2_MI         0x20
#define RTC_CTRL_2_AF         0x40
#define RTC_CTRL_2_AIE        0x80
#define RTC_CTRL_2_DEFAULT    0x00

#define YEAR_OFFSET           1970

typedef struct {
  uint16_t year;
  uint8_t  month;
  uint8_t  day;
  uint8_t  dotw;
  uint8_t  hour;
  uint8_t  min;
  uint8_t  sec;
} datetime_t;

/* Public API (same names as the Waveshare example) */
bool    PCF85063A_Init(void);                // installs OLD driver on I2C1
void    PCF85063A_Reset(void);
void    PCF85063A_Set_Time(datetime_t time);
void    PCF85063A_Set_Date(datetime_t date);
void    PCF85063A_Set_All(datetime_t time);
void    PCF85063A_Read_now(datetime_t *time);
void    PCF85063A_Enable_Alarm(void);
uint8_t PCF85063A_Get_Alarm_Flag(void);
void    PCF85063A_Set_Alarm(datetime_t time);
void    PCF85063A_Read_Alarm(datetime_t *time);
void    datetime_to_str(char *datetime_str, datetime_t time);

/* Lightweight LL helpers for hooks/tests */
bool    PCF85063A_ReadRegs(uint8_t reg, uint8_t *data, size_t len);
bool    PCF85063A_WriteRegs(uint8_t reg, const uint8_t *data, size_t len);

#endif /* INC_PCF85063A_H_ */
