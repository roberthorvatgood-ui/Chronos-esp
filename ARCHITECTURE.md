## I²C Bus Architecture

### Current State
- **Shared I²C bus (I2C_NUM_0)** used by: touch panel (GT911), expander (CH422G), RTC (PCF8563)
- **Dedicated I²C executor task** running on Core 1 serializes all I²C operations through a queue
- Queue-based architecture with sync/async APIs prevents bus contention
- All CH422G expander operations (gate inputs, SD CS pin) use executor for atomic access
- RTC operations (PCF8563) use executor for thread-safe access
- Touch panel operations managed internally by ESP_Panel library

### Architecture Details
- **I²C Executor Task**: Dedicated FreeRTOS task on Core 1 (priority: `tskIDLE_PRIORITY + 2`)
- **Queue Size**: 32 operations (configurable)
- **Sync API**: `hal_i2c_exec_sync()` - blocks caller until operation completes
- **Async API**: `hal_i2c_exec_async()` - non-blocking with optional callback
- **Timeout Management**: Configurable per-operation timeouts (default 120-200ms)

### Benefits
- ✅ Eliminates I²C bus contention between subsystems
- ✅ Allows longer-running experiments without crashes
- ✅ Clean separation between application logic and hardware access
- ✅ Better error handling and timeout management
- ✅ Supports both blocking and non-blocking I²C operations

### Known Limitations
- Touch panel polling rate is managed by ESP_Panel library (not directly controllable)
- Extended experiments (>60s) should use pause/resume coordination during intensive I²C operations
- Gate input polling automatically pauses during screensaver to reduce bus load

### Future Improvements
- Optimize touch panel polling rate if needed
- Add I²C operation metrics/telemetry
- Consider priority queue for time-critical operations
