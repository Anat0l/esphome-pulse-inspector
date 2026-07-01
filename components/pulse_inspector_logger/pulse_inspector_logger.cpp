#include "pulse_inspector_logger.h"

#ifdef USE_ESP_IDF

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace pulse_inspector_logger {

static const char *const TAG = "pulse_inspector_logger";

void PulseInspectorLogger::setup() {
  if (this->channel_ == nullptr) {
    ESP_LOGE(TAG, "No channel configured");
    this->mark_failed();
    return;
  }

  // Subscribe to every edge. Lambda captures `this` and runs on the
  // channel's task, so it must not block or touch thread-unsafe APIs.
  this->channel_->add_on_pulse_callback(
      [this](const pulse_inspector::PulseItem &item) { this->on_pulse(item); });
}

void PulseInspectorLogger::on_pulse(const pulse_inspector::PulseItem &item) {
  this->edge_counter_.fetch_add(1, std::memory_order_relaxed);
  this->last_level_.store(item.level ? 1 : 0, std::memory_order_relaxed);
}

void PulseInspectorLogger::loop() {
  const uint32_t now = millis();
  if (now - this->last_report_ms_ < this->report_interval_ms_) {
    return;
  }
  this->last_report_ms_ = now;

  const uint32_t total = this->edge_counter_.load(std::memory_order_relaxed);
  const uint32_t delta = total - this->previous_reported_count_;
  this->previous_reported_count_ = total;

  ESP_LOGI(TAG, "GPIO%d: %u edges in last %u ms (total=%u, last_level=%u)",
           this->channel_->get_input_gpio_num(), (unsigned) delta,
           (unsigned) this->report_interval_ms_, (unsigned) total,
           (unsigned) this->last_level_.load(std::memory_order_relaxed));
}

void PulseInspectorLogger::dump_config() {
  ESP_LOGCONFIG(TAG, "pulse_inspector_logger:");
  if (this->channel_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Channel: GPIO%d", this->channel_->get_input_gpio_num());
  }
  ESP_LOGCONFIG(TAG, "  Report interval: %u ms", (unsigned) this->report_interval_ms_);
}

}  // namespace pulse_inspector_logger
}  // namespace esphome

#endif  // USE_ESP_IDF
