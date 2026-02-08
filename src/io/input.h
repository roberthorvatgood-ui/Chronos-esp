#pragma once

struct Buttons {
  int prevSelect = HIGH;
  int prevDown   = HIGH;
};

void input_init();
void input_poll_and_publish(Buttons &btns);
void input_read(Buttons &btns, int &currentA, int &currentB);

// Configure pushbuttons on expander (set to -1 to disable)
void input_configure_pushbuttons(int select_exio = -1, int down_exio = -1, bool active_low = true);

// Register a button edge callback: id 0 = Select, 1 = Down.
// pressed==true for press event, false for release event.
using input_button_cb_t = void (*)(int id, bool pressed);
void input_set_button_callback(input_button_cb_t cb);