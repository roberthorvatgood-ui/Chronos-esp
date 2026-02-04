/*****
 * hal_i2c_executor.cpp
 * Central I2C executor implementation for Chronos-esp
 * 
 * [Created: 2026-02-04]
 *****/
#include "hal_i2c_executor.h"
#include <Arduino.h>

namespace hal {

// ── Internal Types and State ───────────────────────────────────────────────

enum RequestType {
  REQ_SYNC,   // Synchronous request with semaphore
  REQ_ASYNC   // Asynchronous request with callback
};

struct I2CRequest {
  RequestType type;
  hal_i2c_request_fn_t op;
  void* ctx;
  
  // For sync requests
  SemaphoreHandle_t done_sem;
  esp_err_t* result_ptr;
  
  // For async requests
  hal_i2c_cb_t callback;
};

static TaskHandle_t s_executor_task = nullptr;
static QueueHandle_t s_request_queue = nullptr;
static size_t s_queue_len = 0;

// Statistics
static uint32_t s_queue_high_water = 0;
static uint32_t s_queue_full_count = 0;

// ── Executor Task ──────────────────────────────────────────────────────────

static void executor_task(void* param) {
  (void)param;
  
  Serial.println("[I2C Executor] Task started on core 0");
  
  I2CRequest req;
  
  while (true) {
    // Block waiting for requests
    if (xQueueReceive(s_request_queue, &req, portMAX_DELAY) == pdTRUE) {
      // Update queue statistics
      UBaseType_t waiting = uxQueueMessagesWaiting(s_request_queue);
      if (waiting > s_queue_high_water) {
        s_queue_high_water = waiting;
      }
      
      // Execute the operation
      esp_err_t result = ESP_FAIL;
      if (req.op) {
        result = req.op(req.ctx);
      }
      
      // Handle completion based on request type
      if (req.type == REQ_SYNC) {
        // Synchronous: store result and signal semaphore
        if (req.result_ptr) {
          *req.result_ptr = result;
        }
        if (req.done_sem) {
          xSemaphoreGive(req.done_sem);
        }
      } else if (req.type == REQ_ASYNC) {
        // Asynchronous: invoke callback if provided
        if (req.callback) {
          req.callback(req.ctx, result);
        }
      }
    }
  }
}

// ── Public API ─────────────────────────────────────────────────────────────

bool hal_i2c_executor_init(size_t queue_len) {
  if (s_executor_task) {
    Serial.println("[I2C Executor] Already initialized");
    return true;
  }
  
  s_queue_len = queue_len;
  
  // Create the request queue
  s_request_queue = xQueueCreate(queue_len, sizeof(I2CRequest));
  if (!s_request_queue) {
    Serial.println("[I2C Executor] Failed to create queue");
    return false;
  }
  
  // Create the executor task pinned to core 0
  BaseType_t ret = xTaskCreatePinnedToCore(
    executor_task,           // Task function
    "i2c_executor",          // Name
    4096,                    // Stack size
    nullptr,                 // Parameters
    5,                       // Priority (slightly above normal)
    &s_executor_task,        // Task handle
    0                        // Core 0
  );
  
  if (ret != pdPASS) {
    Serial.println("[I2C Executor] Failed to create task");
    vQueueDelete(s_request_queue);
    s_request_queue = nullptr;
    return false;
  }
  
  Serial.printf("[I2C Executor] Initialized with queue size %u\n", (unsigned)queue_len);
  return true;
}

esp_err_t hal_i2c_exec_sync(hal_i2c_request_fn_t op, void* ctx, uint32_t timeout_ms) {
  if (!s_executor_task || !s_request_queue) {
    Serial.println("[I2C Executor] Not initialized");
    return ESP_ERR_INVALID_STATE;
  }
  
  if (!op) {
    return ESP_ERR_INVALID_ARG;
  }
  
  // Create a semaphore for synchronization (stack-allocated request pattern)
  SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();
  if (!done_sem) {
    Serial.println("[I2C Executor] Failed to create semaphore");
    return ESP_ERR_NO_MEM;
  }
  
  esp_err_t result = ESP_FAIL;
  
  // Prepare the request
  I2CRequest req;
  req.type = REQ_SYNC;
  req.op = op;
  req.ctx = ctx;
  req.done_sem = done_sem;
  req.result_ptr = &result;
  req.callback = nullptr;
  
  // Send to queue
  TickType_t queue_timeout = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms / 2);
  if (xQueueSend(s_request_queue, &req, queue_timeout) != pdTRUE) {
    Serial.println("[I2C Executor] Queue full, request dropped");
    s_queue_full_count++;
    vSemaphoreDelete(done_sem);
    return ESP_ERR_TIMEOUT;
  }
  
  // Wait for completion
  TickType_t wait_timeout = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  if (xSemaphoreTake(done_sem, wait_timeout) != pdTRUE) {
    Serial.println("[I2C Executor] Operation timeout");
    vSemaphoreDelete(done_sem);
    return ESP_ERR_TIMEOUT;
  }
  
  vSemaphoreDelete(done_sem);
  return result;
}

bool hal_i2c_exec_async(hal_i2c_request_fn_t op, void* ctx, hal_i2c_cb_t cb) {
  if (!s_executor_task || !s_request_queue) {
    Serial.println("[I2C Executor] Not initialized");
    return false;
  }
  
  if (!op) {
    return false;
  }
  
  // Prepare the request
  I2CRequest req;
  req.type = REQ_ASYNC;
  req.op = op;
  req.ctx = ctx;
  req.done_sem = nullptr;
  req.result_ptr = nullptr;
  req.callback = cb;
  
  // Try to send (don't wait, return false if queue full)
  if (xQueueSend(s_request_queue, &req, 0) != pdTRUE) {
    s_queue_full_count++;
    return false;
  }
  
  return true;
}

void hal_i2c_executor_stats(uint32_t* queue_high_water, uint32_t* queue_full_count) {
  if (queue_high_water) {
    *queue_high_water = s_queue_high_water;
  }
  if (queue_full_count) {
    *queue_full_count = s_queue_full_count;
  }
}

} // namespace hal
