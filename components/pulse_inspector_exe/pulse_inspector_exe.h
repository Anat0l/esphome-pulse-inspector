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

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

namespace esphome {
namespace pulse_inspector_exe {

// =============================================================================
// Live decoder for the Necta "Executive" line. Wire format per the MEI
// Protocol A specification, cross-verified against real captures with
// tools/vcd_analyze_uart.py:
//   * 9600 baud, 8E1 (1 start + 8 data + 1 even-parity + 1 stop),
//     LSB-first. Logic 0 == current in loop, logic 1 == no current.
// Physically the frame is 11 bit-times -- identical to MDB's 9N1 -- so
// we use the same edge-driven UART core as pulse_inspector_mdb. The
// only difference is that the 9th bit here is a deterministic parity
// bit, not a freely-set mode/address marker; we verify it and expose
// a separate parity-error counter.
//
// Higher-level semantics (what the byte values mean) depend on the VMC
// manufacturer. This component implements three layers:
//
//   1. UART layer  -- edge-driven decoder shared with pulse_inspector_mdb
//                     (11 bit-times per byte); 9th bit verified as even
//                     parity (8E1).
//   2. Frame layer -- groups bytes by inter-byte silence into master/slave
//                     frames; idle STATUS/CREDIT polls can be suppressed.
//   3. Semantic layer -- credit blocks, vend outcomes, VMC status flags;
//                     YAML triggers (on_vend_complete, on_credit_change, …)
//                     and HA sensors/binary_sensors/text_sensors.
//
// Raw frames remain available via on_master_frame / on_slave_frame for
// vendor-specific automations not covered by the typed events.
// =============================================================================

struct ExeFrameEvent {
  // Logical role on this side of the wire ("master" if it came from the
  // upstream device that initiates polls, "slave" otherwise). We use the
  // configured channel role (which is just the YAML naming choice -- we
  // don't try to detect master/slave from traffic).
  enum class Role : uint8_t { MASTER, SLAVE };
  Role role{Role::MASTER};
  // Wire timestamp of the LAST byte in this frame (us, esp_timer_get_time
  // domain).
  uint32_t end_us{0};
  // Frame contents. Mirrors FrameBuf.
  static constexpr size_t CAPACITY = 32;
  uint8_t data[CAPACITY]{};
  uint8_t mode[CAPACITY]{};
  uint8_t len{0};
  // Was this frame matched against an idle pattern (and therefore
  // suppressed in the log)?
  bool suppressed{false};
};


// Semantic-layer event dispatched to YAML triggers. We keep the payload
// to the few fields the user actually cares about; the underlying byte
// stream is still available via on_master_frame / on_slave_frame.

struct ExeVendEvent {
  bool ok{false};                   // true = Vend OK, false = Vend Failed
  uint32_t value_real_money{0};     // delta credit consumed by this vend
  uint16_t base_units{0};           // raw base-units delta
  uint8_t scaling_factor{0};        // most-recent scaling factor seen
  uint8_t decimal_places{0};        // most-recent decimal-place mask (count)
  uint8_t selection_price{0};       // last 0x32 reply != 0xFE before vend
  uint8_t audit_pairs_pending{0};   // bits 6..0 of the VEND reply
};

struct ExeCreditEvent {
  uint32_t credit_real_money{0};   // base_units * scaling
  uint16_t base_units{0};
  uint8_t scaling_factor{0};
  uint8_t decimal_places{0};
  int32_t delta_real_money{0};     // signed change vs previous block
  bool exact_change_only{false};   // bit 0 of nibble #7 in ACCEPT DATA
};

struct ExeStatusEvent {
  uint8_t status_raw{0};
  bool free_vend_request{false};
  bool vending_inhibited{false};
  bool bdv_variant{false};         // bit 5: 0=standard VMC, 1=BDV001
  uint8_t audit_pairs_pending{0};  // bits 4..0
  uint8_t prev_status_raw{0};
};


class PulseInspectorExe : public Component {
 public:
  // ChannelRole follows the MEI Protocol A terminology: MASTER is the
  // executive (the node sourcing 0x31 STATUS / 0x32 CREDIT polls and
  // controlling the link), SLAVE is the peripheral (VMC, ASU, CPP,
  // ...). The YAML configuration calls these "executive_channel"
  // and "peripheral_channel" to avoid the persistent confusion with
  // GPIO-side TX/RX names used in the parent pulse_inspector.
  enum class ChannelRole : uint8_t { MASTER, SLAVE };

  // Treatment of the 9th bit of every byte. The MEI/Necta default
  // is EVEN (verified empirically). MARK / SPACE force the bit to a
  // constant; NONE skips the check entirely (useful if you want to
  // tap MDB chatter through this component for whatever reason).
  enum class Parity : uint8_t { NONE, EVEN, ODD, MARK, SPACE };

  // ------------------------------------------------------------------- UART
  struct ChannelCtx {
    int channel_idx{-1};
    ChannelRole role{ChannelRole::MASTER};

    enum class State : uint8_t { IDLE, FRAME } state{State::IDLE};
    uint32_t frame_start_us{0};
    uint32_t last_edge_us{0};
    bool last_level{true};
    uint8_t bits_filled{0};
    uint16_t frame_value{0};
    bool stop_bit_seen{true};

    esp_timer_handle_t timeout_timer{nullptr};
    // Set by safety_timeout_cb_ (esp_timer_task, small stack); the actual
    // finalize work is dispatched to loop().
    volatile bool timeout_pending{false};
  };

  struct TimeoutArg {
    PulseInspectorExe *self{nullptr};
    ChannelCtx *ctx{nullptr};
  };

  // ------------------------------------------------------------------ Frame
  struct FrameBuf {
    static constexpr size_t CAPACITY = 32;
    uint8_t buf[CAPACITY]{};
    uint8_t mode[CAPACITY]{};
    size_t len{0};
    uint32_t first_byte_us{0};
    uint32_t last_byte_us{0};
    bool active{false};
  };

  // ---------------------------------------------------------------- Callbacks
  using FrameCallback = std::function<void(const ExeFrameEvent &)>;
  using VendCallback = std::function<void(const ExeVendEvent &)>;
  using CreditCallback = std::function<void(const ExeCreditEvent &)>;
  using StatusCallback = std::function<void(const ExeStatusEvent &)>;

  void set_inspector(pulse_inspector::PulseInspector *p) { inspector_ = p; }
  void set_master_channel_index(int idx) { master_channel_idx_ = idx; }
  void set_slave_channel_index(int idx) { slave_channel_idx_ = idx; }
  void set_baud(uint32_t b) { baud_ = b; }
  void set_parity(Parity p) { parity_ = p; }
  void set_inter_byte_timeout_us(uint32_t v) { inter_byte_timeout_us_ = v; }
  void set_log_raw_frames(bool v) { log_raw_frames_ = v; }
  void set_suppress_idle_polls(bool v) { suppress_idle_polls_ = v; }
  void set_show_parity_in_hex(bool v) { show_parity_in_hex_ = v; }
  // Watchdog: how long without a successful VMC reply before
  // `vmc_online` flips to false. 0 disables the watchdog entirely.
  void set_vmc_online_timeout_ms(uint32_t v) { vmc_online_timeout_ms_ = v; }

  // Add a master/slave byte pattern that, when matched exactly against a
  // completed frame, marks it as an "idle poll" -- the frame is still
  // delivered to YAML triggers and counted, but is hidden from the log
  // and from the `last_*_frame` text sensors when suppress_idle_polls is
  // on. Mode-bits are NOT compared (only byte values), to keep the YAML
  // ergonomic.
  void add_idle_master_pattern(std::vector<uint8_t> pat) {
    idle_master_patterns_.push_back(std::move(pat));
  }
  void add_idle_slave_pattern(std::vector<uint8_t> pat) {
    idle_slave_patterns_.push_back(std::move(pat));
  }

  void add_on_master_frame_callback(FrameCallback &&cb) {
    master_frame_callbacks_.add(std::move(cb));
  }
  void add_on_slave_frame_callback(FrameCallback &&cb) {
    slave_frame_callbacks_.add(std::move(cb));
  }
  void add_on_vend_complete_callback(VendCallback &&cb) {
    vend_complete_callbacks_.add(std::move(cb));
  }
  void add_on_credit_change_callback(CreditCallback &&cb) {
    credit_change_callbacks_.add(std::move(cb));
  }
  void add_on_vmc_status_callback(StatusCallback &&cb) {
    status_callbacks_.add(std::move(cb));
  }

#ifdef USE_SENSOR
  void set_frames_decoded_sensor(sensor::Sensor *s) { sens_frames_decoded_ = s; }
  void set_framing_errors_sensor(sensor::Sensor *s) { sens_framing_errors_ = s; }
  void set_master_bytes_sensor(sensor::Sensor *s) { sens_master_bytes_ = s; }
  void set_slave_bytes_sensor(sensor::Sensor *s) { sens_slave_bytes_ = s; }
  void set_master_frames_sensor(sensor::Sensor *s) { sens_master_frames_ = s; }
  void set_slave_frames_sensor(sensor::Sensor *s) { sens_slave_frames_ = s; }
  void set_idle_polls_sensor(sensor::Sensor *s) { sens_idle_polls_ = s; }
  void set_events_dropped_sensor(sensor::Sensor *s) { sens_events_dropped_ = s; }
  void set_parity_errors_sensor(sensor::Sensor *s) { sens_parity_errors_ = s; }

  // ---- Semantic numeric sensors -------------------------------------
  void set_current_credit_sensor(sensor::Sensor *s) { sens_current_credit_ = s; }
  void set_current_credit_base_units_sensor(sensor::Sensor *s) {
    sens_current_credit_base_units_ = s;
  }
  void set_current_selection_price_sensor(sensor::Sensor *s) {
    sens_current_selection_price_ = s;
  }
  void set_last_vend_value_sensor(sensor::Sensor *s) { sens_last_vend_value_ = s; }
  void set_vends_ok_total_sensor(sensor::Sensor *s) { sens_vends_ok_total_ = s; }
  void set_vends_failed_total_sensor(sensor::Sensor *s) { sens_vends_failed_total_ = s; }
  void set_money_inserted_total_sensor(sensor::Sensor *s) { sens_money_inserted_total_ = s; }
  void set_money_change_total_sensor(sensor::Sensor *s) { sens_money_change_total_ = s; }
  void set_audit_pairs_pending_sensor(sensor::Sensor *s) { sens_audit_pairs_pending_ = s; }
  void set_last_audit_address_sensor(sensor::Sensor *s) { sens_last_audit_address_ = s; }
  void set_last_audit_value_sensor(sensor::Sensor *s) { sens_last_audit_value_ = s; }
  void set_scaling_factor_sensor(sensor::Sensor *s) { sens_scaling_factor_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_last_master_frame_ts(text_sensor::TextSensor *s) { ts_last_master_frame_ = s; }
  void set_last_slave_frame_ts(text_sensor::TextSensor *s) { ts_last_slave_frame_ = s; }
  // ---- Semantic text sensors ----------------------------------------
  void set_last_command_ts(text_sensor::TextSensor *s) { ts_last_command_ = s; }
  void set_last_vend_outcome_ts(text_sensor::TextSensor *s) { ts_last_vend_outcome_ = s; }
  void set_last_status_text_ts(text_sensor::TextSensor *s) { ts_last_status_text_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_vmc_online_bs(binary_sensor::BinarySensor *s) { bs_vmc_online_ = s; }
  void set_vending_inhibited_bs(binary_sensor::BinarySensor *s) { bs_vending_inhibited_ = s; }
  void set_free_vend_request_bs(binary_sensor::BinarySensor *s) { bs_free_vend_request_ = s; }
  void set_vend_in_progress_bs(binary_sensor::BinarySensor *s) { bs_vend_in_progress_ = s; }
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
  // into the frame-buffering layer. Must be called with mutex_ held.
  void emit_byte_locked_(ChannelCtx *ctx, uint16_t value, bool stop_ok);

  // ---------------- Frame layer ----------------------------------------
  // `flags` is a bit-packed field: bit 0 is the actual 9th bit value
  // (parity bit on EXE; mode bit on MDB), bit 1 is set if this byte
  // failed the parity check. We pass both through to the frame-event
  // so that downstream consumers (hex log, YAML triggers) can flag a
  // parity error without losing the underlying byte. The parity check
  // itself is done in emit_byte_locked_.
  void push_master_byte_(uint8_t data, uint8_t flags, uint32_t now_us);
  void push_slave_byte_(uint8_t data, uint8_t flags, uint32_t now_us);
  void flush_master_locked_(uint32_t now_us);
  void flush_slave_locked_(uint32_t now_us);

  // Match frame body against the registered idle-poll patterns. Mode-bit
  // ignored on purpose (see add_idle_*_pattern docs).
  static bool matches_pattern_(const FrameBuf &f,
                               const std::vector<std::vector<uint8_t>> &patterns);

  // Build an ExeFrameEvent from a fully buffered FrameBuf and enqueue it
  // for main-loop publication. Called with mutex_ held.
  void enqueue_frame_locked_(const FrameBuf &f, ChannelRole role);

  // Drain the frame queue on the main loop; updates sensors, publishes
  // text sensors, fires triggers, prints log lines.
  void drain_event_queue_();

  // ---------------- Semantic layer -------------------------------------
  // Per MEI Protocol A every executive byte's bits 7..5 select the
  // peripheral (`001` VMC, `010` ASU, `011` CPP) and bit 4 picks
  // command (`1`) vs data nibble (`0`). The 0x3X / 0x5X / 0x7X
  // command bytes drive a tiny FSM that attributes each peripheral
  // reply to the right command and unpacks multi-nibble transfers
  // (ACCEPT DATA, ASU writes, audit-pair pulls).
  //
  // Approach: each fully decoded UART byte is fed through one of
  // process_master_byte_sem_ / process_slave_byte_sem_ on the main
  // loop (called from drain_event_queue_, so no locking needed).
  // The functions update sem_ and publish sensors / fire triggers
  // when state changes.
  void process_semantic_(const ExeFrameEvent &ev);
  void process_master_byte_sem_(uint8_t byte, uint32_t end_us);
  void process_slave_byte_sem_(uint8_t byte, uint32_t end_us);
  void handle_vmc_command_(uint8_t cmd_byte);
  void handle_vmc_data_nibble_(uint8_t nib_byte);
  void handle_vmc_reply_(uint8_t cmd, uint8_t reply);
  void handle_asu_command_(uint8_t cmd_byte);
  void handle_asu_data_nibble_(uint8_t nib_byte);
  void handle_asu_reply_(uint8_t cmd, uint8_t reply);
  void handle_cpp_command_(uint8_t cmd_byte);
  void handle_cpp_reply_(uint8_t cmd, uint8_t reply);
  void finalize_vmc_data_block_();
  void finalize_asu_data_block_();
  void update_vmc_online_(uint32_t now_us, bool just_replied);
  void publish_status_change_(uint8_t prev_status);
  void publish_credit_change_(uint16_t prev_base_units, uint8_t prev_scaling);
  void emit_vend_event_(bool ok, uint8_t reply_byte);

  // ---------------- Helpers --------------------------------------------
  // Format the event as a hex string. Each byte is "AA" by default; if
  // show_parity_in_hex_ is on it becomes "AA(b)" where b is the actual
  // 9th bit (handy for cross-checking against MDB-style mode-bit dumps).
  // Bytes that failed the parity check are suffixed with "p" regardless
  // of show_parity_in_hex_.
  std::string format_hex_(const ExeFrameEvent &e) const;
  // Friendly name for an executive command byte ("VMC STATUS",
  // "ASU DATA SYNC", "CPP READ CARD", ...). Returns nullptr for
  // unknown bytes.
  static const char *command_name_(uint8_t cmd_byte);

  // ===================================================================
  // Configuration.
  pulse_inspector::PulseInspector *inspector_{nullptr};
  int master_channel_idx_{-1};
  int slave_channel_idx_{-1};
  uint32_t baud_{9600};
  Parity parity_{Parity::EVEN};
  bool log_raw_frames_{true};
  bool suppress_idle_polls_{false};
  // When true, the hex dump prints the 9th bit in parentheses next to
  // every byte (e.g. "00(0) FE(1)"). Default is off because for 8E1
  // it just clutters the log: the bit is fully determined by the data
  // byte and a parity error is already flagged with a "p" suffix.
  bool show_parity_in_hex_{false};

  // Initialised in setup() from baud_ unless explicitly overridden via
  // YAML inter_byte_timeout: 0 means "auto" (30 bit-times).
  uint32_t inter_byte_timeout_us_{0};
  float bit_us_{104.166667f};

  std::vector<std::vector<uint8_t>> idle_master_patterns_;
  std::vector<std::vector<uint8_t>> idle_slave_patterns_;

  // UART state (always two channels: master + slave -- the captures we've
  // seen are unmistakably full-duplex, with master and slave on separate
  // wires and no "single-wire AUTO mode" worth supporting).
  ChannelCtx ch_[2]{};
  TimeoutArg timeout_args_[2]{};
  uint8_t num_channels_{0};

  // Single shared mutex protects UART state and frame buffers. Contention
  // is minimal (a few bytes per second on each direction in idle).
  SemaphoreHandle_t mutex_{nullptr};

  // Frame queue: producer = UART/timer context, consumer = main loop.
  QueueHandle_t event_queue_{nullptr};
  uint32_t events_dropped_{0};

  FrameBuf master_frame_;
  FrameBuf slave_frame_;

  // ---------------- Counters / stats -----------------------------------
  uint32_t frames_decoded_{0};   // bytes total (UART words)
  uint32_t framing_errors_{0};
  uint32_t parity_errors_{0};
  uint32_t master_bytes_{0};
  uint32_t slave_bytes_{0};
  uint32_t master_frames_{0};
  uint32_t slave_frames_{0};
  uint32_t idle_polls_{0};
  // For computing "bytes in last interval" diagnostic.
  uint32_t prev_master_bytes_{0};
  uint32_t prev_slave_bytes_{0};
  uint32_t prev_frames_decoded_{0};
  uint32_t last_diag_log_ms_{0};

  // Callbacks registered from YAML triggers / lambdas.
  CallbackManager<void(const ExeFrameEvent &)> master_frame_callbacks_;
  CallbackManager<void(const ExeFrameEvent &)> slave_frame_callbacks_;
  CallbackManager<void(const ExeVendEvent &)> vend_complete_callbacks_;
  CallbackManager<void(const ExeCreditEvent &)> credit_change_callbacks_;
  CallbackManager<void(const ExeStatusEvent &)> status_callbacks_;

  // ---------------- Semantic FSM state ---------------------------------
  // Everything in here is touched only on the main loop, so no extra
  // synchronisation is needed.
  struct SemState {
    // Last executive command/nibble byte we saw. The next slave byte
    // is its reply, so we keep this around to attribute replies.
    uint8_t pending_cmd{0};
    bool pending_cmd_valid{false};

    // Multi-nibble VMC ACCEPT DATA in flight. Started by 0x38 ACCEPT
    // DATA, terminated by 0x39 DATA SYNC. Eight nibbles total per
    // MEI Protocol A §4.4.10 (16-bit base units, 8-bit scaling, 4-bit
    // decimal-place mask, 4-bit flags).
    bool collecting_vmc_data{false};
    uint8_t vmc_nibbles[8]{};
    uint8_t vmc_nibble_idx{0};

    // Multi-nibble ASU write in flight. Started by the first 0x4X
    // nibble after an ACK / ASU STATUS, terminated by 0x52 DATA SYNC.
    // Four nibbles total per MEI Protocol A §5.4.3 (8-bit address,
    // 8-bit data).
    bool collecting_asu_data{false};
    uint8_t asu_nibbles[4]{};
    uint8_t asu_nibble_idx{0};

    // 0x33 VEND issued, awaiting a non-PNAK reply from VMC. The reply
    // can take up to 60 s per the spec, so we keep this state alive
    // across many idle polls.
    bool vend_pending{false};
    uint32_t vend_started_us{0};
    // Snapshot of credit at VEND time, used to compute delta consumed
    // by the vend.
    uint16_t vend_credit_base_units_at_start{0};
    uint8_t vend_scaling_at_start{0};
    uint8_t selection_price_at_start{0};

    // VMC status snapshot. We compare on every reply to drive the
    // on_vmc_status trigger only on actual changes.
    uint8_t last_vmc_status{0};
    bool have_vmc_status{false};
    bool free_vend_request{false};
    bool vending_inhibited{false};
    bool bdv_variant{false};
    uint8_t audit_pairs_pending{0};

    // Online watchdog: timestamp of the last successful (non-PNAK)
    // VMC reply. Compared against vmc_online_timeout_ms_ in loop().
    uint32_t last_vmc_reply_ms{0};
    bool vmc_online_state{false};
    bool have_online_state{false};

    // Current real-money credit (computed from the most-recent VMC
    // ACCEPT DATA block). base_units * scaling_factor = real-money
    // amount; decimal_places is just for display formatting.
    uint16_t base_units{0};
    uint8_t scaling_factor{0};
    uint8_t decimal_places{0};
    uint8_t exact_change_flag{0};
    bool credit_initialised{false};

    // Last 0x32 CREDIT poll reply (price/line of selected item).
    // 0xFE means "no vend request" -- we keep selection_price=0 in
    // that case so the sensor reads 0 between selections.
    uint8_t selection_price{0};

    // Vend stats (running, since component start).
    uint32_t vends_ok_total{0};
    uint32_t vends_failed_total{0};
    uint32_t last_vend_value{0};

    // Money flow approximations -- computed from credit deltas, not
    // from ASU writes (which are rare; many machines never use them).
    uint32_t money_inserted_total{0};  // sum of positive deltas
    uint32_t money_change_total{0};    // sum of negative deltas not
                                       // attributable to a vend

    // Latest ASU pair seen on the wire (executive write). Useful for
    // debugging custom audit addresses; the absolute accumulators
    // remain owned by the ASU itself and are not echoed on the link.
    uint8_t last_audit_address{0};
    uint8_t last_audit_value{0};
  };
  SemState sem_;

  // VMC online watchdog timeout (ms, 0 = disabled). Default = 5s,
  // which is ~50x the ~85 ms idle poll cycle and tolerates a brief
  // bus stall without false-OFFLINE.
  uint32_t vmc_online_timeout_ms_{5000};

#ifdef USE_SENSOR
  sensor::Sensor *sens_frames_decoded_{nullptr};
  sensor::Sensor *sens_framing_errors_{nullptr};
  sensor::Sensor *sens_master_bytes_{nullptr};
  sensor::Sensor *sens_slave_bytes_{nullptr};
  sensor::Sensor *sens_master_frames_{nullptr};
  sensor::Sensor *sens_slave_frames_{nullptr};
  sensor::Sensor *sens_idle_polls_{nullptr};
  sensor::Sensor *sens_events_dropped_{nullptr};
  sensor::Sensor *sens_parity_errors_{nullptr};
  // Semantic
  sensor::Sensor *sens_current_credit_{nullptr};
  sensor::Sensor *sens_current_credit_base_units_{nullptr};
  sensor::Sensor *sens_current_selection_price_{nullptr};
  sensor::Sensor *sens_last_vend_value_{nullptr};
  sensor::Sensor *sens_vends_ok_total_{nullptr};
  sensor::Sensor *sens_vends_failed_total_{nullptr};
  sensor::Sensor *sens_money_inserted_total_{nullptr};
  sensor::Sensor *sens_money_change_total_{nullptr};
  sensor::Sensor *sens_audit_pairs_pending_{nullptr};
  sensor::Sensor *sens_last_audit_address_{nullptr};
  sensor::Sensor *sens_last_audit_value_{nullptr};
  sensor::Sensor *sens_scaling_factor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *ts_last_master_frame_{nullptr};
  text_sensor::TextSensor *ts_last_slave_frame_{nullptr};
  text_sensor::TextSensor *ts_last_command_{nullptr};
  text_sensor::TextSensor *ts_last_vend_outcome_{nullptr};
  text_sensor::TextSensor *ts_last_status_text_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *bs_vmc_online_{nullptr};
  binary_sensor::BinarySensor *bs_vending_inhibited_{nullptr};
  binary_sensor::BinarySensor *bs_free_vend_request_{nullptr};
  binary_sensor::BinarySensor *bs_vend_in_progress_{nullptr};
#endif
};

}  // namespace pulse_inspector_exe
}  // namespace esphome

#endif  // USE_ESP_IDF
