#include "hal_i2c_executor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "hal:i2c_exec";

struct i2c_request_item {
  hal_i2c_request_fn_t fn;
  void* ctx;
  hal_i2c_cb_t cb;            // optional async callback
  SemaphoreHandle_t done;     // for sync requests (caller waits on this)
  esp_err_t result;
};

static QueueHandle_t s_reqq = NULL;
static TaskHandle_t   s_task = NULL;
static size_t         s_queue_len = 0;
static volatile bool s_executor_ready = false;

// Executor task: runs on core 1 and performs I2C ops serially.
static void i2c_executor_task(void* arg)
{
  (void)arg;
  i2c_request_item item;
  for (;;) {
    if (xQueueReceive(s_reqq, &item, portMAX_DELAY) == pdTRUE) {
      // Execute (fn may be NULL; guard)
      esp_err_t res = ESP_FAIL;
      if (item.fn) {
        res = item.fn(item.ctx);
      } else {
        res = ESP_ERR_INVALID_ARG;
      }
      item.result = res;

      // signal sync caller
      if (item.done) {
        xSemaphoreGive(item.done);
      }

      // call async callback if provided
      if (item.cb) {
        // protect callback running time inside executor context; callback must be short
        item.cb(item.ctx, res);
      }
    }
  }
}

bool hal_i2c_executor_init(size_t queue_len)
{
  if (s_reqq) return true; // already initialized

  if (queue_len == 0) queue_len = 16;
  s_reqq = xQueueCreate(queue_len, sizeof(i2c_request_item));
  if (!s_reqq) {
    ESP_LOGE(TAG, "queue create failed");
    return false;
  }
  s_queue_len = queue_len;

  // Create pinned task on core 0 (CHANGED from core 1)
  BaseType_t r = xTaskCreatePinnedToCore(i2c_executor_task, "i2c_exec", 4096, NULL,
                                         tskIDLE_PRIORITY + 2, &s_task, 0);
  if (r != pdPASS) {
    ESP_LOGE(TAG, "task create failed");
    vQueueDelete(s_reqq);
    s_reqq = NULL;
    s_task = NULL;
    return false;
  }

  // Wait for task to actually start
  vTaskDelay(pdMS_TO_TICKS(100));
  s_executor_ready = true;

  ESP_LOGI(TAG, "I2C executor started (queue_len=%u, core=1)", (unsigned)s_queue_len);
  return true;
}

bool hal_i2c_executor_is_ready() {
  return s_executor_ready;
}

esp_err_t hal_i2c_exec_sync(hal_i2c_request_fn_t op, void* ctx, uint32_t timeout_ms)
{
  if (!s_reqq) return ESP_ERR_INVALID_STATE;
  i2c_request_item item;
  memset(&item, 0, sizeof(item));
  item.fn = op;
  item.ctx = ctx;
  item.cb = NULL;
  item.done = xSemaphoreCreateBinary();
  if (!item.done) return ESP_ERR_NO_MEM;

  // Enqueue (wait up to 500ms for queue space - CHANGED from 50ms)
  if (xQueueSend(s_reqq, &item, pdMS_TO_TICKS(500)) != pdTRUE) {
    vSemaphoreDelete(item.done);
    ESP_LOGW(TAG, "exec_sync: queue full");
    return ESP_ERR_TIMEOUT;
  }

  // Monitor queue depth; warn when contention is high (diagnostic only, volatile for cross-task visibility)
  static volatile int max_queue_depth = 0;
  int cur_depth = (int)uxQueueMessagesWaiting(s_reqq);
  if (cur_depth > max_queue_depth) {
    max_queue_depth = cur_depth;
    if (cur_depth > 20) {
      ESP_LOGW(TAG, "I2C queue depth warning: %d/%u", cur_depth, (unsigned)s_queue_len);
    }
  }

  // Wait for completion
  if (xSemaphoreTake(item.done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    vSemaphoreDelete(item.done);
    ESP_LOGW(TAG, "exec_sync: timeout waiting for result");
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t res = item.result;
  vSemaphoreDelete(item.done);
  return res;
}

bool hal_i2c_exec_async(hal_i2c_request_fn_t op, void* ctx, hal_i2c_cb_t cb)
{
  if (!s_reqq) return false;
  i2c_request_item item;
  memset(&item, 0, sizeof(item));
  item.fn = op;
  item.ctx = ctx;
  item.cb = cb;
  item.done = NULL;

  // Nonblocking enqueue; caller may retry
  if (xQueueSend(s_reqq, &item, 0) != pdTRUE) {
    ESP_LOGW(TAG, "exec_async: queue full");
    return false;
  }
  return true;
}