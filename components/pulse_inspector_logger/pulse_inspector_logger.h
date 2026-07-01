#pragma once

#include "esphome/core/component.h"

#ifdef USE_ESP_IDF

#include <atomic>
#include <cstdint>

#include "esphome/components/pulse_inspector/pulse_inspector.h"

namespace esphome {
namespace pulse_inspector_logger {

// Minimal child component built on top of pulse_inspector. Subscribes to
// the pulse stream of a single channel and periodically reports edge rate
// from the main ESPHome loop.
//
// Treat this class as a template for real decoders, data loggers or
// packet-injection modules: the key idea is that the heavy-lifting happens
// inside the callback (which runs in the channel's FreeRTOS task), and the
// slow parts (sensor publishes, logging of aggregates) happen in loop().
class PulseInspectorLogger : public Component {
 public:
  void set_channel(pulse_inspector::PulseInspectorChannel *c) { channel_ = c; }
  void set_report_interval(uint32_t ms) { report_interval_ms_ = ms; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  void on_pulse(const pulse_inspector::PulseItem &item);

  pulse_inspector::PulseInspectorChannel *channel_{nullptr};
  uint32_t report_interval_ms_{5000};
  uint32_t last_report_ms_{0};

  // Updated from the task context, read from loop(). Atomics keep the
  // read/write pair correct without taking a lock.
  std::atomic<uint32_t> edge_counter_{0};
  std::atomic<uint32_t> last_level_{0};
  uint32_t previous_reported_count_{0};
};

}  // namespace pulse_inspector_logger
}  // namespace esphome

#endif  // USE_ESP_IDF
