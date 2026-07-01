#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_ESP_IDF

#include <functional>
#include <vector>
#include <cstdint>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace esphome {
namespace pulse_inspector {

// Minimum event shoved from the ISR into the per-channel queue. We keep it
// tiny on purpose: copying more than a few bytes in an ISR is painful and
// we want the queue to hold as many events as possible.
struct PulseItem {
  uint32_t cycle;  // xthal_get_ccount() at the moment of the edge
  bool level;      // new logical level (after invert_in is applied)
};

// Callback invoked for every PulseItem drained by the background task.
// WARNING: runs in the per-channel FreeRTOS task context, NOT on the main
// ESPHome loop. Heavy work or calls into thread-unsafe ESPHome APIs should
// be deferred via Component::defer(...) or App.scheduler.
using PulseCallback = std::function<void(const PulseItem &)>;

// Callback invoked once a full "packet" was reassembled by a decoder that
// lives on top of the inspector. PulseInspector itself does not invoke
// this callback (no decoder is built in); it is only plumbed through so
// decoder components can share the same surface.
using PacketCallback = std::function<void(const uint8_t *data, size_t len)>;

class PulseInspectorChannel {
 public:
  void set_input_pin(InternalGPIOPin *pin) { this->input_pin_ = pin; }
  void set_output_pin(InternalGPIOPin *pin) { this->output_pin_ = pin; }
  void set_invert_in(bool v) { this->invert_in_ = v; }
  void set_invert_out(bool v) { this->invert_out_ = v; }
  void set_queue_size(size_t v) { this->queue_size_ = v; }

  // Initialize GPIO, queue and background task. Returns true on success.
  bool setup();

  // Print channel configuration via ESP_LOG*.
  void dump_config(size_t index) const;

  // Periodic diagnostics (called from PulseInspector::loop()).
  void log_diagnostics(size_t index);

  // Spin up the FreeRTOS processing task and enable the edge interrupt.
  // Called from PulseInspector::loop() on the first iteration so that
  // all child components had a chance to register their pulse callbacks
  // during their own setup() phase. Registering callbacks while the task
  // is iterating pulse_callbacks_ is a data race that has been observed
  // to corrupt the std::vector and crash via illegal instruction.
  bool start_processing();

  // --- Public API for child components ---------------------------------
  // Subscribe to every edge seen by this channel. Callbacks fire from
  // the task context; see PulseCallback docs above.
  //
  // IMPORTANT: only call this from your component's setup() (or
  // earlier). The parent PulseInspector defers task creation and
  // interrupt enabling until its first loop() iteration so child
  // setup()s can safely mutate pulse_callbacks_. Once the task is
  // running it iterates pulse_callbacks_ without a mutex, so any
  // concurrent std::vector realloc from a late add_on_pulse_callback()
  // call is undefined behavior.
  void add_on_pulse_callback(PulseCallback &&cb) {
    this->pulse_callbacks_.add(std::move(cb));
  }

  // Subscribe to decoded packets. PulseInspector does not decode anything
  // on its own; higher-level components (decoders) call trigger_packet()
  // to dispatch to every subscriber.
  void add_on_packet_callback(PacketCallback &&cb) {
    this->packet_callbacks_.add(std::move(cb));
  }

  // Dispatch helper for decoders built on top of PulseInspector.
  void trigger_packet(const uint8_t *data, size_t len) {
    this->packet_callbacks_.call(data, len);
  }

  // Access to cached GPIO numbers (useful for child components that want
  // to know which pin they are listening on without going through the
  // ESPHome pin abstraction).
  int get_input_gpio_num() const { return this->input_gpio_num_; }
  int get_output_gpio_num() const { return this->output_gpio_num_; }
  bool has_output() const { return this->has_output_; }

  InternalGPIOPin *get_input_pin() const { return this->input_pin_; }
  InternalGPIOPin *get_output_pin() const { return this->output_pin_; }

 protected:
  // ISR trampoline registered with gpio_isr_handler_add. IRAM_ATTR is only
  // applied at the definition site to avoid duplicate `section(".iram1.N")`
  // attributes (IRAM_ATTR uses __COUNTER__, so placing it on both the
  // declaration and the definition yields different section names and a
  // `-Wattributes` warning).
  static void isr_trampoline(void *arg);

  // Actual per-channel edge handler, runs in ISR context, must stay in IRAM.
  void on_edge();

  // Background task trampoline + main loop.
  static void task_trampoline(void *arg);
  void task_loop();

  // Configuration.
  InternalGPIOPin *input_pin_{nullptr};
  InternalGPIOPin *output_pin_{nullptr};
  bool invert_in_{false};
  bool invert_out_{false};
  size_t queue_size_{256};

  // Cached raw pin numbers so the ISR can touch GPIO registers directly
  // without going through the (non IRAM-safe) HAL.
  int input_gpio_num_{-1};
  int output_gpio_num_{-1};
  bool has_output_{false};

  // Runtime primitives.
  QueueHandle_t queue_{nullptr};
  TaskHandle_t task_{nullptr};

  // Diagnostics. `volatile` because the ISR writes them.
  volatile uint32_t edge_count_{0};
  volatile uint32_t queue_overflow_count_{0};
  volatile uint32_t task_processed_count_{0};
  uint32_t last_logged_edge_count_{0};
  uint32_t last_logged_overflow_count_{0};

  // Subscribers.
  CallbackManager<void(const PulseItem &)> pulse_callbacks_;
  CallbackManager<void(const uint8_t *, size_t)> packet_callbacks_;
};

class PulseInspector : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  // Must run after pins are configured but before user logic.
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void add_channel(PulseInspectorChannel *ch) { this->channels_.push_back(ch); }

  // Read-only access for child components.
  const std::vector<PulseInspectorChannel *> &get_channels() const {
    return this->channels_;
  }

 protected:
  std::vector<PulseInspectorChannel *> channels_;
  uint32_t last_diag_log_ms_{0};
  bool started_{false};
};

}  // namespace pulse_inspector
}  // namespace esphome

#endif  // USE_ESP_IDF
