#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#ifdef USE_ESP_IDF

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esphome/components/pulse_inspector/pulse_inspector.h"

#include "mdb_protocol.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome {
namespace pulse_inspector_mdb {

// =============================================================================
// Full MDB (NAMA Multi-Drop Bus) decoder.
//
// Layers (bottom up):
//   1. UART  -- 9N1 @ configurable baud, edge-driven on the pulse_inspector
//               channel(s). Produces single 9-bit words (data + mode bit) for
//               each direction.
//   2. Frame -- buffers bytes into complete master commands / slave replies
//               with checksum verification. A new master command starts on a
//               mode=1 byte; the current frame ends either on the next mode=1
//               byte or after an inter-byte silence. Slave replies end purely
//               on inter-byte silence (NAMA MDB) or on the mode=1 marker if
//               the peripheral uses the "end of reply" convention.
//   3. Semantic -- interprets buffered frames against the NAMA MDB command
//               set, producing typed MdbEvent objects and/or human-readable
//               log lines. Scale factors learned from SETUP responses are
//               used to convert raw MDB units to euro-cents.
//
// Output surfaces:
//   * ESP_LOG lines at INFO level for every semantic event.
//   * Callback API add_on_event_callback() / add_on_*_callback() for plugins.
//   * Optional ESPHome sensors (numeric, binary, text) that publish state
//     derived from the event stream.
//   * Triggers for YAML automations (see automation.h).
// =============================================================================
class PulseInspectorMdb : public Component {
 public:
  enum class ChannelRole : uint8_t { AUTO, MASTER, SLAVE };

  // ------------------------------------------------------------------- UART
  struct ChannelCtx {
    int channel_idx{-1};
    ChannelRole role{ChannelRole::AUTO};

    enum class State : uint8_t { IDLE, FRAME } state{State::IDLE};
    uint32_t frame_start_us{0};
    uint32_t last_edge_us{0};
    bool last_level{true};
    uint8_t bits_filled{0};
    uint16_t frame_value{0};
    bool stop_bit_seen{true};

    esp_timer_handle_t timeout_timer{nullptr};

    // Set by safety_timeout_cb_ (runs in esp_timer_task with a small
    // stack) to hand the actual finalize work off to the main loop,
    // which has a much larger stack for snprintf / callbacks.
    volatile bool timeout_pending{false};
  };

  struct TimeoutArg {
    PulseInspectorMdb *self{nullptr};
    ChannelCtx *ctx{nullptr};
  };

  // ------------------------------------------------------------------ Frame
  struct FrameBuf {
    static constexpr size_t CAPACITY = 40;
    uint8_t buf[CAPACITY]{};
    uint16_t mode[CAPACITY]{};   // mirror of the 9th bit for each byte
    size_t len{0};
    uint32_t first_byte_us{0};
    uint32_t last_byte_us{0};
    bool active{false};
  };

  // ---------------------------------------------------------------- Callbacks
  using EventCallback = std::function<void(const MdbEvent &)>;

  void set_inspector(pulse_inspector::PulseInspector *p) { inspector_ = p; }
  void set_single_channel_index(int idx) { single_channel_idx_ = idx; }
  void set_master_channel_index(int idx) { master_channel_idx_ = idx; }
  void set_slave_channel_index(int idx) { slave_channel_idx_ = idx; }
  void set_baud(uint32_t b) { baud_ = b; }
  void set_log_slave_ack(bool v) { log_slave_ack_ = v; }
  void set_log_raw_frames(bool v) { log_raw_frames_ = v; }
  void set_suppress_idle_polls(bool v) { suppress_idle_polls_ = v; }

  // Register a user-provided mapping item_number -> human name. Persists
  // for the lifetime of the component.
  void add_selection_name(uint32_t item, std::string name) {
    this->selections_.push_back({item, std::move(name)});
  }

  // Subscribe to every decoded MdbEvent. Called on the ESPHome main loop
  // via defer() (not from the UART task), safe to touch other components.
  void add_on_event_callback(EventCallback &&cb) {
    this->event_callbacks_.add(std::move(cb));
  }

#ifdef USE_SENSOR
  void set_last_item_sensor(sensor::Sensor *s) { this->sens_last_item_ = s; }
  void set_last_price_sensor(sensor::Sensor *s) { this->sens_last_price_ = s; }
  void set_session_funds_sensor(sensor::Sensor *s) { this->sens_session_funds_ = s; }
  void set_vend_success_count_sensor(sensor::Sensor *s) { this->sens_vend_success_ = s; }
  void set_vend_failure_count_sensor(sensor::Sensor *s) { this->sens_vend_failure_ = s; }
  void set_frames_decoded_sensor(sensor::Sensor *s) { this->sens_frames_decoded_ = s; }
  void set_framing_errors_sensor(sensor::Sensor *s) { this->sens_framing_errors_ = s; }
  // BV-centric counters (the ones that actually fire on a coin-mech <-> BV tap).
  void set_bills_stacked_sensor(sensor::Sensor *s) { this->sens_bills_stacked_ = s; }
  void set_bills_escrowed_sensor(sensor::Sensor *s) { this->sens_bills_escrowed_ = s; }
  void set_bills_returned_sensor(sensor::Sensor *s) { this->sens_bills_returned_ = s; }
  void set_bills_rejected_sensor(sensor::Sensor *s) { this->sens_bills_rejected_ = s; }
  void set_last_bill_type_sensor(sensor::Sensor *s) { this->sens_last_bill_type_ = s; }
  // Derived sale-cycle sensors (BV BILL TYPE enable-mask transitions).
  void set_sale_cycles_total_sensor(sensor::Sensor *s) { this->sens_sale_cycles_total_ = s; }
  void set_last_sale_duration_sensor(sensor::Sensor *s) { this->sens_last_sale_duration_ = s; }
  // Low-level diagnostic counters.
  void set_master_bytes_sensor(sensor::Sensor *s) { this->sens_master_bytes_ = s; }
  void set_slave_bytes_sensor(sensor::Sensor *s) { this->sens_slave_bytes_ = s; }
  void set_events_dropped_sensor(sensor::Sensor *s) { this->sens_events_dropped_ = s; }
  void set_proprietary_frames_sensor(sensor::Sensor *s) { this->sens_proprietary_frames_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_bv_jam_bs(binary_sensor::BinarySensor *s) { this->bs_bv_jam_ = s; }
  void set_bv_disabled_bs(binary_sensor::BinarySensor *s) { this->bs_bv_disabled_ = s; }
  void set_changer_jam_bs(binary_sensor::BinarySensor *s) { this->bs_changer_jam_ = s; }
  void set_cashless_malfunction_bs(binary_sensor::BinarySensor *s) { this->bs_cl_malfunction_ = s; }
  void set_session_active_bs(binary_sensor::BinarySensor *s) { this->bs_session_active_ = s; }
  void set_vend_in_progress_bs(binary_sensor::BinarySensor *s) { this->bs_vend_in_progress_ = s; }
  void set_sale_cycle_in_progress_bs(binary_sensor::BinarySensor *s) {
    this->bs_sale_cycle_in_progress_ = s;
  }
#endif
#ifdef USE_TEXT_SENSOR
  void set_last_event_ts(text_sensor::TextSensor *s) { this->ts_last_event_ = s; }
  void set_last_selection_name_ts(text_sensor::TextSensor *s) { this->ts_last_selection_name_ = s; }
  void set_last_bv_error_ts(text_sensor::TextSensor *s) { this->ts_last_bv_error_ = s; }
  void set_last_changer_error_ts(text_sensor::TextSensor *s) { this->ts_last_changer_error_ = s; }
  void set_last_cashless_error_ts(text_sensor::TextSensor *s) { this->ts_last_cl_error_ = s; }
  void set_bv_enable_mask_ts(text_sensor::TextSensor *s) { this->ts_bv_enable_mask_ = s; }
#endif

  void setup() override;
  void dump_config() override;
  void loop() override;

  float get_setup_priority() const override { return setup_priority::HARDWARE - 1.0f; }

 protected:
  // ---------------- UART layer -----------------------------------------
  void on_pulse(ChannelCtx *ctx, const pulse_inspector::PulseItem &item);
  void on_edge_locked_(ChannelCtx *ctx, uint32_t t_us, bool level);
  void fill_bits_until_(ChannelCtx *ctx, int upper);
  void finalize_frame_locked_(ChannelCtx *ctx);
  bool configure_channel_(ChannelCtx *ctx, int channel_idx, ChannelRole role,
                          const char *timer_name);
  static void safety_timeout_cb_(void *arg);

  // Called from the UART layer with a fully decoded 9-bit word. Dispatches
  // into the frame-buffering layer. MUST be called with mutex_ held.
  void emit_byte_locked_(ChannelCtx *ctx, uint16_t value, bool stop_ok);

  // ---------------- Frame layer ----------------------------------------
  // Push one byte into the master/slave buffer. On boundary conditions
  // (mode=1 arrives, inter-byte silence, buffer full) the completed frame
  // is dispatched to the semantic layer. Called with mutex_ held.
  void push_master_byte_(uint8_t data, bool mode, uint32_t now_us);
  void push_slave_byte_(uint8_t data, bool mode, uint32_t now_us);
  void flush_master_locked_(uint32_t now_us);
  void flush_slave_locked_(uint32_t now_us);

  // ---------------- Semantic layer -------------------------------------
  void decode_master_frame_(const FrameBuf &f);
  void decode_slave_frame_(const FrameBuf &f);
  void decode_cashless_master_(const FrameBuf &f);
  void decode_cashless_slave_(const FrameBuf &f);
  void decode_bv_master_(const FrameBuf &f);
  void decode_bv_slave_(const FrameBuf &f);
  void decode_changer_master_(const FrameBuf &f);
  void decode_changer_slave_(const FrameBuf &f);

  // Verify the MDB checksum (sum of all bytes but last == last). Returns
  // true if the frame looks well-formed WITH a CHK byte; `chk_ok` is set
  // to the verification result, and `payload_len` to the length of the
  // frame excluding the CHK byte if present. For very short frames
  // (single-byte ACK/NAK on slave side, or Necta-style "no CHK" commands)
  // we treat the whole buffer as payload.
  static bool split_chk_(const FrameBuf &f, bool *chk_ok, size_t *payload_len);

  // Classify a master frame as an "idle poll / discovery reset" that can
  // be hidden from the raw hex dump when `suppress_idle_polls_` is on.
  // Returns true for 2-byte POLL commands on every peripheral class, and
  // for 2-byte RESET commands on peripherals that are typically absent
  // in the field (Cashless, Display, Energy Management, Comms Gateway).
  // Returns false for anything else (including BV / Changer RESET, which
  // is usually meaningful, and any frame with a bad checksum or unusual
  // length).
  static bool is_idle_poll_frame_(const FrameBuf &f);

  // Convert a raw MDB price (2 bytes, big endian) to cents using the
  // cashless scale factor.
  uint32_t cashless_price_to_cents_(uint16_t raw) const;

  // Emit an event: log, fan out to callbacks, publish to sensors, push
  // into the last-event text sensor. MUST be called on the main loop.
  void publish_event_(const MdbEvent &evt);

  // Enqueue an event for main-loop consumption. Safe to call from the
  // pulse_inspector task (under mutex_) and from the esp_timer task.
  // Drops the event if the queue is full (we'd rather lose a log line
  // than block the UART task and corrupt the decoder's real-time
  // budget).
  void enqueue_event_(const MdbEvent &evt);

  // Drain the event queue; called from the main ESPHome loop().
  void drain_event_queue_();

  // Log a human-readable line for an event.
  static std::string format_event_log_(const MdbEvent &evt);

  // Lookup a selection item number -> name (as configured via
  // selection_map). Returns nullptr if no mapping exists.
  const char *lookup_selection_name_(uint32_t item) const;

  // ===================================================================
  // Configuration.
  pulse_inspector::PulseInspector *inspector_{nullptr};
  int single_channel_idx_{-1};
  int master_channel_idx_{-1};
  int slave_channel_idx_{-1};
  uint32_t baud_{9600};
  bool log_slave_ack_{false};
  bool log_raw_frames_{false};
  bool suppress_idle_polls_{false};
  float bit_us_{104.166667f};
  uint32_t inter_byte_timeout_us_{1000};  // ~10 bit times at 9600

  struct Selection {
    uint32_t item;
    std::string name;
  };
  std::vector<Selection> selections_;

  // UART state (up to two channels).
  ChannelCtx ch_[2]{};
  TimeoutArg timeout_args_[2]{};
  uint8_t num_channels_{0};

  // Single shared mutex protects UART state, frame buffers, scale, and
  // transaction context. Contention between the two channel tasks and
  // the esp_timer tasks is minimal (1 byte every ~1 ms at 9600 baud).
  SemaphoreHandle_t mutex_{nullptr};

  // Event queue: filled from the UART/timer context, drained from
  // loop(). This is what makes sensor publishing and trigger dispatch
  // safe -- they only ever run on the main thread.
  QueueHandle_t event_queue_{nullptr};
  uint32_t events_dropped_{0};

  // Frame buffers (two-wire: master wire and slave wire; single-wire:
  // we route by the 9th bit).
  FrameBuf master_frame_;
  FrameBuf slave_frame_;

  // Scale factors learned from SETUP responses.
  MdbScaleFactors scale_{};

  // Transaction context.
  uint16_t last_master_cmd_{0xFFFF};   // first byte of last master frame
  uint32_t last_master_us_{0};
  // NOTE: full last-master payload is intentionally NOT stored. If a
  // future decoder needs it, use a fixed array like the FrameBuf above
  // (std::vector under mutex on the hot path is avoided on purpose).

  // Last-byte dedup so the log doesn't get spammed with identical POLLs.
  uint16_t last_logged_master_{0xFFFF};

  // Tracks whether the most recent master frame was classified as an
  // "idle poll" by is_idle_poll_frame_(). Used by the slave decoder to
  // suppress the raw dump of the matching single-byte ACK reply when
  // `suppress_idle_polls_` is on. Paired with a timestamp so that an
  // unrelated ACK arriving much later never gets incorrectly silenced.
  bool last_master_was_idle_poll_{false};
  uint32_t last_idle_poll_us_{0};
  uint8_t steady_state_[256]{};
  bool steady_state_known_[256]{};

  // State used to feed binary sensors / stateful fields.
  bool bv_jam_state_{false};
  bool bv_disabled_state_{false};
  bool changer_jam_state_{false};
  bool cl_malfunction_state_{false};
  bool session_active_{false};
  bool vend_in_progress_{false};

  // ---- Sale-cycle tracking (derived from BV BILL TYPE command) -------
  // On this bus the coin mechanism acts as the MDB host: right before
  // the coffee machine starts preparing a drink the host sends
  //   34 00 00 00 00 CHK  (BILL TYPE with enable_mask == 0)
  // which disables bill acceptance for the duration of the sale, and
  // then restores the mask (e.g. 00 01 00 01) once the machine is ready
  // again. That zero/non-zero transition is a reliable proxy for
  // "drink is being prepared" even though we cannot see the VEND
  // commands themselves (those go over the Executive bus).
  uint32_t bv_enable_mask_{0};       // last observed (enable<<16)|escrow
  bool bv_enable_mask_known_{false};
  bool sale_cycle_in_progress_{false};
  uint32_t sale_cycle_start_us_{0};
  uint32_t last_sale_duration_ms_{0};
  uint32_t sale_cycles_total_{0};

  // Per-routing BV bill counters. Incremented in publish_event_() based
  // on (byte_code >> 4) & 0x07:
  //   routing 0 -> escrowed (waiting for STACK/RETURN command)
  //   routing 1 -> stacked  (captured, money is in)
  //   routing 2 -> returned (given back to the user)
  //   routing 3 -> rejected (BV_BILL_REJECTED kind)
  uint32_t bv_bills_stacked_{0};
  uint32_t bv_bills_escrowed_{0};
  uint32_t bv_bills_returned_{0};
  uint32_t bv_bills_rejected_{0};
  uint8_t last_bill_type_{0};

  // Counters.
  uint32_t frames_decoded_{0};
  uint32_t framing_errors_{0};
  uint32_t master_bytes_{0};
  uint32_t slave_bytes_{0};
  uint32_t events_logged_{0};
  uint32_t vend_success_count_{0};
  uint32_t vend_failure_count_{0};
  // Master frames hidden because they are manufacturer-proprietary
  // chatter (non-NAMA base address, or an orphan mode=1 byte from an
  // MEI-style multi-mode-1-byte sequence). Exposed as a sensor so the
  // rate of this traffic is still visible without log spam.
  uint32_t proprietary_master_frames_{0};
  uint32_t last_diag_log_ms_{0};
  uint32_t prev_frames_decoded_{0};

  // User-visible surfaces.
  CallbackManager<void(const MdbEvent &)> event_callbacks_;

#ifdef USE_SENSOR
  sensor::Sensor *sens_last_item_{nullptr};
  sensor::Sensor *sens_last_price_{nullptr};
  sensor::Sensor *sens_session_funds_{nullptr};
  sensor::Sensor *sens_vend_success_{nullptr};
  sensor::Sensor *sens_vend_failure_{nullptr};
  sensor::Sensor *sens_frames_decoded_{nullptr};
  sensor::Sensor *sens_framing_errors_{nullptr};
  sensor::Sensor *sens_bills_stacked_{nullptr};
  sensor::Sensor *sens_bills_escrowed_{nullptr};
  sensor::Sensor *sens_bills_returned_{nullptr};
  sensor::Sensor *sens_bills_rejected_{nullptr};
  sensor::Sensor *sens_last_bill_type_{nullptr};
  sensor::Sensor *sens_sale_cycles_total_{nullptr};
  sensor::Sensor *sens_last_sale_duration_{nullptr};
  sensor::Sensor *sens_master_bytes_{nullptr};
  sensor::Sensor *sens_slave_bytes_{nullptr};
  sensor::Sensor *sens_events_dropped_{nullptr};
  sensor::Sensor *sens_proprietary_frames_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *bs_bv_jam_{nullptr};
  binary_sensor::BinarySensor *bs_bv_disabled_{nullptr};
  binary_sensor::BinarySensor *bs_changer_jam_{nullptr};
  binary_sensor::BinarySensor *bs_cl_malfunction_{nullptr};
  binary_sensor::BinarySensor *bs_session_active_{nullptr};
  binary_sensor::BinarySensor *bs_vend_in_progress_{nullptr};
  binary_sensor::BinarySensor *bs_sale_cycle_in_progress_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *ts_last_event_{nullptr};
  text_sensor::TextSensor *ts_last_selection_name_{nullptr};
  text_sensor::TextSensor *ts_last_bv_error_{nullptr};
  text_sensor::TextSensor *ts_last_changer_error_{nullptr};
  text_sensor::TextSensor *ts_last_cl_error_{nullptr};
  text_sensor::TextSensor *ts_bv_enable_mask_{nullptr};
#endif
};

}  // namespace pulse_inspector_mdb
}  // namespace esphome

#endif  // USE_ESP_IDF
