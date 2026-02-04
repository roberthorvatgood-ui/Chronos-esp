#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Request function type executed in executor context. Return ESP_OK on success.
typedef esp_err_t (*hal_i2c_request_fn_t)(void* ctx);

// Optional async callback: invoked from executor task after op completes.
typedef void (*hal_i2c_cb_t)(void* ctx, esp_err_t res);

// Initialize executor. Returns true on success. Must be called early (hal::init or similar).
bool hal_i2c_executor_init(size_t queue_len);

// Synchronous call: submit op and wait up to timeout_ms for completion.
// op runs in executor task context and must not block the caller (it runs on core0).
esp_err_t hal_i2c_exec_sync(hal_i2c_request_fn_t op, void* ctx, uint32_t timeout_ms);

// Asynchronous call: enqueue op and return immediately. Callback runs on executor task when done.
// Returns true if enqueued, false if queue full.
bool hal_i2c_exec_async(hal_i2c_request_fn_t op, void* ctx, hal_i2c_cb_t cb);

#ifdef __cplusplus
}
#endif