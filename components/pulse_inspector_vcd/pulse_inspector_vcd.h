#pragma once

#include "esphome/core/component.h"

#ifdef USE_ESP_IDF

#include <cstdint>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esphome/components/pulse_inspector/pulse_inspector.h"

namespace esphome {
namespace pulse_inspector_vcd {

// Single captured edge, in microseconds since boot (esp_timer time base,
// copied straight from PulseItem). 64-bit so multi-hour captures and long
// silent gaps never wrap or alias; packed to keep the ring buffer compact
// (10 bytes/event -> 40 KB for the default 4096-event buffer).
struct __attribute__((packed)) VcdEvent {
  int64_t t_us;
  uint8_t ch_idx;
  uint8_t level;
};

// Captures edges from every channel of a parent `pulse_inspector` and
// exposes them as a live VCD (Value Change Dump) stream over a plain TCP
// port. Any VCD viewer (PulseView, GTKWave, ...) can open the result as a
// multi-channel logic waveform and run protocol decoders on top of it.
//
// The component maintains two stages:
//
//  1. A small ring buffer of the most recent `buffer_size` edges. This is
//     the "pre-trigger" history — the events the VCD consumer will see as
//     soon as it connects, giving them context for what happened just
//     before they opened the capture.
//
//  2. A per-connection live queue. While a client is connected, every new
//     edge is pushed to this queue and streamed out immediately, so the
//     capture keeps growing for as long as the client stays connected.
//
// Typical workflow:
//   1. Start the capture:  `nc <device-ip> 9000 > capture.vcd`
//   2. Exercise the signal for as long as you want (minutes, hours...).
//   3. Stop with Ctrl+C — the TCP FIN closes the stream cleanly and the
//      file is ready to open in PulseView / GTKWave.
class PulseInspectorVcd : public Component {
 public:
  void set_inspector(pulse_inspector::PulseInspector *p) { inspector_ = p; }
  void set_buffer_size(size_t n) { capacity_ = n; }
  void set_port(uint16_t p) { port_ = p; }

  // Per-connection knobs. All have sensible defaults; override from YAML
  // when the local network is flaky or the client app is unusually slow.
  void set_send_timeout_sec(uint32_t v) { send_timeout_sec_ = v; }
  void set_live_queue_depth(uint32_t v) { live_queue_depth_ = v; }
  void set_app_keepalive_interval_sec(uint32_t v) { app_keepalive_interval_sec_ = v; }
  void set_tcp_keepalive_enabled(bool v) { tcp_keepalive_enabled_ = v; }
  void set_tcp_keepalive_idle_sec(uint32_t v) { tcp_keepalive_idle_sec_ = v; }
  void set_tcp_keepalive_interval_sec(uint32_t v) { tcp_keepalive_interval_sec_ = v; }
  void set_tcp_keepalive_count(uint32_t v) { tcp_keepalive_count_ = v; }

  void setup() override;
  void dump_config() override;

  // Must come up after WiFi so we can immediately bind the TCP socket.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  // Called from each channel's FreeRTOS task when a new edge is decoded.
  // Appends to the ring buffer and, if a client is currently connected,
  // also to its live streaming queue.
  void on_pulse(uint8_t ch_idx, const pulse_inspector::PulseItem &item);

  // TCP server task: accepts one client at a time, serves the capture and
  // streams until the client closes the connection.
  static void server_trampoline(void *arg);
  void server_loop();
  void serve_client_(int client_sock);

  // Configuration.
  pulse_inspector::PulseInspector *inspector_{nullptr};
  uint16_t port_{9000};
  size_t capacity_{4096};

  // Per-client TCP and streaming knobs. Defaults below are tuned for a
  // moderately reliable Wi-Fi link. With these:
  //
  //   * If the client stops draining for `send_timeout_sec_` seconds, we
  //     drop the connection (vs blocking the task forever).
  //   * If lwIP TCP keepalives are enabled, a half-open connection is
  //     detected within roughly `idle + interval * count` seconds even
  //     when the data path is idle.
  //   * Every `app_keepalive_interval_sec_` seconds without any
  //     edge-driven traffic we emit a `$comment` line over the socket so
  //     intermediate routers / NAT boxes don't drop the flow as idle.
  //     Set to 0 to disable.
  uint32_t send_timeout_sec_{30};
  uint32_t live_queue_depth_{1024};
  uint32_t app_keepalive_interval_sec_{15};
  bool tcp_keepalive_enabled_{true};
  uint32_t tcp_keepalive_idle_sec_{30};
  uint32_t tcp_keepalive_interval_sec_{10};
  uint32_t tcp_keepalive_count_{3};

  // Ring buffer with the pre-trigger history (most recent `capacity_`
  // events). Fixed-size vector allocated once in setup().
  std::vector<VcdEvent> buffer_;
  size_t head_{0};   // next write index
  size_t count_{0};  // number of valid events in buffer_ (0..capacity_)

  // Protects both the ring buffer (head_/count_/buffer_) and the
  // live_queue_ pointer swap.
  SemaphoreHandle_t mutex_{nullptr};

  // Live streaming queue attached to the currently connected client, or
  // nullptr when there is no active connection. Owned and created by the
  // server task on accept, detached and destroyed on disconnect.
  QueueHandle_t live_queue_{nullptr};

  // Diagnostic counter of events dropped because the live_queue_ was full
  // (producer-consumer imbalance — should stay at zero in normal use).
  uint32_t live_overflow_count_{0};

  // One printable ASCII symbol per channel, used as the VCD $var id.
  std::vector<char> symbols_;

  TaskHandle_t server_task_{nullptr};
};

}  // namespace pulse_inspector_vcd
}  // namespace esphome

#endif  // USE_ESP_IDF
