
#pragma once
#include <time.h>
#include <stdbool.h>

bool init_pcf_hooks(void);          // call once from setup()
void test_pcf_battery(void);        // prints OS/VL + RTC + SYS time

// optional utilities if you need them elsewhere
bool pcf_rtc_read_tm(struct tm& out);               // HW -> tm
bool pcf_rtc_write_tm(const struct tm& in);         // tm -> HW
bool pcf_rtc_get_os_flag(bool& os_or_vl_flag);      // read OS/VL (1 = invalid)
bool pcf_rtc_clear_os_flag(void);                   // clear OS/VL by rewriting seconds
void pcf_rtc_one_shot_initialize(void);