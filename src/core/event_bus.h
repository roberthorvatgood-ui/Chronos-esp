
// src/core/event_bus.h
#pragma once
#include <Arduino.h>

/**
 * Simple event bus:
 *  - publish(EventType, payload) queues an event
 *  - subscribe(callback) registers a handler
 *  - dispatch() delivers queued events to all subscribers
 */

enum EventType : uint8_t {
    EVT_GATE_A_RISE,
    EVT_GATE_A_FALL,
    EVT_GATE_B_RISE,
    EVT_GATE_B_FALL,
};

struct Event {
  EventType type;
  uint32_t  a;      // optional payload (e.g., tick ms)
};

using EventCallback = void (*)(const Event&);

class EventBus {
public:
  static constexpr size_t MAX_QUEUE = 32;
  static constexpr size_t MAX_SUBS  = 8;

  void publish(EventType t, uint32_t a = 0) {
    size_t next = (head + 1) % MAX_QUEUE;
    if (next == tail) return;           // drop if full
    queue[head] = { t, a };
    head = next;
  }

  void subscribe(EventCallback cb) {
    if (cb && subCount < MAX_SUBS) subs[subCount++] = cb;
  }

  void dispatch() {
    while (tail != head) {
      const Event e = queue[tail];
      tail = (tail + 1) % MAX_QUEUE;
      for (size_t i = 0; i < subCount; ++i) {
        if (subs[i]) subs[i](e);
      }
    }
  }

private:
  Event        queue[MAX_QUEUE]{};
  size_t       head = 0, tail = 0;
  EventCallback subs[MAX_SUBS]{};
  size_t        subCount = 0;
};


// src/core/event_bus.h  (append near the end)
extern EventBus gBus;   // defined once in your .ino

// Global instance is defined in your .ino:
//   EventBus gBus;
