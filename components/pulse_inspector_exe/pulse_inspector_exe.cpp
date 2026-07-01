#include "pulse_inspector_exe.h"

#ifdef USE_ESP_IDF

#include <cstdio>
#include <cstring>

#include "esphome/core/log.h"

namespace esphome {
namespace pulse_inspector_exe {

static const char *const TAG = "exe";

// Event queue capacity. ExeFrameEvent is ~80 B on ESP32-C3, so 32 slots
// = ~2.5 KB. Enough to absorb a busy poll cycle without dropping data.
static constexpr UBaseType_t EVENT_QUEUE_LEN = 32;

// How often we dump summary stats (ms).
static constexpr uint32_t DIAG_LOG_INTERVAL_MS = 10000;

// Stale-frame safety timeout for the UART layer (esp_timer safety net).
static constexpr uint32_t SAFETY_TIMEOUT_MARGIN_US = 200;

// ===========================================================================
// Setup / dump / loop / channel configuration
// ===========================================================================

bool PulseInspectorExe::configure_channel_(ChannelCtx *ctx, int channel_idx,
                                              ChannelRole role,
                                              const char *timer_name) {
  const auto &channels = this->inspector_->get_channels();
  if (channel_idx < 0 || (size_t) channel_idx >= channels.size()) {
    ESP_LOGE(TAG, "Channel index %d out of range (parent has %u channel(s))",
             channel_idx, (unsigned) channels.size());
    return false;
  }

  ctx->channel_idx = channel_idx;
  ctx->role = role;

  const size_t slot = (size_t) (ctx - this->ch_);
  this->timeout_args_[slot].self = this;
  this->timeout_args_[slot].ctx = ctx;

  esp_timer_create_args_t args = {};
  args.callback = &PulseInspectorExe::safety_timeout_cb_;
  args.arg = &this->timeout_args_[slot];
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = timer_name;
  esp_err_t err = esp_timer_create(&args, &ctx->timeout_timer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_timer_create(%s) failed (err=%d)", timer_name, (int) err);
    return false;
  }

  auto *ch = channels[(size_t) channel_idx];
  ch->add_on_pulse_callback(
      [this, ctx](const pulse_inspector::PulseItem &it) { this->on_pulse(ctx, it); });

  const char *role_str = role == ChannelRole::MASTER ? "MASTER" : "SLAVE";
  ESP_LOGCONFIG(TAG, "  channel %d (GPIO%d) -> %s", channel_idx,
                ch->get_input_gpio_num(), role_str);
  return true;
}

void PulseInspectorExe::setup() {
  if (this->inspector_ == nullptr) {
    ESP_LOGE(TAG, "No parent pulse_inspector configured");
    this->mark_failed();
    return;
  }

  this->bit_us_ = 1000000.0f / (float) this->baud_;
  // Same heuristic as the MDB component: 30 bit-times. At 9600 that's
  // ~3.1 ms, which is comfortably above one-byte-time (~1.15 ms) but
  // far below the inter-poll-cycle silence (~70 ms in idle on Necta
  // Executive). Captures show 13 ms between the two master bytes of
  // an idle poll, so this default groups them into separate frames --
  // which is what we want (we don't yet know whether `00 FE` is one
  // logical command or two; logging two short frames makes the
  // ambiguity explicit). Override via YAML if you want them merged.
  if (this->inter_byte_timeout_us_ == 0) {
    this->inter_byte_timeout_us_ = (uint32_t) (30.0f * this->bit_us_);
  }

  this->mutex_ = xSemaphoreCreateMutex();
  if (this->mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create mutex");
    this->mark_failed();
    return;
  }

  this->event_queue_ = xQueueCreate(EVENT_QUEUE_LEN, sizeof(ExeFrameEvent));
  if (this->event_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create event queue");
    this->mark_failed();
    return;
  }

  if (this->master_channel_idx_ < 0 || this->slave_channel_idx_ < 0) {
    ESP_LOGE(TAG, "Both master_channel and slave_channel must be configured");
    this->mark_failed();
    return;
  }
  if (this->master_channel_idx_ == this->slave_channel_idx_) {
    ESP_LOGE(TAG, "master_channel and slave_channel must differ");
    this->mark_failed();
    return;
  }

  if (!this->configure_channel_(&this->ch_[0], this->master_channel_idx_,
                                ChannelRole::MASTER, "exe_tx_to")) {
    this->mark_failed();
    return;
  }
  if (!this->configure_channel_(&this->ch_[1], this->slave_channel_idx_,
                                ChannelRole::SLAVE, "exe_rx_to")) {
    this->mark_failed();
    return;
  }
  this->num_channels_ = 2;
  ESP_LOGCONFIG(TAG, "EXE decoder ready (master=%d, slave=%d)",
                this->master_channel_idx_, this->slave_channel_idx_);
}

void PulseInspectorExe::dump_config() {
  ESP_LOGCONFIG(TAG, "pulse_inspector_exe:");
  ESP_LOGCONFIG(TAG, "  Master channel: %d", this->ch_[0].channel_idx);
  ESP_LOGCONFIG(TAG, "  Slave  channel: %d", this->ch_[1].channel_idx);
  const char *par = "even";
  switch (this->parity_) {
    case Parity::NONE:  par = "none";  break;
    case Parity::EVEN:  par = "even";  break;
    case Parity::ODD:   par = "odd";   break;
    case Parity::MARK:  par = "mark";  break;
    case Parity::SPACE: par = "space"; break;
  }
  ESP_LOGCONFIG(TAG, "  Baud: %u (bit = %.2f us, parity = %s, "
                     "inter-byte timeout = %u us)",
                (unsigned) this->baud_, this->bit_us_, par,
                (unsigned) this->inter_byte_timeout_us_);
  ESP_LOGCONFIG(TAG, "  Log raw frames:      %s", this->log_raw_frames_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Suppress idle polls: %s", this->suppress_idle_polls_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Show parity in hex:  %s", this->show_parity_in_hex_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Idle master patterns: %u",
                (unsigned) this->idle_master_patterns_.size());
  ESP_LOGCONFIG(TAG, "  Idle slave patterns:  %u",
                (unsigned) this->idle_slave_patterns_.size());
  ESP_LOGCONFIG(TAG, "  Event queue capacity: %u", (unsigned) EVENT_QUEUE_LEN);
  if (this->vmc_online_timeout_ms_ > 0) {
    ESP_LOGCONFIG(TAG, "  VMC online timeout:   %u ms",
                  (unsigned) this->vmc_online_timeout_ms_);
  } else {
    ESP_LOGCONFIG(TAG, "  VMC online timeout:   disabled");
  }
}

void PulseInspectorExe::loop() {
  if (this->mutex_ == nullptr || this->is_failed()) {
    return;
  }
  const uint32_t now_ms = millis();

  // Online watchdog: if no successful VMC reply has come in for
  // vmc_online_timeout_ms_, flip the binary sensor to false. This
  // catches a wire disconnect / VMC reset cleanly without us having
  // to teach drain_event_queue_ about timeouts.
  if (this->vmc_online_timeout_ms_ > 0 && this->sem_.have_online_state) {
    const uint32_t since_ms = now_ms - this->sem_.last_vmc_reply_ms;
    if (this->sem_.vmc_online_state && since_ms > this->vmc_online_timeout_ms_) {
      this->sem_.vmc_online_state = false;
#ifdef USE_BINARY_SENSOR
      if (this->bs_vmc_online_) this->bs_vmc_online_->publish_state(false);
#endif
    }
  }

  // 1. Finalize any per-channel frames flagged by the safety timer, plus
  //    flush stale master/slave frame buffers.
  if (xSemaphoreTake(this->mutex_, pdMS_TO_TICKS(5)) == pdTRUE) {
    for (uint8_t i = 0; i < this->num_channels_; i++) {
      ChannelCtx &ctx = this->ch_[i];
      if (ctx.timeout_pending) {
        ctx.timeout_pending = false;
        if (ctx.state == ChannelCtx::State::FRAME) {
          this->finalize_frame_locked_(&ctx);
        }
      }
    }
    const uint32_t now_us = (uint32_t) esp_timer_get_time();
    if (this->master_frame_.active &&
        (now_us - this->master_frame_.last_byte_us) > this->inter_byte_timeout_us_) {
      this->flush_master_locked_(now_us);
    }
    if (this->slave_frame_.active &&
        (now_us - this->slave_frame_.last_byte_us) > this->inter_byte_timeout_us_) {
      this->flush_slave_locked_(now_us);
    }
    xSemaphoreGive(this->mutex_);
  }

  // 2. Drain the event queue on the main loop.
  this->drain_event_queue_();

  if (now_ms - this->last_diag_log_ms_ >= DIAG_LOG_INTERVAL_MS) {
    this->last_diag_log_ms_ = now_ms;

    const uint32_t d_master = this->master_bytes_ - this->prev_master_bytes_;
    const uint32_t d_slave = this->slave_bytes_ - this->prev_slave_bytes_;
    this->prev_master_bytes_ = this->master_bytes_;
    this->prev_slave_bytes_ = this->slave_bytes_;
    this->prev_frames_decoded_ = this->frames_decoded_;

    ESP_LOGD(TAG,
             "bytes m=%u (+%u) s=%u (+%u), frames m=%u s=%u, framing_err=%u, "
             "parity_err=%u, idle_polls=%u, dropped=%u",
             (unsigned) this->master_bytes_, (unsigned) d_master,
             (unsigned) this->slave_bytes_, (unsigned) d_slave,
             (unsigned) this->master_frames_, (unsigned) this->slave_frames_,
             (unsigned) this->framing_errors_, (unsigned) this->parity_errors_,
             (unsigned) this->idle_polls_, (unsigned) this->events_dropped_);

#ifdef USE_SENSOR
    if (this->sens_frames_decoded_) this->sens_frames_decoded_->publish_state((float) this->frames_decoded_);
    if (this->sens_framing_errors_) this->sens_framing_errors_->publish_state((float) this->framing_errors_);
    if (this->sens_parity_errors_) this->sens_parity_errors_->publish_state((float) this->parity_errors_);
    if (this->sens_master_bytes_) this->sens_master_bytes_->publish_state((float) this->master_bytes_);
    if (this->sens_slave_bytes_) this->sens_slave_bytes_->publish_state((float) this->slave_bytes_);
    if (this->sens_master_frames_) this->sens_master_frames_->publish_state((float) this->master_frames_);
    if (this->sens_slave_frames_) this->sens_slave_frames_->publish_state((float) this->slave_frames_);
    if (this->sens_idle_polls_) this->sens_idle_polls_->publish_state((float) this->idle_polls_);
    if (this->sens_events_dropped_) this->sens_events_dropped_->publish_state((float) this->events_dropped_);
#endif
  }
}

// ===========================================================================
// UART layer (carbon copy of pulse_inspector_mdb's UART layer; the
// physical wire format is identical: 9600 baud 9N1, mode-bit on the 9th
// position, edge-driven decoding via pulse_inspector pulses).
// ===========================================================================

void PulseInspectorExe::safety_timeout_cb_(void *arg) {
  // Runs on esp_timer_task with a small stack -- defer all real work to
  // loop() via the timeout_pending flag.
  auto *ta = static_cast<TimeoutArg *>(arg);
  if (ta == nullptr || ta->ctx == nullptr) {
    return;
  }
  ta->ctx->timeout_pending = true;
}

void PulseInspectorExe::on_pulse(ChannelCtx *ctx,
                                    const pulse_inspector::PulseItem &item) {
  if (this->mutex_ == nullptr || this->is_failed()) {
    return;
  }
  if (xSemaphoreTake(this->mutex_, pdMS_TO_TICKS(20)) != pdTRUE) {
    // Contention with the safety timer or loop(): drop this edge to
    // avoid blocking the pulse_inspector channel task.
    this->events_dropped_++;
    return;
  }
  this->on_edge_locked_(ctx, item.cycle, item.level);
  xSemaphoreGive(this->mutex_);
}

void PulseInspectorExe::fill_bits_until_(ChannelCtx *ctx, int upper) {
  for (int i = (int) ctx->bits_filled; i <= upper && i <= 10; i++) {
    if (i == 0) {
      if (ctx->last_level) {
        this->framing_errors_++;
      }
    } else if (i <= 9) {
      if (ctx->last_level) {
        ctx->frame_value |= (uint16_t) (1u << (i - 1));
      }
    } else {
      ctx->stop_bit_seen = ctx->last_level;
    }
  }
  if (upper >= 10) {
    ctx->bits_filled = 11;
  } else if (upper + 1 > (int) ctx->bits_filled) {
    ctx->bits_filled = (uint8_t) (upper + 1);
  }
}

void PulseInspectorExe::on_edge_locked_(ChannelCtx *ctx, uint32_t t_us,
                                           bool level) {
  if (ctx->state == ChannelCtx::State::FRAME) {
    const uint32_t elapsed = t_us - ctx->frame_start_us;
    const uint32_t frame_total_us = (uint32_t) (11.0f * this->bit_us_);

    if (elapsed < frame_total_us) {
      int upper = (int) ((float) elapsed / this->bit_us_ - 0.5f);
      if (upper < 0) upper = -1;
      this->fill_bits_until_(ctx, upper);
      ctx->last_edge_us = t_us;
      ctx->last_level = level;

      if (ctx->bits_filled >= 11) {
        esp_timer_stop(ctx->timeout_timer);
        this->emit_byte_locked_(ctx, ctx->frame_value, ctx->stop_bit_seen);
        ctx->state = ChannelCtx::State::IDLE;
      }
      return;
    }

    esp_timer_stop(ctx->timeout_timer);
    this->finalize_frame_locked_(ctx);
  }

  if (!level) {
    ctx->state = ChannelCtx::State::FRAME;
    ctx->frame_start_us = t_us;
    ctx->last_edge_us = t_us;
    ctx->last_level = false;
    ctx->bits_filled = 1;
    ctx->frame_value = 0;
    ctx->stop_bit_seen = false;

    const uint32_t timeout_us =
        (uint32_t) (11.0f * this->bit_us_) + SAFETY_TIMEOUT_MARGIN_US;
    esp_timer_stop(ctx->timeout_timer);
    esp_timer_start_once(ctx->timeout_timer, timeout_us);
  }
}

void PulseInspectorExe::finalize_frame_locked_(ChannelCtx *ctx) {
  this->fill_bits_until_(ctx, 10);
  this->emit_byte_locked_(ctx, ctx->frame_value, ctx->stop_bit_seen);
  ctx->state = ChannelCtx::State::IDLE;
}

void PulseInspectorExe::emit_byte_locked_(ChannelCtx *ctx, uint16_t value,
                                             bool stop_ok) {
  this->frames_decoded_++;
  if (!stop_ok) {
    this->framing_errors_++;
  }

  const uint8_t data = (uint8_t) (value & 0xFFu);
  const bool mode = ((value >> 8) & 1u) != 0u;

  // Verify the 9th bit against the configured parity. We still pass
  // `mode` down to the frame layer (so it shows up in hex dumps and
  // YAML triggers) regardless of the result: a parity-broken byte
  // is still useful to see, just flagged.
  bool parity_ok = true;
  if (this->parity_ != Parity::NONE) {
    // Compute even parity by XOR-folding the data byte.
    uint8_t v = data;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    const uint8_t even = v & 1u;
    uint8_t expected;
    switch (this->parity_) {
      case Parity::EVEN:  expected = even;        break;
      case Parity::ODD:   expected = even ^ 1u;   break;
      case Parity::MARK:  expected = 1u;          break;
      case Parity::SPACE: expected = 0u;          break;
      default:            expected = mode ? 1u : 0u;  // NONE: never trips
    }
    if (((mode ? 1u : 0u)) != expected) {
      parity_ok = false;
      this->parity_errors_++;
    }
  }
  // Encode parity verdict into the high bit of the per-byte mode value
  // we hand to the frame layer: bit 0 = the actual 9th bit, bit 1 set
  // means "parity error". The frame buffer is uint8_t, so two bits is
  // exactly what fits.
  const uint8_t mode_byte = (uint8_t) ((mode ? 1u : 0u) | (parity_ok ? 0u : 0x02u));

  // Use wire-end timestamp (start-of-frame + 11 bit-times), not the wall
  // clock -- see pulse_inspector_mdb for the rationale; same problem
  // applies here when the byte is finalized via the safety-timer path on
  // the main task.
  const uint32_t wire_end_us =
      ctx->frame_start_us + (uint32_t) (11.0f * this->bit_us_);

  if (ctx->role == ChannelRole::MASTER) {
    this->master_bytes_++;
    this->push_master_byte_(data, mode_byte, wire_end_us);
  } else {
    this->slave_bytes_++;
    this->push_slave_byte_(data, mode_byte, wire_end_us);
  }
}

// ===========================================================================
// Frame layer -- timing-only grouping (we don't trust the mode-bit
// semantically yet, so we deliberately do NOT use it as a frame
// boundary).
// ===========================================================================

void PulseInspectorExe::push_master_byte_(uint8_t data, uint8_t flags,
                                             uint32_t now_us) {
  if (this->master_frame_.active &&
      (now_us - this->master_frame_.last_byte_us) > this->inter_byte_timeout_us_) {
    this->flush_master_locked_(now_us);
  }
  if (!this->master_frame_.active) {
    this->master_frame_.active = true;
    this->master_frame_.len = 0;
    this->master_frame_.first_byte_us = now_us;
  }
  if (this->master_frame_.len >= FrameBuf::CAPACITY) {
    this->flush_master_locked_(now_us);
    this->master_frame_.active = true;
    this->master_frame_.first_byte_us = now_us;
  }
  const size_t i = this->master_frame_.len++;
  this->master_frame_.buf[i] = data;
  this->master_frame_.mode[i] = flags;
  this->master_frame_.last_byte_us = now_us;
}

void PulseInspectorExe::push_slave_byte_(uint8_t data, uint8_t flags,
                                            uint32_t now_us) {
  // Same trick as in MDB: an incoming slave byte is a hard boundary --
  // a peripheral never speaks while the master is mid-command. So flush
  // any pending master frame BEFORE we touch the slave buffer, otherwise
  // the master frame could end up timestamped after the slave reply.
  if (this->master_frame_.active) {
    this->flush_master_locked_(now_us);
  }
  if (this->slave_frame_.active &&
      (now_us - this->slave_frame_.last_byte_us) > this->inter_byte_timeout_us_) {
    this->flush_slave_locked_(now_us);
  }
  if (!this->slave_frame_.active) {
    this->slave_frame_.active = true;
    this->slave_frame_.len = 0;
    this->slave_frame_.first_byte_us = now_us;
  }
  if (this->slave_frame_.len >= FrameBuf::CAPACITY) {
    this->flush_slave_locked_(now_us);
    this->slave_frame_.active = true;
    this->slave_frame_.first_byte_us = now_us;
  }
  const size_t i = this->slave_frame_.len++;
  this->slave_frame_.buf[i] = data;
  this->slave_frame_.mode[i] = flags;
  this->slave_frame_.last_byte_us = now_us;
}

void PulseInspectorExe::flush_master_locked_(uint32_t now_us) {
  if (!this->master_frame_.active || this->master_frame_.len == 0) {
    this->master_frame_.active = false;
    this->master_frame_.len = 0;
    return;
  }
  this->master_frames_++;
  this->enqueue_frame_locked_(this->master_frame_, ChannelRole::MASTER);
  this->master_frame_.active = false;
  this->master_frame_.len = 0;
  (void) now_us;
}

void PulseInspectorExe::flush_slave_locked_(uint32_t now_us) {
  if (!this->slave_frame_.active || this->slave_frame_.len == 0) {
    this->slave_frame_.active = false;
    this->slave_frame_.len = 0;
    return;
  }
  this->slave_frames_++;
  this->enqueue_frame_locked_(this->slave_frame_, ChannelRole::SLAVE);
  this->slave_frame_.active = false;
  this->slave_frame_.len = 0;
  (void) now_us;
}

bool PulseInspectorExe::matches_pattern_(
    const FrameBuf &f, const std::vector<std::vector<uint8_t>> &patterns) {
  for (const auto &pat : patterns) {
    if (pat.size() != f.len) continue;
    bool eq = true;
    for (size_t i = 0; i < pat.size(); i++) {
      if (pat[i] != f.buf[i]) {
        eq = false;
        break;
      }
    }
    if (eq) return true;
  }
  return false;
}

void PulseInspectorExe::enqueue_frame_locked_(const FrameBuf &f,
                                                 ChannelRole role) {
  ExeFrameEvent ev{};
  ev.role = role == ChannelRole::MASTER ? ExeFrameEvent::Role::MASTER
                                        : ExeFrameEvent::Role::SLAVE;
  ev.end_us = f.last_byte_us;
  ev.len = (uint8_t) std::min<size_t>(f.len, ExeFrameEvent::CAPACITY);
  for (uint8_t i = 0; i < ev.len; i++) {
    ev.data[i] = f.buf[i];
    ev.mode[i] = f.mode[i];
  }
  const auto &patterns = role == ChannelRole::MASTER
                              ? this->idle_master_patterns_
                              : this->idle_slave_patterns_;
  ev.suppressed = this->matches_pattern_(f, patterns);
  if (ev.suppressed) {
    this->idle_polls_++;
  }

  if (this->event_queue_ == nullptr) return;
  if (xQueueSend(this->event_queue_, &ev, 0) != pdTRUE) {
    this->events_dropped_++;
  }
}

// ===========================================================================
// Main-loop publication
// ===========================================================================

std::string PulseInspectorExe::format_hex_(const ExeFrameEvent &e) const {
  // Per-byte rendering: "AA" by default, "AA(b)" with the 9th bit shown
  // when show_parity_in_hex_ is on, plus a trailing "p" if parity failed
  // (regardless of the flag). Buffer holds at most "AA(0)p" plus a space,
  // so 8 bytes is plenty.
  char tmp[8];
  std::string out;
  out.reserve((size_t) e.len * 6);
  for (uint8_t i = 0; i < e.len; i++) {
    if (i) out.push_back(' ');
    if (this->show_parity_in_hex_) {
      snprintf(tmp, sizeof(tmp), "%02X(%u)", (unsigned) e.data[i],
               (unsigned) (e.mode[i] & 1u));
    } else {
      snprintf(tmp, sizeof(tmp), "%02X", (unsigned) e.data[i]);
    }
    out.append(tmp);
    if (e.mode[i] & 0x02u) {
      out.push_back('p');
    }
  }
  return out;
}

void PulseInspectorExe::drain_event_queue_() {
  if (this->event_queue_ == nullptr) return;
  ExeFrameEvent ev;
  while (xQueueReceive(this->event_queue_, &ev, 0) == pdTRUE) {
    const bool is_master = ev.role == ExeFrameEvent::Role::MASTER;
    const std::string hex = format_hex_(ev);

    if (this->log_raw_frames_ &&
        !(this->suppress_idle_polls_ && ev.suppressed)) {
      ESP_LOGD(TAG, "%s [%u] %s", is_master ? "master" : "slave ",
               (unsigned) ev.len, hex.c_str());
    }

#ifdef USE_TEXT_SENSOR
    // Text sensors only get updated for non-suppressed frames so the
    // last-frame display reflects the last "interesting" event rather
    // than the latest idle ping.
    if (!ev.suppressed) {
      if (is_master && this->ts_last_master_frame_) {
        this->ts_last_master_frame_->publish_state(hex);
      } else if (!is_master && this->ts_last_slave_frame_) {
        this->ts_last_slave_frame_->publish_state(hex);
      }
    }
#endif

    // Run the semantic parser BEFORE firing user callbacks so any
    // sensor / state changes are visible to YAML lambdas listening
    // on on_master_frame / on_slave_frame.
    this->process_semantic_(ev);

    if (is_master) {
      this->master_frame_callbacks_.call(ev);
    } else {
      this->slave_frame_callbacks_.call(ev);
    }
  }
}

// ===========================================================================
// Semantic layer
//
// We feed every decoded byte (regardless of frame grouping or suppression)
// through one of process_master_byte_sem_ / process_slave_byte_sem_ on the
// main loop. The pending_cmd field carries the most recent executive byte
// across into the slave handler, so that "reply 0x00 to STATUS" and
// "reply 0x00 to ACCEPT DATA" can be distinguished.
//
// The parser only knows about MEI Protocol A bytes (the spec at
// https://www.wrzutnik.com/wp-content/uploads/Executiv-MEI-000304001-
// Protocol-A-Y2.pdf). Unknown command bytes are still tracked
// (pending_cmd is updated, name shown as "unknown") but their replies
// do not generate semantic events.
// ===========================================================================

namespace {

// MEI Protocol A byte structure: address bits 7..5, mode bit 4
// (1 = command, 0 = data nibble), command/nibble bits 3..0.
constexpr uint8_t exe_addr(uint8_t b) { return (b >> 5) & 0x07u; }
constexpr bool exe_is_command(uint8_t b) { return (b & 0x10u) != 0u; }
constexpr uint8_t exe_payload(uint8_t b) { return b & 0x0Fu; }

constexpr uint8_t ADDR_VMC = 0x01u;  // 001
constexpr uint8_t ADDR_ASU = 0x02u;  // 010
constexpr uint8_t ADDR_CPP = 0x03u;  // 011

}  // namespace

void PulseInspectorExe::process_semantic_(const ExeFrameEvent &ev) {
  // Every byte in the frame is processed independently. This works
  // because the executive protocol's framing is purely a logging
  // convenience -- the application layer is byte-oriented, with
  // request/reply pairing driven by who-spoke-last.
  for (uint8_t i = 0; i < ev.len; i++) {
    // Skip bytes that failed parity: the payload is unreliable, and
    // the executive will reissue / NAK them anyway. Counting them is
    // already done at the UART layer.
    if (ev.mode[i] & 0x02u) continue;
    const uint8_t b = ev.data[i];
    if (ev.role == ExeFrameEvent::Role::MASTER) {
      this->process_master_byte_sem_(b, ev.end_us);
    } else {
      this->process_slave_byte_sem_(b, ev.end_us);
    }
  }
}

void PulseInspectorExe::process_master_byte_sem_(uint8_t byte,
                                                    uint32_t /*end_us*/) {
  const uint8_t addr = exe_addr(byte);
  const bool is_cmd = exe_is_command(byte);

  // Remember this byte so the next slave byte can be attributed to it.
  this->sem_.pending_cmd = byte;
  this->sem_.pending_cmd_valid = true;

#ifdef USE_TEXT_SENSOR
  if (this->ts_last_command_) {
    const char *name = command_name_(byte);
    char buf[40];
    if (name) {
      snprintf(buf, sizeof(buf), "%02X %s", (unsigned) byte, name);
    } else if (!is_cmd) {
      snprintf(buf, sizeof(buf), "%02X data nibble", (unsigned) byte);
    } else {
      snprintf(buf, sizeof(buf), "%02X (unknown)", (unsigned) byte);
    }
    // Only publish if it changed; ESPHome dedup is cheap but this saves
    // some logging churn in idle. Static is fine -- we run on a single
    // FreeRTOS task (the main loop) so there's no TLS issue.
    static std::string last_cmd_text;
    std::string s = buf;
    if (s != last_cmd_text) {
      this->ts_last_command_->publish_state(s);
      last_cmd_text = s;
    }
  }
#endif

  if (addr == ADDR_VMC) {
    if (is_cmd) {
      this->handle_vmc_command_(byte);
    } else {
      this->handle_vmc_data_nibble_(byte);
    }
  } else if (addr == ADDR_ASU) {
    if (is_cmd) {
      this->handle_asu_command_(byte);
    } else {
      this->handle_asu_data_nibble_(byte);
    }
  } else if (addr == ADDR_CPP) {
    if (is_cmd) {
      this->handle_cpp_command_(byte);
    }
    // CPP nibbles (0x6X) are tracked at the byte level only; we don't
    // currently decode card credit transfers.
  }
}

void PulseInspectorExe::process_slave_byte_sem_(uint8_t byte,
                                                   uint32_t end_us) {
  if (!this->sem_.pending_cmd_valid) return;
  const uint8_t cmd = this->sem_.pending_cmd;
  // Each peripheral reply consumes the pending command. If the reply
  // is PNAK (0xFF) the executive will reissue, but in either case the
  // next byte we see should be a new executive byte, so clearing here
  // is safe.
  this->sem_.pending_cmd_valid = false;

  // PNAK is a per-link transport error, not an application reply.
  // Update parity counter is already done at UART layer; here we
  // simply skip semantic handling.
  if (byte == 0xFFu) {
    return;
  }

  // Any non-PNAK reply from the VMC counts as "machine alive".
  if (exe_addr(cmd) == ADDR_VMC) {
    this->update_vmc_online_(end_us, true);
    this->handle_vmc_reply_(cmd, byte);
  } else if (exe_addr(cmd) == ADDR_ASU) {
    this->handle_asu_reply_(cmd, byte);
  } else if (exe_addr(cmd) == ADDR_CPP) {
    this->handle_cpp_reply_(cmd, byte);
  }
}

// ----- VMC command/reply handlers -----------------------------------------

void PulseInspectorExe::handle_vmc_command_(uint8_t cmd_byte) {
  const uint8_t cmd = exe_payload(cmd_byte);
  switch (cmd) {
    case 0x3u:  // 0x33 VEND
      // The executive halts all polling until VEND reply arrives (up to
      // 60 s). We snapshot the credit / selection so emit_vend_event_
      // can report the value consumed even if the reply lands far in
      // the future.
      this->sem_.vend_pending = true;
      this->sem_.vend_started_us = (uint32_t) esp_timer_get_time();
      this->sem_.vend_credit_base_units_at_start = this->sem_.base_units;
      this->sem_.vend_scaling_at_start = this->sem_.scaling_factor;
      this->sem_.selection_price_at_start = this->sem_.selection_price;
#ifdef USE_BINARY_SENSOR
      if (this->bs_vend_in_progress_) {
        this->bs_vend_in_progress_->publish_state(true);
      }
#endif
      break;
    case 0x8u:  // 0x38 ACCEPT DATA
      this->sem_.collecting_vmc_data = true;
      this->sem_.vmc_nibble_idx = 0;
      break;
    case 0x9u:  // 0x39 DATA SYNC
      if (this->sem_.collecting_vmc_data) {
        this->finalize_vmc_data_block_();
        this->sem_.collecting_vmc_data = false;
        this->sem_.vmc_nibble_idx = 0;
      }
      break;
    default:
      break;
  }
}

void PulseInspectorExe::handle_vmc_data_nibble_(uint8_t nib_byte) {
  if (!this->sem_.collecting_vmc_data) return;
  if (this->sem_.vmc_nibble_idx >= 8) {
    // Overflow: spec says exactly 8 nibbles. Drop excess and wait
    // for DATA SYNC to clear state.
    return;
  }
  this->sem_.vmc_nibbles[this->sem_.vmc_nibble_idx++] = exe_payload(nib_byte);
}

void PulseInspectorExe::handle_vmc_reply_(uint8_t cmd, uint8_t reply) {
  const uint8_t c = exe_payload(cmd);
  // Data nibble reply: command was a 0x2X data nibble, so reply is
  // just an ACK. Nothing to do at the semantic layer.
  if (!exe_is_command(cmd)) return;

  switch (c) {
    case 0x1u: {  // STATUS reply
      const uint8_t prev = this->sem_.last_vmc_status;
      this->sem_.last_vmc_status = reply;
      this->sem_.have_vmc_status = true;
      const bool free_vend = (reply & 0x80u) != 0u;
      const bool inhibited = (reply & 0x40u) != 0u;
      const bool bdv = (reply & 0x20u) != 0u;
      const uint8_t pairs = reply & 0x1Fu;
      const bool changed =
          (prev != reply) ||
          this->sem_.free_vend_request != free_vend ||
          this->sem_.vending_inhibited != inhibited ||
          this->sem_.audit_pairs_pending != pairs;
      this->sem_.free_vend_request = free_vend;
      this->sem_.vending_inhibited = inhibited;
      this->sem_.bdv_variant = bdv;
      this->sem_.audit_pairs_pending = pairs;
      if (changed) {
        this->publish_status_change_(prev);
      }
      break;
    }
    case 0x2u: {  // CREDIT reply
      // 0xFE = "no vend request"; anything else is a price/line of
      // the currently-selected drink.
      const uint8_t price = (reply == 0xFEu) ? 0u : reply;
      if (price != this->sem_.selection_price) {
        this->sem_.selection_price = price;
#ifdef USE_SENSOR
        if (this->sens_current_selection_price_) {
          this->sens_current_selection_price_->publish_state((float) price);
        }
#endif
      }
      break;
    }
    case 0x3u: {  // VEND reply
      // Reply: bit 7 = vend failed; bits 6..0 = audit pairs pending.
      const bool ok = (reply & 0x80u) == 0u;
      this->emit_vend_event_(ok, reply);
      break;
    }
    case 0x4u: {  // AUDIT reply (count of pairs to follow)
      // This is just a count; not exposed as a sensor (it would race
      // with audit_pairs_pending from STATUS). We just remember nothing
      // here -- the actual data comes via 0x35 / 0x36 below.
      break;
    }
    case 0x5u: {  // SEND AUDIT ADDRESS reply
      if (reply <= 250u) {
        this->sem_.last_audit_address = reply;
#ifdef USE_SENSOR
        if (this->sens_last_audit_address_) {
          this->sens_last_audit_address_->publish_state((float) reply);
        }
#endif
      }
      break;
    }
    case 0x6u: {  // SEND AUDIT DATA reply
      if (reply <= 250u) {
        this->sem_.last_audit_value = reply;
#ifdef USE_SENSOR
        if (this->sens_last_audit_value_) {
          this->sens_last_audit_value_->publish_state((float) reply);
        }
#endif
      }
      break;
    }
    default:
      break;
  }
}

// ----- ASU command/reply handlers -----------------------------------------

void PulseInspectorExe::handle_asu_command_(uint8_t cmd_byte) {
  const uint8_t cmd = exe_payload(cmd_byte);
  if (cmd == 0x2u) {  // 0x52 DATA SYNC
    if (this->sem_.collecting_asu_data &&
        this->sem_.asu_nibble_idx >= 4) {
      this->finalize_asu_data_block_();
    }
    this->sem_.collecting_asu_data = false;
    this->sem_.asu_nibble_idx = 0;
  }
}

void PulseInspectorExe::handle_asu_data_nibble_(uint8_t nib_byte) {
  // First 0x4X nibble auto-starts an ASU write block.
  if (!this->sem_.collecting_asu_data) {
    this->sem_.collecting_asu_data = true;
    this->sem_.asu_nibble_idx = 0;
  }
  if (this->sem_.asu_nibble_idx < 4) {
    this->sem_.asu_nibbles[this->sem_.asu_nibble_idx++] = exe_payload(nib_byte);
  }
}

void PulseInspectorExe::handle_asu_reply_(uint8_t /*cmd*/, uint8_t /*reply*/) {
  // ASU only ever replies with 0x00 ACK (or 0xFF PNAK, which we
  // already filtered out). Nothing semantic to extract.
}

// ----- CPP command/reply handlers -----------------------------------------

void PulseInspectorExe::handle_cpp_command_(uint8_t /*cmd_byte*/) {
  // CPP semantic decode is intentionally minimal: we only see CPP
  // traffic if the machine has a cashless reader, and absent a
  // physical test setup we don't try to interpret card data here.
}

void PulseInspectorExe::handle_cpp_reply_(uint8_t /*cmd*/, uint8_t /*reply*/) {
}

// ----- Multi-nibble block finalization ------------------------------------

void PulseInspectorExe::finalize_vmc_data_block_() {
  // Per MEI Protocol A §4.4.10:
  //   nibble 0..3 : 16-bit base-units value (lsn first)
  //   nibble 4..5 : 8-bit scaling factor
  //   nibble 6    : decimal-place mask
  //                  0001=integer, 0010=1, 0100=2, 1000=3
  //   nibble 7    : flags (bit 0 = exact change)
  // We require all 8 nibbles; partial blocks are dropped.
  if (this->sem_.vmc_nibble_idx < 8) return;

  const uint8_t *n = this->sem_.vmc_nibbles;
  const uint16_t prev_base = this->sem_.base_units;
  const uint8_t prev_scale = this->sem_.scaling_factor;
  const bool had_credit = this->sem_.credit_initialised;

  this->sem_.base_units =
      (uint16_t) (n[0] | (n[1] << 4) | (n[2] << 8) | (n[3] << 12));
  this->sem_.scaling_factor = (uint8_t) (n[4] | (n[5] << 4));
  // Decode decimal-place mask into a count for display purposes.
  const uint8_t dp_mask = n[6] & 0x0Fu;
  uint8_t dp = 0;
  if (dp_mask & 0x02u) dp = 1;
  else if (dp_mask & 0x04u) dp = 2;
  else if (dp_mask & 0x08u) dp = 3;
  this->sem_.decimal_places = dp;
  this->sem_.exact_change_flag = n[7] & 0x01u;
  this->sem_.credit_initialised = true;

  this->publish_credit_change_(prev_base, prev_scale);
  (void) had_credit;
}

void PulseInspectorExe::finalize_asu_data_block_() {
  // Per MEI Protocol A §5.4.3:
  //   nibble 0..1 : 8-bit address (lsn first)
  //   nibble 2..3 : 8-bit data value (lsn first)
  // We surface this as last_audit_address / last_audit_value sensors
  // -- the absolute accumulator stays on the ASU itself and is not
  // mirrored on the wire.
  const uint8_t *n = this->sem_.asu_nibbles;
  const uint8_t addr = (uint8_t) (n[0] | (n[1] << 4));
  const uint8_t data = (uint8_t) (n[2] | (n[3] << 4));
  this->sem_.last_audit_address = addr;
  this->sem_.last_audit_value = data;
#ifdef USE_SENSOR
  if (this->sens_last_audit_address_) {
    this->sens_last_audit_address_->publish_state((float) addr);
  }
  if (this->sens_last_audit_value_) {
    this->sens_last_audit_value_->publish_state((float) data);
  }
#endif
}

// ----- Online watchdog & state publish ------------------------------------

void PulseInspectorExe::update_vmc_online_(uint32_t /*now_us*/,
                                              bool just_replied) {
  if (!just_replied) return;
  this->sem_.last_vmc_reply_ms = millis();
  if (!this->sem_.vmc_online_state || !this->sem_.have_online_state) {
    this->sem_.vmc_online_state = true;
    this->sem_.have_online_state = true;
#ifdef USE_BINARY_SENSOR
    if (this->bs_vmc_online_) this->bs_vmc_online_->publish_state(true);
#endif
  }
}

void PulseInspectorExe::publish_status_change_(uint8_t prev_status) {
#ifdef USE_BINARY_SENSOR
  if (this->bs_vending_inhibited_) {
    this->bs_vending_inhibited_->publish_state(this->sem_.vending_inhibited);
  }
  if (this->bs_free_vend_request_) {
    this->bs_free_vend_request_->publish_state(this->sem_.free_vend_request);
  }
#endif
#ifdef USE_SENSOR
  if (this->sens_audit_pairs_pending_) {
    this->sens_audit_pairs_pending_->publish_state(
        (float) this->sem_.audit_pairs_pending);
  }
#endif
#ifdef USE_TEXT_SENSOR
  if (this->ts_last_status_text_) {
    char buf[64];
    snprintf(buf, sizeof(buf),
             "0x%02X %s%s%s pairs=%u",
             (unsigned) this->sem_.last_vmc_status,
             this->sem_.vending_inhibited ? "INHIBITED " : "",
             this->sem_.free_vend_request ? "FREE " : "",
             this->sem_.bdv_variant ? "BDV " : "STD ",
             (unsigned) this->sem_.audit_pairs_pending);
    this->ts_last_status_text_->publish_state(buf);
  }
#endif
  ExeStatusEvent e{};
  e.status_raw = this->sem_.last_vmc_status;
  e.free_vend_request = this->sem_.free_vend_request;
  e.vending_inhibited = this->sem_.vending_inhibited;
  e.bdv_variant = this->sem_.bdv_variant;
  e.audit_pairs_pending = this->sem_.audit_pairs_pending;
  e.prev_status_raw = prev_status;
  this->status_callbacks_.call(e);
}

void PulseInspectorExe::publish_credit_change_(uint16_t prev_base_units,
                                                  uint8_t prev_scaling) {
  const uint32_t new_credit =
      (uint32_t) this->sem_.base_units * (uint32_t) this->sem_.scaling_factor;
  const uint32_t prev_credit =
      (uint32_t) prev_base_units * (uint32_t) prev_scaling;
  const int32_t delta = (int32_t) new_credit - (int32_t) prev_credit;

  // Only count credit deltas if we already had a credit baseline,
  // otherwise the very first ACCEPT DATA we see (which is just the
  // executive transmitting the current credit, not a fresh insert)
  // would erroneously bump money_inserted_total.
  if (prev_base_units != 0 || prev_scaling != 0) {
    if (delta > 0) {
      this->sem_.money_inserted_total += (uint32_t) delta;
    } else if (delta < 0) {
      // A negative delta during a vend is "money consumed by vend"
      // and is captured by emit_vend_event_; outside a vend it is
      // change being returned (or a manual cancel). We attribute
      // negative-and-no-vend-pending deltas to "change".
      if (!this->sem_.vend_pending) {
        this->sem_.money_change_total += (uint32_t) (-delta);
      }
    }
  }

#ifdef USE_SENSOR
  if (this->sens_current_credit_) {
    this->sens_current_credit_->publish_state((float) new_credit);
  }
  if (this->sens_current_credit_base_units_) {
    this->sens_current_credit_base_units_->publish_state(
        (float) this->sem_.base_units);
  }
  if (this->sens_scaling_factor_) {
    this->sens_scaling_factor_->publish_state(
        (float) this->sem_.scaling_factor);
  }
  if (this->sens_money_inserted_total_) {
    this->sens_money_inserted_total_->publish_state(
        (float) this->sem_.money_inserted_total);
  }
  if (this->sens_money_change_total_) {
    this->sens_money_change_total_->publish_state(
        (float) this->sem_.money_change_total);
  }
#endif

  ExeCreditEvent e{};
  e.credit_real_money = new_credit;
  e.base_units = this->sem_.base_units;
  e.scaling_factor = this->sem_.scaling_factor;
  e.decimal_places = this->sem_.decimal_places;
  e.delta_real_money = delta;
  e.exact_change_only = this->sem_.exact_change_flag != 0u;
  this->credit_change_callbacks_.call(e);
}

void PulseInspectorExe::emit_vend_event_(bool ok, uint8_t reply_byte) {
  // "Money consumed" by this vend: difference between credit at the
  // moment the executive shipped 0x33 VEND and the next ACCEPT DATA
  // block. We don't have the post-vend credit yet, so we use the
  // current selection_price (already in real-money units, since
  // CREDIT replies are scaled) as a best-effort.
  const uint32_t value_real = (uint32_t) this->sem_.selection_price *
                              (uint32_t) this->sem_.scaling_factor;
  if (ok) {
    this->sem_.vends_ok_total++;
    this->sem_.last_vend_value = value_real;
  } else {
    this->sem_.vends_failed_total++;
  }
  this->sem_.vend_pending = false;

#ifdef USE_SENSOR
  if (this->sens_vends_ok_total_) {
    this->sens_vends_ok_total_->publish_state(
        (float) this->sem_.vends_ok_total);
  }
  if (this->sens_vends_failed_total_) {
    this->sens_vends_failed_total_->publish_state(
        (float) this->sem_.vends_failed_total);
  }
  if (ok && this->sens_last_vend_value_) {
    this->sens_last_vend_value_->publish_state(
        (float) this->sem_.last_vend_value);
  }
#endif
#ifdef USE_TEXT_SENSOR
  if (this->ts_last_vend_outcome_) {
    this->ts_last_vend_outcome_->publish_state(ok ? "OK" : "FAILED");
  }
#endif
#ifdef USE_BINARY_SENSOR
  if (this->bs_vend_in_progress_) this->bs_vend_in_progress_->publish_state(false);
#endif

  ExeVendEvent e{};
  e.ok = ok;
  e.value_real_money = value_real;
  e.base_units = this->sem_.base_units;
  e.scaling_factor = this->sem_.scaling_factor;
  e.decimal_places = this->sem_.decimal_places;
  e.selection_price = this->sem_.selection_price;
  e.audit_pairs_pending = (uint8_t) (reply_byte & 0x7Fu);
  this->vend_complete_callbacks_.call(e);
}

const char *PulseInspectorExe::command_name_(uint8_t cmd_byte) {
  // Command bytes: bit 4 set, address in bits 7..5.
  if ((cmd_byte & 0x10u) == 0) return nullptr;
  const uint8_t addr = (cmd_byte >> 5) & 0x07u;
  const uint8_t code = cmd_byte & 0x0Fu;
  switch (addr) {
    case 0x1u:  // VMC
      switch (code) {
        case 0x1u: return "VMC STATUS";
        case 0x2u: return "VMC CREDIT";
        case 0x3u: return "VMC VEND";
        case 0x4u: return "VMC AUDIT";
        case 0x5u: return "VMC SEND AUDIT ADDR";
        case 0x6u: return "VMC SEND AUDIT DATA";
        case 0x7u: return "VMC IDENTIFY";
        case 0x8u: return "VMC ACCEPT DATA";
        case 0x9u: return "VMC DATA SYNC";
        case 0xFu: return "VMC NAK";
        default:   return "VMC ?";
      }
    case 0x2u:  // ASU
      switch (code) {
        case 0x1u: return "ASU STATUS";
        case 0x2u: return "ASU DATA SYNC";
        case 0xFu: return "ASU NAK";
        default:   return "ASU ?";
      }
    case 0x3u:  // CPP
      switch (code) {
        case 0x1u: return "CPP STATUS";
        case 0x2u: return "CPP READ CARD";
        case 0x3u: return "CPP SEND DATA";
        case 0x4u: return "CPP ACCEPT DATA";
        case 0x5u: return "CPP DECREMENT";
        case 0x6u: return "CPP REINSTATE";
        case 0x7u: return "CPP RETURN CARD";
        case 0x8u: return "CPP DATA SYNC";
        case 0x9u: return "CPP AUDIT";
        case 0xAu: return "CPP SEND AUDIT ADDR";
        case 0xBu: return "CPP SEND AUDIT DATA";
        case 0xCu: return "CPP IDENTIFY";
        case 0xDu: return "CPP ACCEPT DISPLAY";
        case 0xFu: return "CPP NAK";
        default:   return "CPP ?";
      }
    default:
      return nullptr;
  }
}

}  // namespace pulse_inspector_exe
}  // namespace esphome

#endif  // USE_ESP_IDF
