#include "pulse_inspector_mdb.h"

#ifdef USE_ESP_IDF

#include <cstdio>
#include <cstring>

#include "esphome/core/log.h"

namespace esphome {
namespace pulse_inspector_mdb {

static const char *const TAG = "mdb";

// Event queue capacity. Each slot holds one full MdbEvent (~128 B on
// ESP32-C3). 32 slots = ~4 KB of heap reserved at setup time, which is
// plenty to absorb a busy POLL cycle without dropping data.
static constexpr UBaseType_t EVENT_QUEUE_LEN = 32;

// How often we dump summary stats (ms).
static constexpr uint32_t DIAG_LOG_INTERVAL_MS = 10000;

// Helper: safely copy a C-string into MdbEvent::description, always
// null-terminating.
static inline void set_desc_(MdbEvent &evt, const char *s) {
  std::strncpy(evt.description, s, MdbEvent::DESC_CAPACITY - 1);
  evt.description[MdbEvent::DESC_CAPACITY - 1] = '\0';
}

// Stale-frame safety timeout for the UART layer (esp_timer safety net).
static constexpr uint32_t SAFETY_TIMEOUT_MARGIN_US = 200;

// How often loop() sweeps for stale frame buffers.
static constexpr uint32_t FRAME_FLUSH_POLL_MS = 5;

// ===========================================================================
// Setup / dump / loop / channel configuration
// ===========================================================================

bool PulseInspectorMdb::configure_channel_(ChannelCtx *ctx, int channel_idx,
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
  args.callback = &PulseInspectorMdb::safety_timeout_cb_;
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

  const char *role_str =
      role == ChannelRole::MASTER ? "MASTER"
                                  : (role == ChannelRole::SLAVE ? "SLAVE" : "AUTO");
  ESP_LOGCONFIG(TAG, "  channel %d (GPIO%d) -> %s", channel_idx,
                ch->get_input_gpio_num(), role_str);
  return true;
}

void PulseInspectorMdb::setup() {
  if (this->inspector_ == nullptr) {
    ESP_LOGE(TAG, "No parent pulse_inspector configured");
    this->mark_failed();
    return;
  }

  this->bit_us_ = 1000000.0f / (float) this->baud_;
  // One 9-N-1 frame already takes ~11 bit-times on the wire, and the emit
  // timestamp sits at the END of the frame. Two back-to-back bytes therefore
  // arrive ~11 bit-times apart at push_*_byte_(), so the stale threshold has
  // to comfortably exceed that. We use 30 bit-times (~3.1 ms at 9600 baud):
  //  - covers back-to-back byte emission (~1.15 ms apart);
  //  - covers up to ~1 ms inter-byte gap allowed inside an MDB command;
  //  - is still far shorter than the inter-poll-cycle idle (>100 ms), so
  //    successive POLL cycles remain correctly separated.
  this->inter_byte_timeout_us_ = (uint32_t) (30.0f * this->bit_us_);

  this->mutex_ = xSemaphoreCreateMutex();
  if (this->mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create mutex");
    this->mark_failed();
    return;
  }

  this->event_queue_ = xQueueCreate(EVENT_QUEUE_LEN, sizeof(MdbEvent));
  if (this->event_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create event queue");
    this->mark_failed();
    return;
  }

  const bool two_wire =
      (this->master_channel_idx_ >= 0 && this->slave_channel_idx_ >= 0);
  const bool single_wire = (this->single_channel_idx_ >= 0);

  if (two_wire) {
    if (this->master_channel_idx_ == this->slave_channel_idx_) {
      ESP_LOGE(TAG, "tx_channel and rx_channel must differ");
      this->mark_failed();
      return;
    }
    if (!this->configure_channel_(&this->ch_[0], this->master_channel_idx_,
                                  ChannelRole::MASTER, "mdb_tx_to")) {
      this->mark_failed();
      return;
    }
    if (!this->configure_channel_(&this->ch_[1], this->slave_channel_idx_,
                                  ChannelRole::SLAVE, "mdb_rx_to")) {
      this->mark_failed();
      return;
    }
    this->num_channels_ = 2;
    ESP_LOGCONFIG(TAG, "MDB decoder in two-wire mode (tx=%d, rx=%d)",
                  this->master_channel_idx_, this->slave_channel_idx_);
  } else if (single_wire) {
    if (!this->configure_channel_(&this->ch_[0], this->single_channel_idx_,
                                  ChannelRole::AUTO, "mdb_to")) {
      this->mark_failed();
      return;
    }
    this->num_channels_ = 1;
    ESP_LOGCONFIG(TAG, "MDB decoder in single-wire mode (channel=%d)",
                  this->single_channel_idx_);
  } else {
    ESP_LOGE(TAG, "Must configure either 'channel' or both 'tx_channel' and 'rx_channel'");
    this->mark_failed();
    return;
  }
}

void PulseInspectorMdb::dump_config() {
  ESP_LOGCONFIG(TAG, "pulse_inspector_mdb:");
  if (this->num_channels_ == 2) {
    ESP_LOGCONFIG(TAG, "  Mode: two-wire (separate TX/RX)");
    ESP_LOGCONFIG(TAG, "  TX (master) channel: %d", this->ch_[0].channel_idx);
    ESP_LOGCONFIG(TAG, "  RX (slave)  channel: %d", this->ch_[1].channel_idx);
  } else {
    ESP_LOGCONFIG(TAG, "  Mode: single-wire");
    ESP_LOGCONFIG(TAG, "  Channel: %d", this->ch_[0].channel_idx);
  }
  ESP_LOGCONFIG(TAG, "  Baud: %u (bit = %.2f us, inter-byte timeout = %u us)",
                (unsigned) this->baud_, this->bit_us_,
                (unsigned) this->inter_byte_timeout_us_);
  ESP_LOGCONFIG(TAG, "  Log slave ACK echoes: %s", this->log_slave_ack_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Log raw frames:       %s", this->log_raw_frames_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Suppress idle polls:  %s", this->suppress_idle_polls_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Selection names:      %u", (unsigned) this->selections_.size());
  ESP_LOGCONFIG(TAG, "  Event queue capacity: %u", (unsigned) EVENT_QUEUE_LEN);
}

void PulseInspectorMdb::loop() {
  if (this->mutex_ == nullptr || this->is_failed()) {
    return;
  }
  const uint32_t now_ms = millis();

  // 1. Finalize any per-channel frames that the esp_timer flagged as
  //    timed-out, plus flush stale master/slave frame buffers. The
  //    timer callback only sets a flag (small stack); the real work
  //    happens here on the main task where stack is ample.
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

  // 2. Drain the event queue into the main-thread publisher. This is
  //    where sensors get updated, triggers fire, and log lines are
  //    printed -- all on the ESPHome main loop, where it's safe.
  this->drain_event_queue_();

  if (now_ms - this->last_diag_log_ms_ >= DIAG_LOG_INTERVAL_MS) {
    this->last_diag_log_ms_ = now_ms;

    const uint32_t delta = this->frames_decoded_ - this->prev_frames_decoded_;
    this->prev_frames_decoded_ = this->frames_decoded_;

    ESP_LOGD(TAG,
             "frames=%u (+%u in last %u s), master=%u, slave=%u, framing_err=%u, "
             "propr=%u, events=%u (dropped=%u), bills_stack/esc/ret/rej=%u/%u/%u/%u, "
             "sale_cycles=%u, sale_in_progress=%u",
             (unsigned) this->frames_decoded_,
             (unsigned) delta,
             (unsigned) (DIAG_LOG_INTERVAL_MS / 1000),
             (unsigned) this->master_bytes_,
             (unsigned) this->slave_bytes_,
             (unsigned) this->framing_errors_,
             (unsigned) this->proprietary_master_frames_,
             (unsigned) this->events_logged_,
             (unsigned) this->events_dropped_,
             (unsigned) this->bv_bills_stacked_,
             (unsigned) this->bv_bills_escrowed_,
             (unsigned) this->bv_bills_returned_,
             (unsigned) this->bv_bills_rejected_,
             (unsigned) this->sale_cycles_total_,
             (unsigned) (this->sale_cycle_in_progress_ ? 1 : 0));

#ifdef USE_SENSOR
    if (this->sens_frames_decoded_) this->sens_frames_decoded_->publish_state((float) this->frames_decoded_);
    if (this->sens_framing_errors_) this->sens_framing_errors_->publish_state((float) this->framing_errors_);
    if (this->sens_vend_success_) this->sens_vend_success_->publish_state((float) this->vend_success_count_);
    if (this->sens_vend_failure_) this->sens_vend_failure_->publish_state((float) this->vend_failure_count_);
    if (this->sens_master_bytes_) this->sens_master_bytes_->publish_state((float) this->master_bytes_);
    if (this->sens_slave_bytes_) this->sens_slave_bytes_->publish_state((float) this->slave_bytes_);
    if (this->sens_events_dropped_) this->sens_events_dropped_->publish_state((float) this->events_dropped_);
    if (this->sens_proprietary_frames_) this->sens_proprietary_frames_->publish_state((float) this->proprietary_master_frames_);
    // Keep BV counters ticking even if no bill event fires during the
    // interval -- useful the first time HA subscribes and for
    // statistics after a restart.
    if (this->sens_bills_stacked_) this->sens_bills_stacked_->publish_state((float) this->bv_bills_stacked_);
    if (this->sens_bills_escrowed_) this->sens_bills_escrowed_->publish_state((float) this->bv_bills_escrowed_);
    if (this->sens_bills_returned_) this->sens_bills_returned_->publish_state((float) this->bv_bills_returned_);
    if (this->sens_bills_rejected_) this->sens_bills_rejected_->publish_state((float) this->bv_bills_rejected_);
    if (this->sens_sale_cycles_total_) this->sens_sale_cycles_total_->publish_state((float) this->sale_cycles_total_);
#endif
#ifdef USE_BINARY_SENSOR
    // Periodic re-assert so the binary sensor is never left "unknown"
    // between events.
    if (this->bs_sale_cycle_in_progress_) {
      this->bs_sale_cycle_in_progress_->publish_state(this->sale_cycle_in_progress_);
    }
#endif
  }
}

// ===========================================================================
// UART layer
// ===========================================================================

void PulseInspectorMdb::safety_timeout_cb_(void *arg) {
  // IMPORTANT: this runs on esp_timer_task, whose stack is small
  // (CONFIG_ESP_TIMER_TASK_STACK_SIZE, default 3584 B). We absolutely
  // cannot do the full finalize path here -- snprintf and the MDB
  // semantic decoders alone can eat >1 KB of stack. Instead, just set
  // a flag and let loop() (which runs on the main task with a much
  // larger stack) pick it up on its next iteration.
  auto *ta = static_cast<TimeoutArg *>(arg);
  if (ta == nullptr || ta->ctx == nullptr) {
    return;
  }
  ta->ctx->timeout_pending = true;
}

void PulseInspectorMdb::on_pulse(ChannelCtx *ctx,
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

void PulseInspectorMdb::fill_bits_until_(ChannelCtx *ctx, int upper) {
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

void PulseInspectorMdb::on_edge_locked_(ChannelCtx *ctx, uint32_t t_us,
                                           bool level) {
  if (ctx->state == ChannelCtx::State::FRAME) {
    const uint32_t elapsed = t_us - ctx->frame_start_us;
    // When an edge arrives >=11 bit-times after frame_start it belongs to
    // the NEXT frame's start bit, so we want the else-branch (finalize
    // current frame, then fall through to start a new one). Using
    // floor(11 * bit_us) gives the correct boundary behaviour: at exactly
    // 11 bit-times the condition below is false and we finalize.
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

void PulseInspectorMdb::finalize_frame_locked_(ChannelCtx *ctx) {
  this->fill_bits_until_(ctx, 10);
  this->emit_byte_locked_(ctx, ctx->frame_value, ctx->stop_bit_seen);
  ctx->state = ChannelCtx::State::IDLE;
}

void PulseInspectorMdb::emit_byte_locked_(ChannelCtx *ctx, uint16_t value,
                                             bool stop_ok) {
  this->frames_decoded_++;
  if (!stop_ok) {
    this->framing_errors_++;
  }

  const uint8_t data = (uint8_t) (value & 0xFFu);
  const bool mode = ((value >> 8) & 1u) != 0u;

  bool is_master;
  if (ctx->role == ChannelRole::MASTER) {
    is_master = true;
  } else if (ctx->role == ChannelRole::SLAVE) {
    is_master = false;
  } else {
    is_master = mode;
  }

  // Timestamp the byte with its WIRE completion time (start-of-frame +
  // 11 bit-times), NOT the current wall clock. Rationale:
  // when the byte is finalized via the safety timer + loop() path
  // (typical for the last byte of an MDB command, since there is >=100 ms
  // of idle after it), emit_byte_locked_ runs on the main task and the
  // wall clock can easily be tens of milliseconds past the real byte end
  // (WiFi / API stacks etc). Using wall clock here would make the
  // inter-byte timeout check in push_master_byte_ / push_slave_byte_
  // fire against the previous byte that was emitted on time from the
  // channel task, and the preceding multi-byte command would be split
  // into per-byte frames.
  const uint32_t wire_end_us =
      ctx->frame_start_us + (uint32_t) (11.0f * this->bit_us_);

  if (is_master) {
    this->master_bytes_++;
    this->push_master_byte_(data, mode, wire_end_us);
  } else {
    this->slave_bytes_++;
    this->push_slave_byte_(data, mode, wire_end_us);
  }
}

// ===========================================================================
// Frame layer -- buffer bytes into complete MDB frames
// ===========================================================================

void PulseInspectorMdb::push_master_byte_(uint8_t data, bool mode,
                                             uint32_t now_us) {
  // A new master command starts on a mode=1 byte. If we were still buffering
  // the previous command, flush it first.
  if (mode && this->master_frame_.active) {
    this->flush_master_locked_(now_us);
  }

  // Stale buffer (previous byte was too long ago) -> flush.
  if (this->master_frame_.active &&
      (now_us - this->master_frame_.last_byte_us) > this->inter_byte_timeout_us_) {
    this->flush_master_locked_(now_us);
  }

  // In two-wire mode the master wire never sees mode=1 mid-stream EXCEPT
  // as a new command. In single-wire AUTO mode, the first byte of a slave
  // reply is mode=0 so push_master_byte_ won't be called for it.
  if (!this->master_frame_.active) {
    this->master_frame_.active = true;
    this->master_frame_.len = 0;
    this->master_frame_.first_byte_us = now_us;
  }

  if (this->master_frame_.len >= FrameBuf::CAPACITY) {
    // Overflow -> flush what we have and restart.
    this->flush_master_locked_(now_us);
    this->master_frame_.active = true;
    this->master_frame_.first_byte_us = now_us;
  }

  const size_t i = this->master_frame_.len++;
  this->master_frame_.buf[i] = data;
  this->master_frame_.mode[i] = mode ? 1u : 0u;
  this->master_frame_.last_byte_us = now_us;
}

void PulseInspectorMdb::push_slave_byte_(uint8_t data, bool mode,
                                            uint32_t now_us) {
  // A peripheral never transmits while the master is still sending, so the
  // arrival of a slave byte is a hard boundary: any master frame that's
  // still sitting in the buffer waiting for its inter-byte timeout is, by
  // definition, complete. Flush it NOW so that decode_master_frame_ runs
  // and updates last_master_was_idle_poll_ / last_idle_poll_us_ BEFORE
  // decode_slave_frame_ gets to look at them.
  //
  // Without this flush-on-slave-arrival, the mode=1 "end of reply" marker
  // on a single-byte ACK causes the slave frame to be flushed immediately
  // (see below), but the master frame behind it is still pending -- so
  // the ACK correlation check against a stale `last_idle_poll_us_` from
  // the PREVIOUS cycle (~300 ms ago) always fails the 10 ms window, and
  // every ACK leaks through unsuppressed.
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
  this->slave_frame_.mode[i] = mode ? 1u : 0u;
  this->slave_frame_.last_byte_us = now_us;

  // Some peripherals set mode=1 on the CHK byte to mark end-of-reply.
  // If we see it, flush immediately.
  if (mode) {
    this->flush_slave_locked_(now_us);
  }
}

void PulseInspectorMdb::flush_master_locked_(uint32_t now_us) {
  if (!this->master_frame_.active || this->master_frame_.len == 0) {
    this->master_frame_.active = false;
    this->master_frame_.len = 0;
    return;
  }
  FrameBuf snapshot = this->master_frame_;
  this->master_frame_.active = false;
  this->master_frame_.len = 0;
  (void) now_us;
  this->decode_master_frame_(snapshot);
}

void PulseInspectorMdb::flush_slave_locked_(uint32_t now_us) {
  if (!this->slave_frame_.active || this->slave_frame_.len == 0) {
    this->slave_frame_.active = false;
    this->slave_frame_.len = 0;
    return;
  }
  FrameBuf snapshot = this->slave_frame_;
  this->slave_frame_.active = false;
  this->slave_frame_.len = 0;
  (void) now_us;
  this->decode_slave_frame_(snapshot);
}

bool PulseInspectorMdb::split_chk_(const FrameBuf &f, bool *chk_ok,
                                      size_t *payload_len) {
  if (f.len < 2) {
    *chk_ok = true;
    *payload_len = f.len;
    return false;
  }
  uint8_t sum = 0;
  for (size_t i = 0; i + 1 < f.len; i++) {
    sum = (uint8_t) (sum + f.buf[i]);
  }
  if (sum == f.buf[f.len - 1]) {
    *chk_ok = true;
    *payload_len = f.len - 1;
    return true;
  }
  *chk_ok = false;
  *payload_len = f.len;  // treat as-is
  return false;
}

// Returns true when `base` (an MDB address, i.e. the high 5 bits of the
// first command byte) is a valid NAMA-assigned peripheral class. Anything
// else is either reserved or used by manufacturers (MEI, Jofemar, ...) for
// proprietary side-channel messages that are not part of NAMA MDB 4.x.
static bool is_nama_peripheral_base_(uint8_t base) {
  switch (base) {
    case MDB_ADDR_VMC:          // 0x00 (not a peripheral but legal on wire)
    case MDB_ADDR_CHANGER:      // 0x08
    case MDB_ADDR_CASHLESS1:    // 0x10
    case MDB_ADDR_COMMS:        // 0x18
    case MDB_ADDR_DISPLAY:      // 0x20
    case MDB_ADDR_ENERGY:       // 0x28
    case MDB_ADDR_BV:           // 0x30
    case MDB_ADDR_USD1:         // 0x40
    case MDB_ADDR_AGE:          // 0x48
    case MDB_ADDR_USD2:         // 0x58
    case MDB_ADDR_CASHLESS2:    // 0x60
    case MDB_ADDR_EXPERIMENTAL: // 0x70
    case 0x78:                  // reserved/experimental upper slot
      return true;
    default:
      return false;
  }
}

bool PulseInspectorMdb::is_idle_poll_frame_(const FrameBuf &f) {
  if (f.len == 0) return false;
  if (f.mode[0] != 1) return false;

  // --- Case A: proprietary / out-of-spec fragments --------------------
  // Single-byte master frames with mode=1 only happen when the host
  // transmits two address bytes back-to-back with a gap > inter_byte_
  // timeout. That is characteristic of MEI's proprietary side-channel
  // (e.g. coin-mech <-> BV keepalives at addresses 0xC0 / 0xF0 / 0xF8 on
  // Necta/Crane combos). Hide them: they are harmless, carry no info we
  // can decode, and would otherwise flood the log and the "Last event"
  // text sensor with `master ? cmd (0xC6, 1 bytes)` style lines.
  if (f.len == 1) {
    return true;
  }

  // --- Case B: any frame whose base address is NOT NAMA-assigned -----
  // Two-byte (or longer) frames at non-NAMA base addresses are the
  // second half of the same proprietary chatter ("33 F3 FC -> 00" and
  // friends). Standard NAMA peripherals always live in the small fixed
  // set of bases above, so this filter never hides legitimate traffic.
  const uint8_t base = f.buf[0] & 0xF8u;
  if (!is_nama_peripheral_base_(base)) {
    return true;
  }

  // --- Case C (original logic): idle POLL / discovery RESET ----------
  // Must be a complete, checksum-correct 2-byte command that starts on
  // an ADDRESS byte (mode=1). Any deviation (short frame, bad CHK, wrong
  // mode bit) means "show it" -- those cases are exactly the anomalies
  // the user wants to keep visible.
  if (f.len != 2) return false;
  if (f.buf[0] != f.buf[1]) return false;  // CHK == cmd for no-data cmds

  const uint8_t cmd = f.buf[0];
  const uint8_t addr = cmd & 0xF8u;
  const uint8_t sub = cmd & 0x07u;

  // POLL commands: these are the per-cycle heartbeat every VMC sends to
  // every configured peripheral. The sub-command code differs between
  // Cashless (0x02) and the Changer/BV/etc. family (0x03).
  const bool is_poll =
      ((addr == MDB_ADDR_CASHLESS1 || addr == MDB_ADDR_CASHLESS2) &&
       sub == MDB_CL_POLL) ||
      ((addr == MDB_ADDR_CHANGER || addr == MDB_ADDR_BV ||
        addr == MDB_ADDR_COMMS || addr == MDB_ADDR_DISPLAY ||
        addr == MDB_ADDR_ENERGY || addr == MDB_ADDR_USD1 ||
        addr == MDB_ADDR_USD2 || addr == MDB_ADDR_AGE) &&
       sub == MDB_CHG_POLL);
  if (is_poll) return true;

  // Discovery RESETs to peripherals that are typically absent. When the
  // device is present it answers RESET once at boot and never gets RESET
  // again in steady state; when it's absent, the VMC keeps probing. We
  // suppress those probes too. BV / Changer RESETs are intentionally NOT
  // in this list -- those devices are always present in this machine and
  // an unexpected RESET on them is meaningful.
  if (sub == 0x00u) {
    switch (addr) {
      case MDB_ADDR_CASHLESS1:
      case MDB_ADDR_CASHLESS2:
      case MDB_ADDR_COMMS:
      case MDB_ADDR_DISPLAY:
      case MDB_ADDR_ENERGY:
      case MDB_ADDR_USD1:
      case MDB_ADDR_USD2:
      case MDB_ADDR_AGE:
        return true;
      default:
        return false;
    }
  }

  return false;
}

// ===========================================================================
// Semantic layer -- interpret buffered frames against NAMA MDB
// ===========================================================================

void PulseInspectorMdb::decode_master_frame_(const FrameBuf &f) {
  if (f.len == 0) return;
  const uint8_t first = f.buf[0];
  const uint8_t base = first & 0xF8u;

  // Remember this as the last master command, for slave-reply correlation.
  // (master_frame_ itself was already cleared by the caller — use the
  // snapshot `f` that was passed in.)
  this->last_master_cmd_ = (uint16_t) ((f.mode[0] << 8) | first);
  this->last_master_us_ = f.first_byte_us;

  // Dedup: the same single-byte POLL repeats forever. Log raw frames for
  // variety or the first occurrence only.
  const bool is_new_master = (first != (uint8_t) (this->last_logged_master_ & 0xFFu));
  if (is_new_master) {
    this->last_logged_master_ = (uint16_t) first;
  }

  // Classify the frame and remember it, so the next slave [1] 00 ACK can
  // be correlated and suppressed too when `suppress_idle_polls_` is on.
  const bool is_idle_poll = is_idle_poll_frame_(f);
  this->last_master_was_idle_poll_ = is_idle_poll;
  this->last_idle_poll_us_ = f.first_byte_us;

  if (this->log_raw_frames_ &&
      !(this->suppress_idle_polls_ && is_idle_poll)) {
    char hex[3 * FrameBuf::CAPACITY + 1];
    char *p = hex;
    for (size_t i = 0; i < f.len && (size_t) (p - hex) < sizeof(hex) - 4; i++) {
      p += std::snprintf(p, (size_t) (hex + sizeof(hex) - p), "%02X ", f.buf[i]);
    }
    ESP_LOGD(TAG, "master [%u] %s", (unsigned) f.len, hex);
  }

  switch (base) {
    case MDB_ADDR_CASHLESS1:
    case MDB_ADDR_CASHLESS2:
      this->decode_cashless_master_(f);
      break;
    case MDB_ADDR_BV:
      this->decode_bv_master_(f);
      break;
    case MDB_ADDR_CHANGER:
      this->decode_changer_master_(f);
      break;
    default:
      // A non-NAMA base address here is manufacturer-proprietary chatter
      // (see is_idle_poll_frame_ / is_nama_peripheral_base_ for context).
      // Count it separately so the user can see the rate on the sensor,
      // but don't emit a RAW_MASTER_FRAME event when suppression is on --
      // that would flood the "MDB: Last event" text_sensor with
      // meaningless `master ? cmd (0xC6, 1 bytes)` lines.
      if (this->suppress_idle_polls_ && is_idle_poll) {
        this->proprietary_master_frames_++;
        break;
      }
      if (is_new_master) {
        MdbEvent evt;
        evt.kind = MdbEvent::RAW_MASTER_FRAME;
        evt.timestamp_us = f.first_byte_us;
        evt.byte_code = first;
        char tmp[96];
        std::snprintf(tmp, sizeof(tmp), "master %s %s (0x%02X, %u bytes)",
                      mdb_address_name(base),
                      mdb_subcommand_name(base, first & 0x07u), first,
                      (unsigned) f.len);
        set_desc_(evt, tmp);
        this->enqueue_event_(evt);
      }
      break;
  }
}

void PulseInspectorMdb::decode_slave_frame_(const FrameBuf &f) {
  if (f.len == 0) return;

  // Suppress the "[1] 00" ACK that completes an idle POLL / discovery
  // RESET cycle. Only short-circuit when the last master frame was
  // classified as idle AND this reply arrived within the normal MDB
  // response window (t_response <= 5 ms per NAMA spec; we use a slightly
  // looser 10 ms budget to cover jitter from the sniffer pipeline).
  const bool is_ack = (f.len == 1 && f.buf[0] == MDB_ACK);
  const uint32_t since_master_us = f.first_byte_us - this->last_idle_poll_us_;
  const bool ack_after_idle_poll =
      is_ack && this->last_master_was_idle_poll_ && since_master_us <= 10000u;

  if (this->log_raw_frames_ &&
      !(this->suppress_idle_polls_ && ack_after_idle_poll)) {
    char hex[3 * FrameBuf::CAPACITY + 1];
    char *p = hex;
    for (size_t i = 0; i < f.len && (size_t) (p - hex) < sizeof(hex) - 4; i++) {
      p += std::snprintf(p, (size_t) (hex + sizeof(hex) - p), "%02X ", f.buf[i]);
    }
    ESP_LOGD(TAG, "slave  [%u] %s", (unsigned) f.len, hex);
  }

  // Single-byte short replies.
  if (f.len == 1) {
    const uint8_t b = f.buf[0];
    if (b == MDB_ACK) {
      if (this->log_slave_ack_) {
        ESP_LOGD(TAG, "slave ACK");
      }
      return;
    }
    if (b == MDB_NAK) {
      MdbEvent evt;
      evt.kind = MdbEvent::RAW_SLAVE_FRAME;
      evt.timestamp_us = f.first_byte_us;
      evt.byte_code = b;
      set_desc_(evt, "slave NAK (retransmit requested)");
      this->enqueue_event_(evt);
      return;
    }
    if (b == MDB_PNAK) {
      // "Peripheral NAK" / just-reset indicator.
      MdbEvent evt;
      evt.kind = MdbEvent::RAW_SLAVE_FRAME;
      evt.timestamp_us = f.first_byte_us;
      evt.byte_code = b;
      set_desc_(evt, "slave PNAK (just-reset / not ready)");
      this->enqueue_event_(evt);
      return;
    }
  }

  // Multi-byte reply: route based on what the last master command was.
  const uint8_t last_first = (uint8_t) (this->last_master_cmd_ & 0xFFu);
  const uint8_t last_base = last_first & 0xF8u;
  switch (last_base) {
    case MDB_ADDR_CASHLESS1:
    case MDB_ADDR_CASHLESS2:
      this->decode_cashless_slave_(f);
      return;
    case MDB_ADDR_BV:
      this->decode_bv_slave_(f);
      return;
    case MDB_ADDR_CHANGER:
      this->decode_changer_slave_(f);
      return;
    default:
      break;
  }

  // Unknown context.
  MdbEvent evt;
  evt.kind = MdbEvent::RAW_SLAVE_FRAME;
  evt.timestamp_us = f.first_byte_us;
  char tmp[64];
  std::snprintf(tmp, sizeof(tmp), "slave %u bytes (no master context)",
                (unsigned) f.len);
  set_desc_(evt, tmp);
  this->enqueue_event_(evt);
}

// ----- Cashless ------------------------------------------------------------

uint32_t PulseInspectorMdb::cashless_price_to_cents_(uint16_t raw) const {
  uint32_t v = (uint32_t) raw * (uint32_t) this->scale_.cashless_scale;
  // If decimal places > 2 we'd lose precision, but cents is our unit.
  // Example: scale=1, decimals=2 -> raw=150 -> 150 cents (1.50 EUR).
  //          scale=5, decimals=2 -> raw=30  -> 150 cents.
  switch (this->scale_.cashless_decimal_places) {
    case 0: v *= 100u; break;
    case 1: v *= 10u; break;
    case 2: /* already cents */ break;
    case 3: v /= 10u; break;
    default: break;
  }
  return v;
}

void PulseInspectorMdb::decode_cashless_master_(const FrameBuf &f) {
  if (f.len == 0) return;
  const uint8_t first = f.buf[0];
  const uint8_t sub = first & 0x07u;

  bool chk_ok = false;
  size_t payload_len = f.len;
  split_chk_(f, &chk_ok, &payload_len);

  switch (sub) {
    case MDB_CL_POLL:
      // POLL has no payload. Expected; no master-side event.
      return;
    case MDB_CL_VEND: {
      if (payload_len < 2) return;
      const uint8_t vend_sub = f.buf[1];
      switch (vend_sub) {
        case MDB_CL_VEND_REQUEST:
        case MDB_CL_VEND_CASH_SALE:
        case MDB_CL_VEND_NEGATIVE: {
          if (payload_len < 6) return;
          const uint16_t price_raw = (uint16_t) ((f.buf[2] << 8) | f.buf[3]);
          const uint16_t item = (uint16_t) ((f.buf[4] << 8) | f.buf[5]);
          MdbEvent evt;
          evt.kind = (vend_sub == MDB_CL_VEND_CASH_SALE)
                         ? MdbEvent::CASH_SALE
                         : MdbEvent::VEND_REQUEST;
          evt.timestamp_us = f.first_byte_us;
          evt.price_cents = this->cashless_price_to_cents_(price_raw);
          evt.item_number = item;
          const char *sel = this->lookup_selection_name_(item);
          char tmp[96];
          if (vend_sub == MDB_CL_VEND_CASH_SALE) {
            std::snprintf(tmp, sizeof(tmp), "CASH SALE price=%u cents item=%u%s%s",
                          (unsigned) evt.price_cents, (unsigned) item,
                          sel ? " (" : "", sel ? sel : "");
          } else if (vend_sub == MDB_CL_VEND_NEGATIVE) {
            std::snprintf(tmp, sizeof(tmp),
                          "NEGATIVE VEND REQUEST price=%u cents item=%u%s%s",
                          (unsigned) evt.price_cents, (unsigned) item,
                          sel ? " (" : "", sel ? sel : "");
          } else {
            std::snprintf(tmp, sizeof(tmp),
                          "VEND REQUEST price=%u cents item=%u%s%s",
                          (unsigned) evt.price_cents, (unsigned) item,
                          sel ? " (" : "", sel ? sel : "");
          }
          if (sel) {
            size_t l = std::strlen(tmp);
            if (l + 2 < sizeof(tmp)) std::strcat(tmp, ")");
          }
          set_desc_(evt, tmp);
          this->enqueue_event_(evt);
          return;
        }
        case MDB_CL_VEND_CANCEL: {
          MdbEvent evt;
          evt.kind = MdbEvent::RAW_MASTER_FRAME;
          evt.timestamp_us = f.first_byte_us;
          set_desc_(evt, "VEND CANCEL");
          this->enqueue_event_(evt);
          return;
        }
        case MDB_CL_VEND_SUCCESS: {
          if (payload_len < 4) return;
          const uint16_t item = (uint16_t) ((f.buf[2] << 8) | f.buf[3]);
          MdbEvent evt;
          evt.kind = MdbEvent::VEND_SUCCESS;
          evt.timestamp_us = f.first_byte_us;
          evt.item_number = item;
          const char *sel = this->lookup_selection_name_(item);
          char tmp[96];
          std::snprintf(tmp, sizeof(tmp), "VEND SUCCESS item=%u%s%s%s",
                        (unsigned) item,
                        sel ? " (" : "", sel ? sel : "", sel ? ")" : "");
          set_desc_(evt, tmp);
          this->enqueue_event_(evt);
          return;
        }
        case MDB_CL_VEND_FAILURE: {
          MdbEvent evt;
          evt.kind = MdbEvent::VEND_FAILURE;
          evt.timestamp_us = f.first_byte_us;
          // Keep the item number from the last VEND_REQUEST we saw.
          set_desc_(evt, "VEND FAILURE (drink not dispensed)");
          this->enqueue_event_(evt);
          return;
        }
        case MDB_CL_VEND_SESSION_COMPLETE: {
          MdbEvent evt;
          evt.kind = MdbEvent::END_SESSION;
          evt.timestamp_us = f.first_byte_us;
          set_desc_(evt, "SESSION COMPLETE (VMC initiated)");
          this->enqueue_event_(evt);
          return;
        }
        default:
          return;
      }
    }
    case MDB_CL_READER: {
      if (payload_len < 2) return;
      const uint8_t reader_sub = f.buf[1];
      MdbEvent evt;
      evt.kind = MdbEvent::RAW_MASTER_FRAME;
      evt.timestamp_us = f.first_byte_us;
      char tmp[64];
      if (reader_sub == 0x00) {
        std::snprintf(tmp, sizeof(tmp), "READER DISABLE");
      } else if (reader_sub == 0x01) {
        std::snprintf(tmp, sizeof(tmp), "READER ENABLE");
      } else if (reader_sub == 0x02) {
        std::snprintf(tmp, sizeof(tmp), "READER CANCEL");
      } else {
        std::snprintf(tmp, sizeof(tmp), "READER sub=0x%02X", reader_sub);
      }
      set_desc_(evt, tmp);
      this->enqueue_event_(evt);
      return;
    }
    default:
      return;
  }
}

void PulseInspectorMdb::decode_cashless_slave_(const FrameBuf &f) {
  if (f.len == 0) return;
  const uint8_t last_master_sub =
      (uint8_t) (this->last_master_cmd_ & 0x07u);

  bool chk_ok = false;
  size_t payload_len = f.len;
  split_chk_(f, &chk_ok, &payload_len);

  // Cashless SETUP Config Data response: [0x01, FeatureLevel, Country H,
  // Country L, ScaleFactor, DecimalPlaces, MaxResponseTime, MiscOptions].
  if (last_master_sub == MDB_CL_SETUP && payload_len >= 6 &&
      f.buf[0] == MDB_CL_REPLY_CONFIG) {
    this->scale_.cashless_scale = f.buf[4];
    this->scale_.cashless_decimal_places = f.buf[5];
    MdbEvent evt;
    evt.kind = MdbEvent::RAW_SLAVE_FRAME;
    evt.timestamp_us = f.first_byte_us;
    char tmp[80];
    std::snprintf(tmp, sizeof(tmp),
                  "CONFIG: feature=L%u country=%02X%02X scale=%u decimals=%u",
                  (unsigned) f.buf[1], (unsigned) f.buf[2], (unsigned) f.buf[3],
                  (unsigned) f.buf[4], (unsigned) f.buf[5]);
    set_desc_(evt, tmp);
    this->enqueue_event_(evt);
    return;
  }

  // POLL reply: can carry one or more status codes. Iterate.
  if (last_master_sub == MDB_CL_POLL) {
    size_t i = 0;
    while (i < payload_len) {
      const uint8_t code = f.buf[i];
      switch (code) {
        case MDB_CL_REPLY_JUST_RESET: {
          MdbEvent evt;
          evt.kind = MdbEvent::CL_RESET;
          evt.timestamp_us = f.first_byte_us;
          set_desc_(evt, "CASHLESS JUST RESET");
          this->enqueue_event_(evt);
          i += 1;
          break;
        }
        case MDB_CL_REPLY_BEGIN_SESSION: {
          if (i + 2 >= payload_len) { i = payload_len; break; }
          const uint16_t funds_raw = (uint16_t) ((f.buf[i + 1] << 8) | f.buf[i + 2]);
          MdbEvent evt;
          evt.kind = MdbEvent::BEGIN_SESSION;
          evt.timestamp_us = f.first_byte_us;
          evt.funds_cents = this->cashless_price_to_cents_(funds_raw);
          char tmp[64];
          std::snprintf(tmp, sizeof(tmp), "BEGIN SESSION funds=%u cents",
                        (unsigned) evt.funds_cents);
          set_desc_(evt, tmp);
          this->enqueue_event_(evt);
          i += 3;
          break;
        }
        case MDB_CL_REPLY_SESSION_CANCEL_REQ: {
          MdbEvent evt;
          evt.kind = MdbEvent::SESSION_CANCEL_REQUEST;
          evt.timestamp_us = f.first_byte_us;
          set_desc_(evt, "SESSION CANCEL REQUEST");
          this->enqueue_event_(evt);
          i += 1;
          break;
        }
        case MDB_CL_REPLY_VEND_APPROVED: {
          if (i + 2 >= payload_len) { i = payload_len; break; }
          const uint16_t price_raw = (uint16_t) ((f.buf[i + 1] << 8) | f.buf[i + 2]);
          MdbEvent evt;
          evt.kind = MdbEvent::VEND_APPROVED;
          evt.timestamp_us = f.first_byte_us;
          evt.price_cents = this->cashless_price_to_cents_(price_raw);
          char tmp[64];
          std::snprintf(tmp, sizeof(tmp), "VEND APPROVED price=%u cents",
                        (unsigned) evt.price_cents);
          set_desc_(evt, tmp);
          this->enqueue_event_(evt);
          i += 3;
          break;
        }
        case MDB_CL_REPLY_VEND_DENIED: {
          MdbEvent evt;
          evt.kind = MdbEvent::VEND_DENIED;
          evt.timestamp_us = f.first_byte_us;
          set_desc_(evt, "VEND DENIED");
          this->enqueue_event_(evt);
          i += 1;
          break;
        }
        case MDB_CL_REPLY_END_SESSION: {
          MdbEvent evt;
          evt.kind = MdbEvent::END_SESSION;
          evt.timestamp_us = f.first_byte_us;
          set_desc_(evt, "END SESSION");
          this->enqueue_event_(evt);
          i += 1;
          break;
        }
        case MDB_CL_REPLY_CANCELLED: {
          MdbEvent evt;
          evt.kind = MdbEvent::RAW_SLAVE_FRAME;
          evt.timestamp_us = f.first_byte_us;
          set_desc_(evt, "CASHLESS CANCELLED");
          this->enqueue_event_(evt);
          i += 1;
          break;
        }
        case MDB_CL_REPLY_MALFUNCTION: {
          uint8_t err = 0;
          if (i + 1 < payload_len) err = f.buf[i + 1];
          MdbEvent evt;
          evt.kind = MdbEvent::CL_MALFUNCTION;
          evt.timestamp_us = f.first_byte_us;
          evt.byte_code = err;
          char tmp[64];
          std::snprintf(tmp, sizeof(tmp), "CASHLESS MALFUNCTION code=0x%02X", err);
          set_desc_(evt, tmp);
          this->enqueue_event_(evt);
          i += 2;
          break;
        }
        case MDB_CL_REPLY_CMD_OUT_OF_SEQ: {
          MdbEvent evt;
          evt.kind = MdbEvent::RAW_SLAVE_FRAME;
          evt.timestamp_us = f.first_byte_us;
          set_desc_(evt, "CASHLESS CMD OUT OF SEQUENCE");
          this->enqueue_event_(evt);
          i += 1;
          break;
        }
        case MDB_CL_REPLY_DIAGNOSTIC: {
          MdbEvent evt;
          evt.kind = MdbEvent::CL_DIAGNOSTIC;
          evt.timestamp_us = f.first_byte_us;
          // Payload is vendor-specific; include length.
          char tmp[64];
          std::snprintf(tmp, sizeof(tmp), "CASHLESS DIAGNOSTIC (+%u bytes)",
                        (unsigned) (payload_len - i - 1));
          set_desc_(evt, tmp);
          this->enqueue_event_(evt);
          i = payload_len;
          break;
        }
        default: {
          // Unknown reply code, stop iterating to avoid mis-parsing.
          i = payload_len;
          break;
        }
      }
    }
    return;
  }

  // Other contexts: log as raw.
  MdbEvent evt;
  evt.kind = MdbEvent::RAW_SLAVE_FRAME;
  evt.timestamp_us = f.first_byte_us;
  char tmp[64];
  std::snprintf(tmp, sizeof(tmp), "Cashless reply (%u bytes, ctx=0x%02X)",
                (unsigned) payload_len, last_master_sub);
  set_desc_(evt, tmp);
  this->enqueue_event_(evt);
}

// ----- Bill Validator ------------------------------------------------------

void PulseInspectorMdb::decode_bv_master_(const FrameBuf &f) {
  if (f.len == 0) return;
  const uint8_t sub = f.buf[0] & 0x07u;

  bool chk_ok = false;
  size_t payload_len = f.len;
  split_chk_(f, &chk_ok, &payload_len);

  // BILL TYPE (0x34): [addr][enable_H][enable_L][escrow_H][escrow_L] CHK
  //   enable_mask != 0 -> BV is accepting bills, machine is idle / waiting
  //   enable_mask == 0 -> BV is locked by host, a sale is in progress
  // The zero/non-zero transition is our proxy for "sale cycle" boundaries
  // since the VEND command itself lives on the Executive bus we don't tap.
  if (sub == MDB_BV_BILL_TYPE && payload_len >= 5) {
    const uint16_t enable_mask =
        (uint16_t) (((uint16_t) f.buf[1] << 8) | f.buf[2]);
    const uint16_t escrow_mask =
        (uint16_t) (((uint16_t) f.buf[3] << 8) | f.buf[4]);
    const uint32_t combined =
        ((uint32_t) enable_mask << 16) | (uint32_t) escrow_mask;

    const bool changed =
        (!this->bv_enable_mask_known_) || (combined != this->bv_enable_mask_);
    if (changed) {
      const uint32_t prev_mask = this->bv_enable_mask_;
      const bool had_prev = this->bv_enable_mask_known_;
      this->bv_enable_mask_ = combined;
      this->bv_enable_mask_known_ = true;

      MdbEvent evt;
      evt.kind = MdbEvent::BV_BILL_TYPE_CMD;
      evt.timestamp_us = f.first_byte_us;
      evt.data32 = combined;
      char tmp[64];
      std::snprintf(tmp, sizeof(tmp),
                    "BV BILL_TYPE: enable=%04X escrow=%04X",
                    (unsigned) enable_mask, (unsigned) escrow_mask);
      set_desc_(evt, tmp);
      this->enqueue_event_(evt);

      // Sale-cycle transitions. Only consider the "accept" mask for this.
      const bool now_disabled = (enable_mask == 0);
      const bool was_disabled =
          had_prev && ((prev_mask >> 16) == 0);

      if (now_disabled && !was_disabled && !this->sale_cycle_in_progress_) {
        this->sale_cycle_in_progress_ = true;
        this->sale_cycle_start_us_ = f.first_byte_us;
        MdbEvent cyc;
        cyc.kind = MdbEvent::SALE_CYCLE_BEGIN;
        cyc.timestamp_us = f.first_byte_us;
        cyc.data32 = prev_mask >> 16;  // the enable mask that was in effect
        set_desc_(cyc, "Sale cycle BEGIN (BV locked by host)");
        this->enqueue_event_(cyc);
      } else if (!now_disabled && this->sale_cycle_in_progress_) {
        this->sale_cycle_in_progress_ = false;
        const uint32_t dur_us =
            f.first_byte_us - this->sale_cycle_start_us_;
        this->last_sale_duration_ms_ = dur_us / 1000u;
        this->sale_cycles_total_++;
        MdbEvent cyc;
        cyc.kind = MdbEvent::SALE_CYCLE_END;
        cyc.timestamp_us = f.first_byte_us;
        cyc.data32 = this->last_sale_duration_ms_;
        char tmp2[80];
        std::snprintf(tmp2, sizeof(tmp2),
                      "Sale cycle END (duration=%u ms, total=%u)",
                      (unsigned) this->last_sale_duration_ms_,
                      (unsigned) this->sale_cycles_total_);
        set_desc_(cyc, tmp2);
        this->enqueue_event_(cyc);
      }
    }
    return;
  }

  // ESCROW (0x35): [addr][0x00 or 0x01] CHK
  //   0x01 = STACK the escrowed bill, 0x00 = RETURN it to the user.
  if (sub == MDB_BV_ESCROW && payload_len >= 2) {
    const uint8_t cmd = f.buf[1];
    MdbEvent evt;
    evt.kind = MdbEvent::BV_ESCROW_CMD;
    evt.timestamp_us = f.first_byte_us;
    evt.byte_code = cmd;
    char tmp[48];
    std::snprintf(tmp, sizeof(tmp), "BV ESCROW: %s (0x%02X)",
                  cmd == 0x01 ? "STACK" : (cmd == 0x00 ? "RETURN" : "?"),
                  (unsigned) cmd);
    set_desc_(evt, tmp);
    this->enqueue_event_(evt);
    return;
  }

  // Other BV sub-commands (RESET, SETUP, SECURITY, POLL, STACKER,
  // EXPANSION) are handled by the raw-frame path / by the slave reply
  // (SETUP populates scale factors) and don't need their own events.
}

void PulseInspectorMdb::decode_bv_slave_(const FrameBuf &f) {
  if (f.len == 0) return;
  const uint8_t last_master_sub = (uint8_t) (this->last_master_cmd_ & 0x07u);

  bool chk_ok = false;
  size_t payload_len = f.len;
  split_chk_(f, &chk_ok, &payload_len);

  // BV SETUP response: [FeatureLevel, Country H, Country L, Scale H, Scale L,
  // Decimal Places, Stacker Cap H, Stacker Cap L, BillSecurity H,
  // BillSecurity L, Escrow, BillType credit (16 bytes)]. Scale and decimals
  // drive bill value conversion.
  if (last_master_sub == MDB_BV_SETUP && payload_len >= 11) {
    this->scale_.bv_scale = (uint16_t) ((f.buf[3] << 8) | f.buf[4]);
    this->scale_.bv_decimal_places = f.buf[5];
    MdbEvent evt;
    evt.kind = MdbEvent::RAW_SLAVE_FRAME;
    evt.timestamp_us = f.first_byte_us;
    char tmp[96];
    std::snprintf(tmp, sizeof(tmp),
                  "BV SETUP: feature=L%u country=%02X%02X scale=%u decimals=%u",
                  (unsigned) f.buf[0], (unsigned) f.buf[1], (unsigned) f.buf[2],
                  (unsigned) this->scale_.bv_scale,
                  (unsigned) this->scale_.bv_decimal_places);
    set_desc_(evt, tmp);
    this->enqueue_event_(evt);
    return;
  }

  // POLL reply: sequence of up to 16 status bytes.
  if (last_master_sub == MDB_BV_POLL) {
    for (size_t i = 0; i < payload_len; i++) {
      const uint8_t b = f.buf[i];
      if (b == 0x00) continue;  // filler / stop

      // 0x01-0x0C: fault codes.
      const char *fault = mdb_bv_status_name(b);
      if (fault != nullptr) {
        MdbEvent evt;
        evt.timestamp_us = f.first_byte_us;
        evt.byte_code = b;
        if (b == 0x06) {
          evt.kind = MdbEvent::BV_RESET;
        } else if (b == 0x09) {
          evt.kind = MdbEvent::BV_STATUS;
        } else {
          evt.kind = MdbEvent::BV_ERROR;
        }
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), "BV: %s (0x%02X)", fault, b);
        set_desc_(evt, tmp);
        this->enqueue_event_(evt);
        continue;
      }

      // 0x1n / 0x2n: stacker.
      if ((b & 0xE0u) == 0x20u || (b & 0xE0u) == 0x10u) {
        const uint16_t count = (uint16_t) (b & 0x1Fu);
        MdbEvent evt;
        evt.kind = MdbEvent::BV_STACKED;
        evt.timestamp_us = f.first_byte_us;
        evt.byte_code = b;
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), "BV stacker: %u bills%s",
                      (unsigned) count,
                      (b & 0x80u) ? " (full)" : "");
        set_desc_(evt, tmp);
        this->enqueue_event_(evt);
        continue;
      }

      // 0x8n-0x9F: bill accepted. Top bit set + nibble-encoded routing+type.
      if ((b & 0x80u) != 0u) {
        const uint8_t routing = (uint8_t) ((b >> 4) & 0x07u);  // 0..7
        const uint8_t bill_type = (uint8_t) (b & 0x0Fu);
        MdbEvent evt;
        evt.timestamp_us = f.first_byte_us;
        evt.byte_code = b;
        evt.bill_type = bill_type;
        evt.routed_to_stacker = (routing == 1);
        const char *route_name = nullptr;
        switch (routing) {
          case 0: route_name = "escrow"; break;
          case 1: route_name = "stacker"; break;
          case 2: route_name = "returned"; break;
          case 3: route_name = "rejected"; break;
          case 4: route_name = "disabled"; break;
          default: route_name = "?"; break;
        }
        if (routing == 3) {
          evt.kind = MdbEvent::BV_BILL_REJECTED;
        } else {
          evt.kind = MdbEvent::BV_BILL_ACCEPTED;
        }
        char tmp[80];
        std::snprintf(tmp, sizeof(tmp),
                      "BV: bill type=%u routing=%s (raw=0x%02X)",
                      (unsigned) bill_type, route_name, b);
        set_desc_(evt, tmp);
        this->enqueue_event_(evt);
        continue;
      }
    }
    return;
  }
}

// ----- Coin Changer --------------------------------------------------------

void PulseInspectorMdb::decode_changer_master_(const FrameBuf &f) {
  if (f.len == 0) return;
  const uint8_t sub = f.buf[0] & 0x07u;
  if (sub == MDB_CHG_DISPENSE && f.len >= 2) {
    const uint8_t arg = f.buf[1];
    const uint8_t coin_type = (uint8_t) (arg & 0x0Fu);
    const uint8_t count = (uint8_t) ((arg >> 4) & 0x0Fu);
    MdbEvent evt;
    evt.kind = MdbEvent::CHG_COIN_DISPENSED;
    evt.timestamp_us = f.first_byte_us;
    evt.coin_type = coin_type;
    evt.coin_count = count;
    char tmp[64];
    std::snprintf(tmp, sizeof(tmp),
                  "Changer DISPENSE: %u x coin type %u (command)",
                  (unsigned) count, (unsigned) coin_type);
    set_desc_(evt, tmp);
    this->enqueue_event_(evt);
  }
}

void PulseInspectorMdb::decode_changer_slave_(const FrameBuf &f) {
  if (f.len == 0) return;
  const uint8_t last_master_sub = (uint8_t) (this->last_master_cmd_ & 0x07u);

  bool chk_ok = false;
  size_t payload_len = f.len;
  split_chk_(f, &chk_ok, &payload_len);

  // Changer SETUP reply: [FeatureLevel, Country H, Country L, Scale,
  // Decimals, Coin Type Routing H, Coin Type Routing L, CoinTypeCredit (16)].
  if (last_master_sub == MDB_CHG_SETUP && payload_len >= 7) {
    this->scale_.changer_scale = f.buf[3];
    this->scale_.changer_decimal_places = f.buf[4];
    MdbEvent evt;
    evt.kind = MdbEvent::RAW_SLAVE_FRAME;
    evt.timestamp_us = f.first_byte_us;
    char tmp[96];
    std::snprintf(tmp, sizeof(tmp),
                  "Changer SETUP: feature=L%u country=%02X%02X scale=%u decimals=%u",
                  (unsigned) f.buf[0], (unsigned) f.buf[1], (unsigned) f.buf[2],
                  (unsigned) this->scale_.changer_scale,
                  (unsigned) this->scale_.changer_decimal_places);
    set_desc_(evt, tmp);
    this->enqueue_event_(evt);
    return;
  }

  // POLL reply: up to 16 status bytes.
  if (last_master_sub == MDB_CHG_POLL) {
    for (size_t i = 0; i < payload_len; i++) {
      const uint8_t b = f.buf[i];
      if (b == 0x00) continue;

      const char *fault = mdb_changer_status_name(b);
      if (fault != nullptr) {
        MdbEvent evt;
        evt.timestamp_us = f.first_byte_us;
        evt.byte_code = b;
        if (b == 0x0B) {
          evt.kind = MdbEvent::CHG_RESET;
        } else {
          evt.kind = MdbEvent::CHG_ERROR;
        }
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), "Changer: %s (0x%02X)", fault, b);
        set_desc_(evt, tmp);
        this->enqueue_event_(evt);
        continue;
      }

      // 0x2n / 0x3n: slug (invalid coin attempt). Low nibble = coin type.
      if ((b & 0xE0u) == 0x20u) {
        MdbEvent evt;
        evt.kind = MdbEvent::CHG_SLUG;
        evt.timestamp_us = f.first_byte_us;
        evt.coin_type = (uint8_t) (b & 0x1Fu);
        evt.byte_code = b;
        char tmp[48];
        std::snprintf(tmp, sizeof(tmp), "Changer slug: %u rejected",
                      (unsigned) evt.coin_type);
        set_desc_(evt, tmp);
        this->enqueue_event_(evt);
        continue;
      }

      // 0x4n-0x5n: coin deposited.
      // Format: [YYXX XXXX] where YY=routing, XXXX=type, plus next byte = count in tube.
      if ((b & 0xC0u) == 0x40u) {
        const uint8_t routing = (uint8_t) ((b >> 4) & 0x03u);
        const uint8_t coin_type = (uint8_t) (b & 0x0Fu);
        uint8_t count_in_tube = 0;
        if (i + 1 < payload_len) {
          count_in_tube = f.buf[++i];
        }
        MdbEvent evt;
        evt.kind = MdbEvent::CHG_COIN_DEPOSITED;
        evt.timestamp_us = f.first_byte_us;
        evt.coin_type = coin_type;
        evt.coin_count = count_in_tube;
        evt.routed_to_stacker = (routing == 1);
        evt.byte_code = b;
        const char *route_name = nullptr;
        switch (routing) {
          case 0: route_name = "cash-box"; break;
          case 1: route_name = "tube"; break;
          case 2: route_name = "not used"; break;
          case 3: route_name = "reject"; break;
          default: route_name = "?"; break;
        }
        char tmp[80];
        std::snprintf(tmp, sizeof(tmp),
                      "Changer coin deposited: type=%u routing=%s tube=%u",
                      (unsigned) coin_type, route_name, (unsigned) count_in_tube);
        set_desc_(evt, tmp);
        this->enqueue_event_(evt);
        continue;
      }
    }
    return;
  }

  // TUBE_STATUS reply is 18 bytes; we just emit a summary event.
  if (last_master_sub == MDB_CHG_TUBE_STATUS && payload_len >= 18) {
    MdbEvent evt;
    evt.kind = MdbEvent::CHG_TUBES_UPDATE;
    evt.timestamp_us = f.first_byte_us;
    char tmp[64];
    std::snprintf(tmp, sizeof(tmp),
                  "Changer TUBE_STATUS full=%02X%02X counts=%02X..%02X",
                  (unsigned) f.buf[0], (unsigned) f.buf[1],
                  (unsigned) f.buf[2], (unsigned) f.buf[payload_len - 1]);
    set_desc_(evt, tmp);
    this->enqueue_event_(evt);
    return;
  }
}

// ===========================================================================
// Event publishing (fan-out to log, callbacks, sensors, triggers)
// ===========================================================================

const char *PulseInspectorMdb::lookup_selection_name_(uint32_t item) const {
  for (const auto &s : this->selections_) {
    if (s.item == item) return s.name.c_str();
  }
  return nullptr;
}

void PulseInspectorMdb::enqueue_event_(const MdbEvent &evt) {
  if (this->event_queue_ == nullptr) {
    return;
  }
  // Non-blocking send: if the queue is full, we drop the event rather
  // than stall the UART task. Losing a log line is always preferable to
  // missing UART bytes.
  if (xQueueSend(this->event_queue_, &evt, 0) != pdTRUE) {
    this->events_dropped_++;
  }
}

void PulseInspectorMdb::drain_event_queue_() {
  if (this->event_queue_ == nullptr) {
    return;
  }
  MdbEvent evt;
  // Bound the per-loop drain to avoid starving other ESPHome components
  // if the queue is saturated; on the next loop() we'll process more.
  for (int guard = 0; guard < 16; guard++) {
    if (xQueueReceive(this->event_queue_, &evt, 0) != pdTRUE) {
      return;
    }
    this->publish_event_(evt);
  }
}

void PulseInspectorMdb::publish_event_(const MdbEvent &evt) {
  this->events_logged_++;

  // 1. Log
  ESP_LOGI(TAG, "%s", evt.description);

  // 2. Counters / stateful flags
  switch (evt.kind) {
    case MdbEvent::VEND_REQUEST:
    case MdbEvent::CASH_SALE:
      this->vend_in_progress_ = true;
      break;
    case MdbEvent::VEND_APPROVED:
      // still in progress, waiting for SUCCESS/FAILURE
      break;
    case MdbEvent::VEND_DENIED:
      this->vend_in_progress_ = false;
      break;
    case MdbEvent::VEND_SUCCESS:
      this->vend_success_count_++;
      this->vend_in_progress_ = false;
      break;
    case MdbEvent::VEND_FAILURE:
      this->vend_failure_count_++;
      this->vend_in_progress_ = false;
      break;
    case MdbEvent::BEGIN_SESSION:
      this->session_active_ = true;
      break;
    case MdbEvent::END_SESSION:
    case MdbEvent::SESSION_CANCEL_REQUEST:
      this->session_active_ = false;
      this->vend_in_progress_ = false;
      break;
    case MdbEvent::BV_ERROR:
      if (evt.byte_code == 0x05) this->bv_jam_state_ = true;
      if (evt.byte_code == 0x09) this->bv_disabled_state_ = true;
      break;
    case MdbEvent::BV_STATUS:
      this->bv_disabled_state_ = (evt.byte_code == 0x09);
      break;
    case MdbEvent::BV_RESET:
      this->bv_jam_state_ = false;
      this->bv_disabled_state_ = false;
      break;
    case MdbEvent::BV_BILL_ACCEPTED: {
      // byte_code carries the raw POLL status byte (0x80..0xCF).
      // Bits 6..4 encode routing: 0=escrow, 1=stacker, 2=returned,
      // 3=rejected (handled via BV_BILL_REJECTED below), 4=disabled.
      const uint8_t routing = (uint8_t) ((evt.byte_code >> 4) & 0x07u);
      switch (routing) {
        case 0: this->bv_bills_escrowed_++; break;
        case 1: this->bv_bills_stacked_++; break;
        case 2: this->bv_bills_returned_++; break;
        default: break;
      }
      this->last_bill_type_ = evt.bill_type;
      break;
    }
    case MdbEvent::BV_BILL_REJECTED:
      this->bv_bills_rejected_++;
      break;
    case MdbEvent::CHG_ERROR:
      if (evt.byte_code == 0x07 || evt.byte_code == 0x0C)
        this->changer_jam_state_ = true;
      break;
    case MdbEvent::CHG_RESET:
      this->changer_jam_state_ = false;
      break;
    case MdbEvent::CL_MALFUNCTION:
      this->cl_malfunction_state_ = true;
      break;
    case MdbEvent::CL_RESET:
      this->cl_malfunction_state_ = false;
      break;
    default:
      break;
  }

#ifdef USE_SENSOR
  if (this->sens_last_item_ && (evt.kind == MdbEvent::VEND_REQUEST ||
                                 evt.kind == MdbEvent::CASH_SALE ||
                                 evt.kind == MdbEvent::VEND_SUCCESS ||
                                 evt.kind == MdbEvent::VEND_FAILURE)) {
    if (evt.item_number != 0 || evt.kind != MdbEvent::VEND_FAILURE) {
      this->sens_last_item_->publish_state((float) evt.item_number);
    }
  }
  if (this->sens_last_price_ && (evt.kind == MdbEvent::VEND_REQUEST ||
                                  evt.kind == MdbEvent::CASH_SALE ||
                                  evt.kind == MdbEvent::VEND_APPROVED)) {
    this->sens_last_price_->publish_state(evt.price_cents / 100.0f);
  }
  if (this->sens_session_funds_ && evt.kind == MdbEvent::BEGIN_SESSION) {
    this->sens_session_funds_->publish_state(evt.funds_cents / 100.0f);
  }
  if (this->sens_vend_success_ && evt.kind == MdbEvent::VEND_SUCCESS) {
    this->sens_vend_success_->publish_state((float) this->vend_success_count_);
  }
  if (this->sens_vend_failure_ && evt.kind == MdbEvent::VEND_FAILURE) {
    this->sens_vend_failure_->publish_state((float) this->vend_failure_count_);
  }
  // BV counters: publish on the event that moved them.
  if (evt.kind == MdbEvent::BV_BILL_ACCEPTED) {
    const uint8_t routing = (uint8_t) ((evt.byte_code >> 4) & 0x07u);
    if (routing == 1 && this->sens_bills_stacked_) {
      this->sens_bills_stacked_->publish_state((float) this->bv_bills_stacked_);
    } else if (routing == 0 && this->sens_bills_escrowed_) {
      this->sens_bills_escrowed_->publish_state((float) this->bv_bills_escrowed_);
    } else if (routing == 2 && this->sens_bills_returned_) {
      this->sens_bills_returned_->publish_state((float) this->bv_bills_returned_);
    }
    if (this->sens_last_bill_type_) {
      this->sens_last_bill_type_->publish_state((float) evt.bill_type);
    }
  }
  if (evt.kind == MdbEvent::BV_BILL_REJECTED && this->sens_bills_rejected_) {
    this->sens_bills_rejected_->publish_state((float) this->bv_bills_rejected_);
  }
  // Sale-cycle sensors: fire on both boundaries so HA has fresh values
  // promptly once a cycle closes.
  if (evt.kind == MdbEvent::SALE_CYCLE_END) {
    if (this->sens_sale_cycles_total_) {
      this->sens_sale_cycles_total_->publish_state((float) this->sale_cycles_total_);
    }
    if (this->sens_last_sale_duration_) {
      this->sens_last_sale_duration_->publish_state(this->last_sale_duration_ms_ / 1000.0f);
    }
  }
#endif

#ifdef USE_BINARY_SENSOR
  if (this->bs_bv_jam_) this->bs_bv_jam_->publish_state(this->bv_jam_state_);
  if (this->bs_bv_disabled_) this->bs_bv_disabled_->publish_state(this->bv_disabled_state_);
  if (this->bs_changer_jam_) this->bs_changer_jam_->publish_state(this->changer_jam_state_);
  if (this->bs_cl_malfunction_) this->bs_cl_malfunction_->publish_state(this->cl_malfunction_state_);
  if (this->bs_session_active_) this->bs_session_active_->publish_state(this->session_active_);
  if (this->bs_vend_in_progress_) this->bs_vend_in_progress_->publish_state(this->vend_in_progress_);
  if (this->bs_sale_cycle_in_progress_ &&
      (evt.kind == MdbEvent::SALE_CYCLE_BEGIN ||
       evt.kind == MdbEvent::SALE_CYCLE_END)) {
    this->bs_sale_cycle_in_progress_->publish_state(this->sale_cycle_in_progress_);
  }
#endif

#ifdef USE_TEXT_SENSOR
  if (this->ts_last_event_) {
    this->ts_last_event_->publish_state(evt.description);
  }
  if (this->ts_last_bv_error_ && evt.kind == MdbEvent::BV_ERROR) {
    this->ts_last_bv_error_->publish_state(evt.description);
  }
  if (this->ts_last_changer_error_ && evt.kind == MdbEvent::CHG_ERROR) {
    this->ts_last_changer_error_->publish_state(evt.description);
  }
  if (this->ts_last_cl_error_ && evt.kind == MdbEvent::CL_MALFUNCTION) {
    this->ts_last_cl_error_->publish_state(evt.description);
  }
  if (this->ts_last_selection_name_ && (evt.kind == MdbEvent::VEND_REQUEST ||
                                          evt.kind == MdbEvent::CASH_SALE ||
                                          evt.kind == MdbEvent::VEND_SUCCESS)) {
    const char *name = this->lookup_selection_name_(evt.item_number);
    char tmp[32];
    if (name != nullptr) {
      this->ts_last_selection_name_->publish_state(name);
    } else {
      std::snprintf(tmp, sizeof(tmp), "item %u", (unsigned) evt.item_number);
      this->ts_last_selection_name_->publish_state(tmp);
    }
  }
  if (this->ts_bv_enable_mask_ && evt.kind == MdbEvent::BV_BILL_TYPE_CMD) {
    const uint16_t enable_mask = (uint16_t) (evt.data32 >> 16);
    const uint16_t escrow_mask = (uint16_t) (evt.data32 & 0xFFFFu);
    char tmp[24];
    std::snprintf(tmp, sizeof(tmp), "%04X/%04X",
                  (unsigned) enable_mask, (unsigned) escrow_mask);
    this->ts_bv_enable_mask_->publish_state(tmp);
  }
#endif

  // 3. Fire user callbacks (triggers attach here).
  this->event_callbacks_.call(evt);
}

}  // namespace pulse_inspector_mdb
}  // namespace esphome

#endif  // USE_ESP_IDF
