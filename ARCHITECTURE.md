## I²C Bus Architecture

### Current State
- Shared I²C bus (I2C_NUM_0) used by: touch panel (GT911), expander (CH422G), RTC (PCF8563)
- Simple mutex-based locking in `hal_i2c_manager`
- Gate input polling paused during screensaver to reduce contention

### Known Limitations
- Extended experiments (>60s) with continuous gate polling may experience occasional I²C timeouts
- Recommendation: Keep measurement sessions under 60 seconds

### Future Improvements
- Dedicated I²C executor task for serialized bus access (see issue #XXX)
- Reduced touch panel polling rate
- Better coordination between subsystems
