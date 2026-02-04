/*****
 * hal_i2c_executor.h
 * Central I2C executor for Chronos-esp
 * 
 * Purpose: Single dedicated I2C executor task that performs all physical I2C 
 *          transactions on behalf of the system. Runs on core 0 to eliminate 
 *          I2C contention and prevent Interrupt WDT panics.
 * 
 * [Created: 2026-02-04]
 *****/
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_err.h>

namespace hal {

// ── I2C Executor Types ─────────────────────────────────────────────────────

/**
 * Request operation function type.
 * The function is called within the executor task context on core 0.
 * @param ctx User context pointer
 * @return ESP_OK on success, error code otherwise
 */
using hal_i2c_request_fn_t = esp_err_t (*)(void* ctx);

/**
 * Callback function type for async requests.
 * Called on executor task (core 0) after request completes.
 * @param ctx User context pointer
 * @param res Result of the operation
 */
using hal_i2c_cb_t = void (*)(void* ctx, esp_err_t res);

// ── I2C Executor API ───────────────────────────────────────────────────────

/**
 * Initialize the I2C executor.
 * Creates the executor task pinned to core 0 and the request queue.
 * 
 * @param queue_len Queue size (default 16, sufficient for most applications)
 * @return true on success, false on failure
 * 
 * Note: Must be called early in hal::init() or hal::begin() before any
 *       subsystem tries to use the executor.
 */
bool hal_i2c_executor_init(size_t queue_len = 16);

/**
 * Execute an I2C operation synchronously.
 * Blocks the caller until the operation completes or times out.
 * Safe to call from any task on any core.
 * 
 * @param op Operation function to execute on core 0
 * @param ctx Context pointer passed to op
 * @param timeout_ms Maximum time to wait (in milliseconds)
 * @return Result from the operation, or ESP_ERR_TIMEOUT if timeout
 * 
 * Note: Uses a stack-allocated request object to avoid heap allocation.
 *       The op function runs in executor context on core 0.
 */
esp_err_t hal_i2c_exec_sync(hal_i2c_request_fn_t op, void* ctx, uint32_t timeout_ms);

/**
 * Queue an I2C operation asynchronously.
 * Returns immediately after queuing the request.
 * Safe to call from any task; use FromISR-friendly alternatives if in ISR.
 * 
 * @param op Operation function to execute on core 0
 * @param ctx Context pointer passed to op and cb
 * @param cb Callback invoked after op completes (or nullptr)
 * @return true if queued successfully, false if queue full
 * 
 * Note: The callback is invoked on the executor task (core 0).
 *       Do not call blocking functions or UI updates from the callback.
 */
bool hal_i2c_exec_async(hal_i2c_request_fn_t op, void* ctx, hal_i2c_cb_t cb);

/**
 * Get executor statistics.
 * Useful for debugging and monitoring queue depth.
 * 
 * @param queue_high_water Maximum queue depth reached (out parameter)
 * @param queue_full_count Number of times queue was full (out parameter)
 */
void hal_i2c_executor_stats(uint32_t* queue_high_water, uint32_t* queue_full_count);

} // namespace hal
