#ifndef WAVESHARE_IO_PORT_H
#define WAVESHARE_IO_PORT_H

// Minimal Waveshare CH422G pin / mask definitions used by setup()
// Adjust SDA/SCL pin numbers if your board uses different pins.

#include <stdint.h>

// CH422G I2C address macro provided by esp-io-expander libs; ensure that header is included
// where this header is used. If not available, replace with the numeric address for CH422G.
#ifndef EXAMPLE_I2C_ADDR
#define EXAMPLE_I2C_ADDR    (ESP_IO_EXPANDER_I2C_CH422G_ADDRESS)
#endif

// Default I2C pins used by Waveshare S3 boards (adjust if needed)
#ifndef EXAMPLE_I2C_SDA_PIN
#define EXAMPLE_I2C_SDA_PIN 8   // SDA
#endif
#ifndef EXAMPLE_I2C_SCL_PIN
#define EXAMPLE_I2C_SCL_PIN 9   // SCL
#endif

// Digital input indices on the CH422G used for Gate A/B in this project
#ifndef DI0
#define DI0 0
#endif
#ifndef DI0_mask
#define DI0_mask (1U << DI0)
#endif

#ifndef DI1
#define DI1 5
#endif
#ifndef DI1_mask
#define DI1_mask (1U << DI1)
#endif

#endif // WAVESHARE_IO_PORT_H