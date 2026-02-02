// (only the waveshare_io_test function shown — keep rest of file unchanged)

void waveshare_io_test(void)
{
    Serial.println("Initialize IO expander");
    /* Initialize IO expander */
    expander = new esp_expander::CH422G(EXAMPLE_I2C_SCL_PIN, EXAMPLE_I2C_SDA_PIN, EXAMPLE_I2C_ADDR);
    expander->init();
    expander->begin();

    Serial.println("Set the OC pin to push-pull output mode.");
    expander->enableOC_PushPull();

    Serial.println("Set the IO0-7 pin to input mode.");
    expander->enableAllIO_Input();

    // Set output pins to default high
    expander->digitalWrite(DO0, 1);
    expander->digitalWrite(DO1, 1);

    Serial.println("IO test example start");
    // Note: wiring recommended: pushbutton between EXIOx and GND, with pull-up (expander internal or external).
    // Buttons are active-low when using pull-ups, so the code tests for bit == 0.
    uint8_t events = 0;
    while (1)
    {
        // single read of both DI0 & DI1
        uint64_t v = expander->multiDigitalRead(PIN_GATE_A_mask | PIN_GATE_B_mask);

        // active-low: bit == 0 when pressed
        if ((v & PIN_GATE_A_mask) == 0) {
            events++;
        }

        if ((v & PIN_GATE_B_mask) == 0) {
            events++;
        }

        // sample outputs toggle for visual test
        expander->digitalWrite(DO0, (events & 1) ? 1 : 0);
        expander->digitalWrite(DO1, (events & 2) ? 1 : 0);

        delay(10);

        if (events >= 2) break;
    }
}