#include "src/drivers/expander_output_blink.h"
#include "src/drivers/hal_panel.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdlib>

/*
  expander_output_blink.cpp

  Implements a blocking toggler and a non-blocking starter (FreeRTOS task).
*/

#ifndef DOUT_EXIO0
#define DOUT_EXIO0 0
#endif

#ifndef DOUT_EXIO1
#define DOUT_EXIO1 1
#endif

void expander_toggle_terminal_do01_blocking(uint32_t period_ms,
                                            uint32_t cycles,
                                            uint8_t exio_do0,
                                            uint8_t exio_do1)
{
  // Ensure expander is ready
  if (!hal::expander_wait_ready(1000)) {
    Serial.println("[BLINK] Expander not ready; aborting toggle test.");
    return;
  }

  // Configure both pins as outputs
  hal::expander_pinMode(exio_do0, true);
  hal::expander_pinMode(exio_do1, true);

  Serial.printf("[BLINK] Starting expander toggle DO%d/DO%d: period=%ums cycles=%u\n",
                (unsigned)exio_do0, (unsigned)exio_do1, (unsigned)period_ms, (unsigned)cycles);

  bool state = false;
  uint32_t iteration = 0;

  while (cycles == 0 || iteration < cycles) {
    state = !state;
    // Drive DO0 = state, DO1 = !state (opposite states)
    hal::expander_digitalWrite(exio_do0, state);
    hal::expander_digitalWrite(exio_do1, !state);

    Serial.printf("[BLINK] Iter %u: DO%d=%d  DO%d=%d\n",
                  (unsigned)iteration,
                  (unsigned)exio_do0, state ? 1 : 0,
                  (unsigned)exio_do1, state ? 0 : 1);

    iteration++;
    vTaskDelay(pdMS_TO_TICKS(period_ms));
  }

  // Leave outputs in known safe state: both LOW
  hal::expander_digitalWrite(exio_do0, false);
  hal::expander_digitalWrite(exio_do1, false);
  Serial.println("[BLINK] Toggle test finished; outputs set LOW.");
}

// Internal task wrapper; pvParameters must point to a uint32_t[4] allocation:
// [0] = period_ms, [1] = cycles, [2] = exio_do0, [3] = exio_do1
static void expander_toggle_task(void* pvParameters)
{
  uint32_t *p = (uint32_t*)pvParameters;
  if (!p) {
    vTaskDelete(NULL);
    return;
  }
  uint32_t period_ms = p[0];
  uint32_t cycles = p[1];
  uint8_t exio_do0 = (uint8_t)p[2];
  uint8_t exio_do1 = (uint8_t)p[3];
  // Run blocking toggler on this task
  expander_toggle_terminal_do01_blocking(period_ms, cycles, exio_do0, exio_do1);
  free(p);
  vTaskDelete(NULL);
}

bool expander_toggle_terminal_do01_start(uint32_t period_ms,
                                         uint32_t cycles,
                                         uint8_t exio_do0,
                                         uint8_t exio_do1)
{
  // Allocate parameters for task
  uint32_t *p = (uint32_t*)malloc(sizeof(uint32_t) * 4);
  if (!p) {
    Serial.println("[BLINK] Failed to allocate task params");
    return false;
  }
  p[0] = period_ms;
  p[1] = cycles;
  p[2] = exio_do0;
  p[3] = exio_do1;

  BaseType_t r = xTaskCreate(
      expander_toggle_task,
      "expander_blink",
      4096 / sizeof(StackType_t), // stack size (words)
      p,
      tskIDLE_PRIORITY + 1,
      NULL);

  if (r != pdPASS) {
    Serial.println("[BLINK] Failed to create task");
    free(p);
    return false;
  }
  Serial.println("[BLINK] Toggle task started");
  return true;
}