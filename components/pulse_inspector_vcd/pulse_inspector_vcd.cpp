#include "pulse_inspector_vcd.h"

#ifdef USE_ESP_IDF

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esphome/core/log.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/tcp.h"  // TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT

namespace esphome {
namespace pulse_inspector_vcd {

static const char *const TAG = "pulse_inspector_vcd";

// First printable ASCII char that VCD allows as a $var identifier. The spec
// says 33..126 excluding space; '!' is the first. With a single-byte id we
// can encode up to 94 channels in a VCD file — far more than we'll ever
// hook up to one pulse_inspector.
static constexpr char VCD_SYM_FIRST = '!';
static constexpr size_t VCD_MAX_CHANNELS = 94;

// FreeRTOS priorities / stack sizes for the TCP server task.
static constexpr UBaseType_t SERVER_TASK_PRIO = 4;
static constexpr uint32_t SERVER_TASK_STACK = 4096;

// Max time to wait for the buffer mutex from a channel task. If we can't
// take it within this window (should never happen — critical section is
// a few memcpy's), drop the edge rather than blocking the ISR pipeline.
static constexpr TickType_t PRODUCER_MUTEX_TIMEOUT = pdMS_TO_TICKS(10);

// Max time to wait for the buffer mutex from the server task. Here we're
// fine to wait longer — download is explicitly triggered by the user.
static constexpr TickType_t CONSUMER_MUTEX_TIMEOUT = pdMS_TO_TICKS(1000);

// How long the streaming loop waits for a new event before flushing the
// pending TCP buffer and probing the socket. Small enough that the user
// sees fresh data promptly; large enough to actually batch sends.
static constexpr TickType_t STREAM_POLL_INTERVAL = pdMS_TO_TICKS(100);

// NOTE: Per-connection live queue depth, TCP send timeout, application
// heartbeat interval and lwIP TCP-keepalive knobs are configurable from
// YAML now -- see PulseInspectorVcd::set_*_sec() and the matching
// fields in the header.

// ---------------------------------------------------------------------------
// ChunkedSender: small helper around send() that batches writes into a
// stack-local buffer so we don't issue a separate TCP syscall per VCD line.
// ---------------------------------------------------------------------------
namespace {

struct ChunkedSender {
  int sock;
  char buf[1024];
  size_t fill{0};
  bool ok{true};
  // Diagnostics: total bytes successfully handed to send() during the
  // session, and the errno captured on the first failed send. Useful to
  // tell apart "client app stopped reading" vs "lwIP returned an error".
  uint32_t total_sent{0};
  int last_errno{0};

  void flush() {
    if (!ok || fill == 0) return;
    size_t off = 0;
    while (off < fill) {
      int n = send(sock, buf + off, fill - off, 0);
      if (n <= 0) {
        if (last_errno == 0) last_errno = errno;
        ok = false;
        return;
      }
      off += (size_t) n;
      total_sent += (uint32_t) n;
    }
    fill = 0;
  }

  void write(const char *s, size_t n) {
    while (n > 0 && ok) {
      size_t room = sizeof(buf) - fill;
      size_t take = n < room ? n : room;
      memcpy(buf + fill, s, take);
      fill += take;
      s += take;
      n -= take;
      if (fill == sizeof(buf)) flush();
    }
  }

  void writes(const char *s) { write(s, strlen(s)); }

  __attribute__((format(printf, 2, 3)))
  void printfln(const char *fmt, ...) {
    char line[80];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t) n >= sizeof(line)) n = sizeof(line) - 1;
    write(line, (size_t) n);
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// PulseInspectorVcd
// ---------------------------------------------------------------------------

void PulseInspectorVcd::setup() {
  if (inspector_ == nullptr) {
    ESP_LOGE(TAG, "No pulse_inspector configured");
    this->mark_failed();
    return;
  }

  const auto &channels = inspector_->get_channels();
  if (channels.empty()) {
    ESP_LOGE(TAG, "PulseInspector has no channels");
    this->mark_failed();
    return;
  }
  if (channels.size() > VCD_MAX_CHANNELS) {
    ESP_LOGE(TAG, "Too many channels (%u) for single-byte VCD identifiers",
             (unsigned) channels.size());
    this->mark_failed();
    return;
  }

  mutex_ = xSemaphoreCreateMutex();
  if (mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create mutex");
    this->mark_failed();
    return;
  }

  buffer_.assign(capacity_, VcdEvent{});
  head_ = 0;
  count_ = 0;

  // Subscribe to every channel. Each callback carries the channel index so
  // the shared buffer can distinguish them without any extra lookup.
  symbols_.clear();
  symbols_.reserve(channels.size());
  for (size_t i = 0; i < channels.size(); i++) {
    symbols_.push_back((char) (VCD_SYM_FIRST + i));
    uint8_t idx = (uint8_t) i;
    channels[i]->add_on_pulse_callback(
        [this, idx](const pulse_inspector::PulseItem &item) { this->on_pulse(idx, item); });
  }

  BaseType_t ret = xTaskCreate(&PulseInspectorVcd::server_trampoline, "sig_vcd",
                               SERVER_TASK_STACK, this, SERVER_TASK_PRIO, &server_task_);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create VCD TCP server task (err=%d)", (int) ret);
    this->mark_failed();
    return;
  }

  ESP_LOGCONFIG(TAG, "VCD capture ready on tcp/%u (buffer=%u events, %u channels)",
                (unsigned) port_, (unsigned) capacity_, (unsigned) channels.size());
}

void PulseInspectorVcd::on_pulse(uint8_t ch_idx, const pulse_inspector::PulseItem &item) {
  VcdEvent ev{};
#ifdef __XTENSA__
  // item.cycle is xthal_get_ccount() (CPU cycles). Convert to microseconds
  // assuming a 240 MHz CPU clock (ESP32 default). If you change the clock
  // via `esp32 -> cpu_frequency`, update this divisor accordingly.
  ev.t_us = item.cycle / 240u;
#else
  // On ESP32-C3/S2/S3 pulse_inspector stores esp_timer_get_time() (µs).
  ev.t_us = item.cycle;
#endif
  ev.ch_idx = ch_idx;
  ev.level = item.level ? 1 : 0;

  if (xSemaphoreTake(mutex_, PRODUCER_MUTEX_TIMEOUT) != pdTRUE) {
    return;  // drop event rather than blocking the channel task
  }
  buffer_[head_] = ev;
  head_ = (head_ + 1) % capacity_;
  if (count_ < capacity_) count_++;

  // If a client is currently streaming, also enqueue the event for live
  // delivery. We hold the mutex while touching live_queue_ so the server
  // task can safely null it out and delete the queue on disconnect.
  if (live_queue_ != nullptr) {
    if (xQueueSend(live_queue_, &ev, 0) != pdTRUE) {
      live_overflow_count_++;
    }
  }
  xSemaphoreGive(mutex_);
}

void PulseInspectorVcd::server_trampoline(void *arg) {
  static_cast<PulseInspectorVcd *>(arg)->server_loop();
}

void PulseInspectorVcd::server_loop() {
  int srv = -1;
  for (;;) {
    if (srv < 0) {
      srv = socket(AF_INET, SOCK_STREAM, 0);
      if (srv < 0) {
        ESP_LOGE(TAG, "socket() failed, retrying in 2s");
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }

      int on = 1;
      setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
      addr.sin_port = htons(port_);

      if (bind(srv, (sockaddr *) &addr, sizeof(addr)) < 0 || listen(srv, 1) < 0) {
        ESP_LOGE(TAG, "bind/listen on :%u failed (errno=%d)", (unsigned) port_, errno);
        close(srv);
        srv = -1;
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
      ESP_LOGI(TAG, "Listening for VCD downloads on tcp/%u", (unsigned) port_);
    }

    sockaddr_in cli_addr{};
    socklen_t cli_len = sizeof(cli_addr);
    int cli = accept(srv, (sockaddr *) &cli_addr, &cli_len);
    if (cli < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    ESP_LOGI(TAG, "Client %u.%u.%u.%u connected, starting live VCD stream",
             (unsigned) ((cli_addr.sin_addr.s_addr >> 0) & 0xff),
             (unsigned) ((cli_addr.sin_addr.s_addr >> 8) & 0xff),
             (unsigned) ((cli_addr.sin_addr.s_addr >> 16) & 0xff),
             (unsigned) ((cli_addr.sin_addr.s_addr >> 24) & 0xff));

    this->serve_client_(cli);
    close(cli);
  }
}

void PulseInspectorVcd::serve_client_(int client_sock) {
  // Give send() a bounded timeout so a half-dead client (router rebooted,
  // laptop suspended, ...) doesn't wedge the task forever. Made
  // configurable so flaky Wi-Fi setups can give the kernel more headroom
  // before the connection is forcibly torn down.
  {
    struct timeval tv {};
    tv.tv_sec = (time_t) this->send_timeout_sec_;
    tv.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }

  // Enable lwIP TCP-level keepalives. Without these a half-open
  // connection is only detected via TCP retransmission timeouts
  // (60-180 s with the lwIP defaults), which is exactly the "1-3 min
  // disconnect" symptom on noisy Wi-Fi. With keepalives on, the kernel
  // probes the peer after `idle` seconds of silence and tears the socket
  // down after `count` failed probes spaced `interval` seconds apart.
  if (this->tcp_keepalive_enabled_) {
    int on = 1;
    setsockopt(client_sock, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    int idle = (int) this->tcp_keepalive_idle_sec_;
    int intvl = (int) this->tcp_keepalive_interval_sec_;
    int cnt = (int) this->tcp_keepalive_count_;
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
  }

  // Disable Nagle: our payload is many small VCD lines (3-12 bytes),
  // and Nagle would either coalesce them (worsening latency) or — under
  // packet loss — keep them queued in the lwIP send buffer, which on
  // ESP32 defaults to a tiny 5744 B and gets full surprisingly fast.
  {
    int on = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  }

  // Ask lwIP for a larger TCP send buffer. Default ESP-IDF
  // CONFIG_LWIP_TCP_SND_BUF_DEFAULT is 5744 (4 * MSS). With a 32 KB
  // request lwIP uses whatever it can actually allocate, but the call
  // itself never fails (silently bounded by CONFIG_LWIP_TCP_SND_BUF_MAX).
  {
    int sndbuf = 32 * 1024;
    setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  }

  // Allocate the per-connection live queue first so producers that fire
  // between the snapshot and the start of streaming still end up in the
  // stream (they'll be time-ordered after the snapshot by virtue of being
  // newer events; stable_sort on the snapshot alone is enough there, and
  // the streaming side keeps monotonicity via `last_t_emitted`).
  const UBaseType_t queue_depth = (UBaseType_t) this->live_queue_depth_;
  QueueHandle_t live = xQueueCreate(queue_depth, sizeof(VcdEvent));
  if (live == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate live streaming queue (depth=%u)",
             (unsigned) queue_depth);
    return;
  }

  // Atomically: take a snapshot of the ring buffer for pre-trigger
  // history, attach the live queue so on_pulse() starts filling it, and
  // reset the overflow counter so a later log message only reflects this
  // session.
  std::vector<VcdEvent> snap;
  if (xSemaphoreTake(mutex_, CONSUMER_MUTEX_TIMEOUT) == pdTRUE) {
    snap.reserve(count_);
    size_t start = (count_ == capacity_) ? head_ : 0;
    for (size_t i = 0; i < count_; i++) {
      snap.push_back(buffer_[(start + i) % capacity_]);
    }
    live_queue_ = live;
    live_overflow_count_ = 0;
    xSemaphoreGive(mutex_);
  } else {
    ESP_LOGW(TAG, "Could not take mutex; streaming without pre-trigger history");
    if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
      live_queue_ = live;
      live_overflow_count_ = 0;
      xSemaphoreGive(mutex_);
    }
  }

  // Channel callbacks run in per-channel FreeRTOS tasks, so the ring buffer
  // is roughly — but not strictly — ordered by timestamp. A stable sort
  // enforces global chronological order that VCD requires (#t markers must
  // be non-decreasing).
  std::stable_sort(snap.begin(), snap.end(),
                   [](const VcdEvent &a, const VcdEvent &b) { return a.t_us < b.t_us; });

  ChunkedSender out{client_sock};
  const auto &channels = inspector_->get_channels();

  // --- VCD header ---------------------------------------------------------
  out.writes("$date esphome pulse_inspector_vcd $end\n");
  out.writes("$version 1 $end\n");
  out.writes("$timescale 1 us $end\n");
  out.writes("$scope module esp32 $end\n");
  for (size_t i = 0; i < channels.size(); i++) {
    out.printfln("$var wire 1 %c ch%u_gpio%d $end\n",
                 symbols_[i], (unsigned) i, channels[i]->get_input_gpio_num());
  }
  out.writes("$upscope $end\n");
  out.writes("$enddefinitions $end\n");

  // --- Initial values at time 0 ------------------------------------------
  // VCD requires every $var to have a defined state at the start of the
  // waveform. We infer the initial level per channel from the first edge
  // we see (either in the pre-trigger snapshot or, failing that, in the
  // live stream): since PulseItem.level is the level AFTER the edge, the
  // initial one is its inverse. Channels that never transition are left
  // as 'x' (unknown) — the viewer will render them as a dashed line.
  std::vector<int8_t> init(channels.size(), -1);
  for (const auto &e : snap) {
    if (e.ch_idx < init.size() && init[e.ch_idx] == -1) {
      init[e.ch_idx] = e.level ? 0 : 1;
    }
  }
  out.writes("#0\n");
  for (size_t i = 0; i < channels.size(); i++) {
    char v = (init[i] == -1) ? 'x' : (char) ('0' + init[i]);
    out.printfln("%c%c\n", v, symbols_[i]);
  }
  out.flush();  // push the header out immediately so viewers can open the

  // stream as it grows.

  // --- Pre-trigger snapshot + live stream --------------------------------
  // t0 is the reference point for all `#t` markers. We prefer the oldest
  // event in the pre-trigger snapshot; if the ring buffer was empty, we
  // defer picking t0 until the first live event arrives.
  bool have_t0 = false;
  uint32_t t0 = 0;
  uint32_t last_t_emitted = 0;  // enforces monotonic `#t` markers

  auto emit_event = [&](const VcdEvent &e) {
    if (!have_t0) {
      t0 = e.t_us;
      have_t0 = true;
    }
    uint32_t t = e.t_us - t0;
    // Events from different channel tasks can arrive slightly out of
    // timestamp order. VCD requires #t to be non-decreasing, so clamp.
    if (t < last_t_emitted) t = last_t_emitted;
    if (t != last_t_emitted) {
      out.printfln("#%u\n", (unsigned) t);
      last_t_emitted = t;
    }
    if ((size_t) e.ch_idx < symbols_.size()) {
      out.printfln("%c%c\n", e.level ? '1' : '0', symbols_[e.ch_idx]);
    }
  };

  for (const auto &e : snap) emit_event(e);
  out.flush();

  // Live loop: block on the per-connection queue, write every arriving
  // event straight to the socket, flush on idle so the user sees live
  // progress even during quiet periods. The loop exits when send() fails
  // (client disconnected) or the socket send timeout expires.
  //
  // Two safety nets keep the link alive on quiet networks:
  //
  //   1. Periodic application-layer heartbeat: every
  //      `app_keepalive_interval_sec_` seconds without a real event we
  //      write a `$comment` line. VCD viewers ignore it, but the bytes
  //      on the wire keep stateful NATs/routers from declaring the
  //      connection idle and reset it. Set the interval to 0 to disable.
  //
  //   2. lwIP TCP keepalives (configured above) — kernel-level probes
  //      that detect a fully dead peer within `idle + count*interval`
  //      seconds.
  const uint32_t app_keepalive_ms =
      this->app_keepalive_interval_sec_ * 1000u;
  // Wall-clock anchors for: last real event (drives app keepalive), last
  // diagnostic log line, and start of the session (for "session age" in
  // the log). All in ms since boot, wrap-safe via uint32 arithmetic.
  const uint32_t session_start_ms =
      (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
  uint32_t last_data_ms = session_start_ms;
  uint32_t last_log_ms = session_start_ms;
  uint32_t events_emitted = 0;
  // The probe `send(..., 0, MSG_DONTWAIT)` that used to live in the idle
  // branch was removed: with SO_KEEPALIVE on it just adds noise (and on
  // some lwIP states returns -1/ENOMEM, which we'd misread as a peer
  // disconnect and tear the session down ourselves).
  // Tag of the code path that detected the disconnect. Reset on every
  // `out.ok = false` transition we trip ourselves so the final disconnect
  // log line tells us exactly which flush failed.
  const char *exit_tag = "loop";
  VcdEvent ev;
  while (out.ok) {
    BaseType_t r = xQueueReceive(live, &ev, STREAM_POLL_INTERVAL);
    if (r == pdTRUE) {
      emit_event(ev);
      events_emitted++;
      // Implicit flush from emit_event when the buffer fills; if it
      // fails we'll catch it on the next `while (out.ok)` check.
      last_data_ms = (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
      if (!out.ok) { exit_tag = "emit"; break; }
    } else {
      // Idle tick — push whatever sits in the chunked-sender buffer so
      // the user sees fresh data promptly.
      out.flush();
      if (!out.ok) { exit_tag = "idle-flush"; break; }

      if (app_keepalive_ms != 0) {
        const uint32_t now_ms =
            (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now_ms - last_data_ms >= app_keepalive_ms) {
          out.writes("$comment esphome vcd keepalive $end\n");
          out.flush();
          if (!out.ok) { exit_tag = "keepalive"; break; }
          last_data_ms = now_ms;
        }
      }
    }

    // Periodic session diagnostic — tells the user (and us) whether the
    // device is actually still pushing bytes and how big the on-wire
    // capture currently is. If the file on the client side is much
    // smaller than `bytes` here, the bottleneck is on the receiving end
    // (PowerShell redirect buffering, slow disk, ...), not the device.
    {
      const uint32_t now_ms =
          (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
      if (now_ms - last_log_ms >= 5000u) {
        const uint32_t age_s = (now_ms - session_start_ms) / 1000u;
        ESP_LOGI(TAG,
                 "VCD stream: %u s, %u events, %u bytes sent, queue overflow=%u",
                 (unsigned) age_s, (unsigned) events_emitted,
                 (unsigned) out.total_sent,
                 (unsigned) live_overflow_count_);
        last_log_ms = now_ms;
      }
    }
  }

  // Detach the live queue from the producers before destroying it.
  if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    live_queue_ = nullptr;
    xSemaphoreGive(mutex_);
  }
  // Drain anything producers managed to enqueue between the null-out and
  // now so vQueueDelete doesn't leak heap items. VcdEvent is trivially
  // destructible, so a blind drain loop is enough.
  while (xQueueReceive(live, &ev, 0) == pdTRUE) {
  }
  vQueueDelete(live);

  // `last_errno != 0` means lwIP signalled an error on send() — we're the
  // ones who tore down the connection. `last_errno == 0` means send()
  // never failed and the loop exited only because the peer closed the
  // socket from its side (or our app-level reset path triggered).
  // `exit_tag` narrows it down further: which exact flush failed.
  ESP_LOGI(TAG,
           "Client disconnected at %s: %u events, %u bytes sent, "
           "queue overflow=%u, last_errno=%d (%s)",
           exit_tag, (unsigned) events_emitted, (unsigned) out.total_sent,
           (unsigned) live_overflow_count_, out.last_errno,
           out.last_errno == 0 ? "peer closed" : strerror(out.last_errno));
}

void PulseInspectorVcd::dump_config() {
  ESP_LOGCONFIG(TAG, "pulse_inspector_vcd:");
  ESP_LOGCONFIG(TAG, "  TCP port: %u", (unsigned) port_);
  ESP_LOGCONFIG(TAG, "  Buffer size: %u events", (unsigned) capacity_);
  ESP_LOGCONFIG(TAG, "  Live queue depth: %u events",
                (unsigned) live_queue_depth_);
  ESP_LOGCONFIG(TAG, "  Send timeout: %u s", (unsigned) send_timeout_sec_);
  if (app_keepalive_interval_sec_ != 0) {
    ESP_LOGCONFIG(TAG, "  App keepalive: %u s", (unsigned) app_keepalive_interval_sec_);
  } else {
    ESP_LOGCONFIG(TAG, "  App keepalive: disabled");
  }
  if (tcp_keepalive_enabled_) {
    ESP_LOGCONFIG(TAG, "  TCP keepalive: idle=%us interval=%us count=%u",
                  (unsigned) tcp_keepalive_idle_sec_,
                  (unsigned) tcp_keepalive_interval_sec_,
                  (unsigned) tcp_keepalive_count_);
  } else {
    ESP_LOGCONFIG(TAG, "  TCP keepalive: disabled");
  }
  ESP_LOGCONFIG(TAG, "  Download with:  nc <device-ip> %u > capture.vcd",
                (unsigned) port_);
}

}  // namespace pulse_inspector_vcd
}  // namespace esphome

#endif  // USE_ESP_IDF
