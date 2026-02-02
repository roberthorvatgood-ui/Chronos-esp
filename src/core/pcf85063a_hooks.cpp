
#include <Arduino.h>
#include "pcf85063a_hooks.h"
#include "waveshare_pcf85063a.h"
#include "rtc_manager.h"

/* OS/VL flag is bit7 of seconds (0x04) */

static inline void dt_to_tm(const datetime_t& in, struct tm& out) {
  out = {};
  out.tm_year = (int)in.year - 1900;
  out.tm_mon  = (int)in.month - 1;
  out.tm_mday = (int)in.day;
  out.tm_wday = (int)in.dotw;
  out.tm_hour = (int)in.hour;
  out.tm_min  = (int)in.min;
  out.tm_sec  = (int)in.sec;
}
static inline void tm_to_dt(const struct tm& in, datetime_t& out) {
  out.year  = (uint16_t)(in.tm_year + 1900);
  out.month = (uint8_t)(in.tm_mon + 1);
  out.day   = (uint8_t)(in.tm_mday);
  out.dotw  = (uint8_t)(in.tm_wday);
  out.hour  = (uint8_t)(in.tm_hour);
  out.min   = (uint8_t)(in.tm_min);
  out.sec   = (uint8_t)(in.tm_sec);
}


void pcf_rtc_one_shot_initialize()
{
    // 1) Get current (valid) system time
    struct tm now_tm {};
    rtc_get_time(now_tm);

    if ((now_tm.tm_year + 1900) < 2024) {
        Serial.println("[PCF] One-shot init aborted: system time is not valid yet.");
        return;
    }

    // 2) Write correct time into the PCF hardware RTC
    if (!pcf_rtc_write_tm(now_tm)) {
        Serial.println("[PCF] One-shot init FAILED: cannot write time to PCF.");
        return;
    }

    // 3) Clear OS/VL flag
    if (!pcf_rtc_clear_os_flag()) {
        Serial.println("[PCF] One-shot init FAILED: cannot clear OS/VL.");
        return;
    }

    // 4) Read back OS/VL to confirm
    bool os = true;
    pcf_rtc_get_os_flag(os);

    Serial.println("[PCF] ================= One-Shot RTC Initialization =================");
    Serial.println("[PCF] Wrote correct time to PCF85063A.");
    Serial.println("[PCF] Cleared OS/VL flag.");
    Serial.printf ("[PCF] OS/VL after clear = %d (expected 0)\n", os ? 1 : 0);
    Serial.println("[PCF] RTC is now fully initialized. Disconnect USB for 2–3 minutes to test retention.");
    Serial.println("=======================================================================");
}


/* Public utils */
bool pcf_rtc_read_tm(struct tm& out) {
  datetime_t t{}; PCF85063A_Read_now(&t);
  if (t.year < 2000) return false;
  dt_to_tm(t, out); return true;
}
bool pcf_rtc_write_tm(const struct tm& in) {
  datetime_t t{}; tm_to_dt(in, t);
  PCF85063A_Set_All(t); return true;
}
bool pcf_rtc_get_os_flag(bool& os_or_vl_flag) {
  uint8_t sec_raw = 0;
  if (!PCF85063A_ReadRegs(RTC_SECOND_ADDR, &sec_raw, 1)) return false;
  os_or_vl_flag = (sec_raw & 0x80) != 0;
  return true;
}
bool pcf_rtc_clear_os_flag(void) {
  uint8_t buf[7] = {0};           // sec..year
  if (!PCF85063A_ReadRegs(RTC_SECOND_ADDR, buf, sizeof buf)) return false;
  buf[0] &= 0x7F;                  // clear OS/VL
  return PCF85063A_WriteRegs(RTC_SECOND_ADDR, buf, sizeof buf);
}

/* rtc_manager hooks */


static bool hw_reader_hook(struct tm& out)
{
    datetime_t t;

    // 1) First read (may be invalid immediately after boot)
    PCF85063A_Read_now(&t);

    // 2) Retry if year is invalid
    if (t.year < 2024) {
        delay(50);                      // allow RTC crystal + I2C to settle
        PCF85063A_Read_now(&t);
    }

    // 3) If still invalid, return false -> boot will fallback
    if (t.year < 2024) {
        Serial.println("[PCF] HW reader: still invalid after retry");
        return false;
    }

    // 4) Convert to struct tm
    out.tm_year = t.year - 1900;
    out.tm_mon  = t.month - 1;
    out.tm_mday = t.day;
    out.tm_hour = t.hour;
    out.tm_min  = t.min;
    out.tm_sec  = t.sec;
    out.tm_wday = t.dotw;

    Serial.println("[PCF] HW reader: time accepted");
    return true;
}



static bool hw_writer_hook(const struct tm& in)
{
    bool ok = pcf_rtc_write_tm(in);

    // Optional: auto-clear OS/VL if it is set
    bool os = true;
    if (pcf_rtc_get_os_flag(os) && os) {
        pcf_rtc_clear_os_flag();
        Serial.println("[PCF] Auto-cleared OS/VL after time update.");
    }

    return ok;
}

bool init_pcf_hooks(void) {
  if (!PCF85063A_Init()) {
    Serial.println("[PCF] Init (OLD I2C) failed."); return false;
  }
  rtc_set_hw_time_reader(hw_reader_hook);
  rtc_set_hw_time_writer(hw_writer_hook);
  Serial.println("[PCF] RTC hooks installed (OLD I2C on I2C0 SDA=8 SCL=9).");
  return true;
}

void test_pcf_battery(void) {
  bool os=true;
  if (!pcf_rtc_get_os_flag(os)) { Serial.println("[PCF] OS/VL read failed"); return; }

  datetime_t hw{}; PCF85063A_Read_now(&hw);
  struct tm sys{}; time_t now=time(nullptr); localtime_r(&now,&sys);

  char hw_s[48]; datetime_to_str(hw_s, hw);
  char sys_s[48]; snprintf(sys_s,sizeof sys_s,"%04d-%02d-%02d %02d:%02d:%02d",
                           sys.tm_year+1900, sys.tm_mon+1, sys.tm_mday,
                           sys.tm_hour, sys.tm_min, sys.tm_sec);

  Serial.printf("[PCF] OS/VL=%d  RTC=%s  SYS=%s\n", os?1:0, hw_s, sys_s);
  if (os) {
    Serial.println("[PCF] OS/VL=1 -> Set time (NTP/manual), then pcf_rtc_clear_os_flag(), power off a few minutes, power on.");
  } else {
    Serial.println("[PCF] OS/VL=0 -> Ready for retention test (remove USB 2–3 min, reconnect).");
  }
}
