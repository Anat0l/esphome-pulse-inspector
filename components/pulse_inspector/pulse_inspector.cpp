#include "pulse_inspector.h"

#ifdef USE_ESP_IDF

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "soc/gpio_reg.h"
#include "soc/soc_caps.h"

#ifdef __XTENSA__
#include "xtensa/hal.h"
#endif

namespace esphome {
namespace pulse_inspector {

static const char *const TAG = "pulse_inspector";

// Stack depth and priority for the per-channel task. The task reads
// PulseItems from the queue and dispatches them to user callbacks. Keep
// the stack generous: downstream components (e.g. pulse_inspector_mdb)
// can do snprintf, FreeRTOS queue operations, and other moderately
// stack-hungry work inside the callback, and we want a comfortable margin
// to survive nested compiler inlining + libc working buffers.
static constexpr uint32_t TASK_STACK_WORDS = 4096;
static constexpr UBaseType_t TASK_PRIORITY = 1;

// How often we dump diagnostics to the log (ms).
static constexpr uint32_t DIAG_LOG_INTERVAL_MS = 5000;

// IRAM-safe GPIO helpers. Touch GPIO registers directly via the chip-agnostic
// REG_READ / REG_WRITE macros to avoid calling non-IRAM functions from the
// ISR (flash loads would crash us). Using the register macros instead of the
// `GPIO` struct keeps us portable across ESP32 variants whose gpio_dev_s
// layouts differ (e.g. ESP32-C3/S3 wrap the fields in anonymous unions, and
// chips with ≤32 GPIOs have no second bank at all).
static inline int IRAM_ATTR gpio_get_level_iram(int gpio_num) {
  if (gpio_num < 32) {
    return (REG_READ(GPIO_IN_REG) >> gpio_num) & 0x1;
  }
#if SOC_GPIO_PIN_COUNT > 32
  return (REG_READ(GPIO_IN1_REG) >> (gpio_num - 32)) & 0x1;
#else
  return 0;
#endif
}

static inline void IRAM_ATTR gpio_set_level_iram(int gpio_num, int level) {
  if (gpio_num < 32) {
    if (level) {
      REG_WRITE(GPIO_OUT_W1TS_REG, (uint32_t) 1 << gpio_num);
    } else {
      REG_WRITE(GPIO_OUT_W1TC_REG, (uint32_t) 1 << gpio_num);
    }
    return;
  }
#if SOC_GPIO_PIN_COUNT > 32
  if (level) {
    REG_WRITE(GPIO_OUT1_W1TS_REG, (uint32_t) 1 << (gpio_num - 32));
  } else {
    REG_WRITE(GPIO_OUT1_W1TC_REG, (uint32_t) 1 << (gpio_num - 32));
  }
#endif
}

// ---------------------------------------------------------------------------
// PulseInspectorChannel
// ---------------------------------------------------------------------------

bool PulseInspectorChannel::setup() {
  if (this->input_pin_ == nullptr) {
    ESP_LOGE(TAG, "Channel has no input_pin configured");
    return false;
  }

  this->input_gpio_num_ = this->input_pin_->get_pin();
  this->has_output_ = this->output_pin_ != nullptr;
  if (this->has_output_) {
    this->output_gpio_num_ = this->output_pin_->get_pin();
  }

  // Configure input pin: ANY-edge interrupt, pull-up.
  gpio_num_t in_num = (gpio_num_t) this->input_gpio_num_;
  gpio_set_direction(in_num, GPIO_MODE_INPUT);
  gpio_set_pull_mode(in_num, GPIO_PULLUP_ONLY);
  gpio_set_intr_type(in_num, GPIO_INTR_ANYEDGE);

  // Configure output pin (if any) and pre-mirror current input level so we
  // start in a sensible state.
  if (this->has_output_) {
    this->output_pin_->setup();
    this->output_pin_->pin_mode(gpio::FLAG_OUTPUT);
    bool cur_level = (bool) gpio_get_level_iram(this->input_gpio_num_) ^ this->invert_in_;
    gpio_set_level_iram(this->output_gpio_num_, cur_level ^ this->invert_out_);
  }

  // Create the pulse queue.
  this->queue_ = xQueueCreate(this->queue_size_, sizeof(PulseItem));
  if (this->queue_ == nullptr) {
    ESP_LOGE(TAG, "GPIO%d: failed to create pulse queue (size=%u)",
             this->input_gpio_num_, (unsigned) this->queue_size_);
    return false;
  }

  // Install the ISR handler but DO NOT enable the interrupt yet, and
  // DO NOT start the processing task. Both are deferred to
  // start_processing(), which is called once all child components have
  // registered their pulse callbacks. Otherwise add_on_pulse_callback()
  // from child setup() races with task_loop()'s iteration of the same
  // CallbackManager (std::vector) on this task, which can corrupt the
  // vector and crash via illegal instruction on the next tick.
  esp_err_t err = gpio_isr_handler_add(in_num, &PulseInspectorChannel::isr_trampoline, this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "GPIO%d: gpio_isr_handler_add failed (err=%d)", this->input_gpio_num_, (int) err);
    return false;
  }

  ESP_LOGCONFIG(TAG, "  Channel GPIO%d -> %s armed (queue=%u, interrupt disabled until loop starts)",
                this->input_gpio_num_,
                this->has_output_ ? (std::string("GPIO") + std::to_string(this->output_gpio_num_)).c_str() : "(none)",
                (unsigned) this->queue_size_);
  return true;
}

bool PulseInspectorChannel::start_processing() {
  if (this->task_ != nullptr) {
    return true;  // already started
  }
  if (this->queue_ == nullptr) {
    return false;  // setup failed earlier
  }

  BaseType_t task_ret = xTaskCreate(&PulseInspectorChannel::task_trampoline, "pi_ch",
                                    TASK_STACK_WORDS, this, TASK_PRIORITY, &this->task_);
  if (task_ret != pdPASS) {
    ESP_LOGE(TAG, "GPIO%d: failed to create task (err=%d)", this->input_gpio_num_, (int) task_ret);
    return false;
  }

  // Now (and only now) enable the edge interrupt. From this point on,
  // pulse_callbacks_ must not be modified anymore -- it can only be
  // read by task_loop().
  gpio_intr_enable((gpio_num_t) this->input_gpio_num_);
  ESP_LOGI(TAG, "GPIO%d: processing started, interrupt enabled", this->input_gpio_num_);
  return true;
}

void IRAM_ATTR PulseInspectorChannel::isr_trampoline(void *arg) {
  static_cast<PulseInspectorChannel *>(arg)->on_edge();
}

void IRAM_ATTR PulseInspectorChannel::on_edge() {
  BaseType_t higher_priority_task_woken = pdFALSE;

  PulseItem item;
#ifdef __XTENSA__
  item.cycle = xthal_get_ccount();
#else
  item.cycle = (uint32_t) esp_timer_get_time();
#endif
  item.level = (bool) gpio_get_level_iram(this->input_gpio_num_) ^ this->invert_in_;

  // Mirror to the output pin as fast as possible - this is the whole point
  // of the transparent pass-through mode.
  if (this->has_output_) {
    gpio_set_level_iram(this->output_gpio_num_, item.level ^ this->invert_out_);
  }

  this->edge_count_++;

  BaseType_t ret = xQueueSendToBackFromISR(this->queue_, &item, &higher_priority_task_woken);
  if (ret != pdTRUE) {
    this->queue_overflow_count_++;
  }

  if (higher_priority_task_woken) {
    portYIELD_FROM_ISR();
  }
}

void PulseInspectorChannel::task_trampoline(void *arg) {
  static_cast<PulseInspectorChannel *>(arg)->task_loop();
}

void PulseInspectorChannel::task_loop() {
  PulseItem item;
  for (;;) {
    BaseType_t ret = xQueueReceive(this->queue_, &item, portMAX_DELAY);
    if (ret != pdTRUE) {
      continue;
    }

    this->task_processed_count_++;

    // Dispatch to child components. Callbacks run in this task context;
    // if they need to touch thread-unsafe ESPHome APIs (sensor state,
    // network, etc.) they should defer the work via Component::defer()
    // or App.scheduler to the main loop.
    this->pulse_callbacks_.call(item);
  }
}

void PulseInspectorChannel::dump_config(size_t index) const {
  ESP_LOGCONFIG(TAG, "  Channel %u:", (unsigned) index);
  ESP_LOGCONFIG(TAG, "    Input pin: GPIO%d (invert=%s)", this->input_gpio_num_,
                YESNO(this->invert_in_));
  if (this->has_output_) {
    ESP_LOGCONFIG(TAG, "    Output pin: GPIO%d (invert=%s)", this->output_gpio_num_,
                  YESNO(this->invert_out_));
  } else {
    ESP_LOGCONFIG(TAG, "    Output pin: (none, receive-only)");
  }
  ESP_LOGCONFIG(TAG, "    Queue size: %u", (unsigned) this->queue_size_);
}

void PulseInspectorChannel::log_diagnostics(size_t index) {
  uint32_t edges = this->edge_count_;
  uint32_t overflows = this->queue_overflow_count_;
  uint32_t delta_edges = edges - this->last_logged_edge_count_;
  uint32_t delta_overflows = overflows - this->last_logged_overflow_count_;

  ESP_LOGD(TAG,
           "ch%u GPIO%d: edges=%u (+%u) overflows=%u (+%u) processed=%u",
           (unsigned) index, this->input_gpio_num_, (unsigned) edges, (unsigned) delta_edges,
           (unsigned) overflows, (unsigned) delta_overflows,
           (unsigned) this->task_processed_count_);

  this->last_logged_edge_count_ = edges;
  this->last_logged_overflow_count_ = overflows;
}

// ---------------------------------------------------------------------------
// PulseInspector (parent)
// ---------------------------------------------------------------------------

void PulseInspector::setup() {
  ESP_LOGCONFIG(TAG, "Setting up pulse_inspector with %u channel(s)...",
                (unsigned) this->channels_.size());

  // Install the global GPIO ISR service exactly once. ESP_ERR_INVALID_STATE
  // means it has already been installed by someone else, which is fine.
  esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "gpio_install_isr_service failed (err=%d)", (int) err);
    this->mark_failed();
    return;
  }

  for (auto *ch : this->channels_) {
    if (!ch->setup()) {
      this->mark_failed();
      return;
    }
  }
}

void PulseInspector::loop() {
  // On the very first loop iteration, start each channel's processing
  // task and enable its interrupt. By now every other component has
  // finished its setup() (ESPHome guarantees setup() completes for all
  // components before any loop() is called), so pulse_callbacks_ is
  // fully populated and safe to read from the task without a mutex.
  if (!this->started_) {
    this->started_ = true;
    for (auto *ch : this->channels_) {
      ch->start_processing();
    }
  }

  const uint32_t now = millis();
  if (now - this->last_diag_log_ms_ < DIAG_LOG_INTERVAL_MS) {
    return;
  }
  this->last_diag_log_ms_ = now;

  for (size_t i = 0; i < this->channels_.size(); i++) {
    this->channels_[i]->log_diagnostics(i);
  }
}

void PulseInspector::dump_config() {
  ESP_LOGCONFIG(TAG, "pulse_inspector:");
  ESP_LOGCONFIG(TAG, "  Channels: %u", (unsigned) this->channels_.size());
  for (size_t i = 0; i < this->channels_.size(); i++) {
    this->channels_[i]->dump_config(i);
  }
}

}  // namespace pulse_inspector
}  // namespace esphome

#endif  // USE_ESP_IDF
