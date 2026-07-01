# pulse_inspector_vcd (`PulseInspectorVcd`)

Turns ESP32 into a **network logic analyzer**. Subscribes to all channels
of parent [`pulse_inspector`](../pulse_inspector/README.en.md) and streams
edges as live **[VCD (Value Change Dump)](https://en.wikipedia.org/wiki/Value_change_dump)**
over TCP.

Russian version (full manual): [README.md](README.md).

| ESPHome metadata | |
|---|---|
| `CODEOWNERS` | `@anat0l` |
| `DEPENDENCIES` | `esp32`, `pulse_inspector` |
| `MULTI_CONF` | `false` |

Component index: [../README.en.md](../README.en.md).

Connect with `nc`/`ncat`/`socat`/PulseView, save to file, or open live in
[PulseView](https://sigrok.org/wiki/PulseView) or
[GTKWave](https://gtkwave.sourceforge.net/) and add protocol decoders
(UART, I²C, SPI, …).

## Features

- Captures **all** parent `pulse_inspector` channels (up to 94 wires per
  VCD id limit).
- Ring buffer (`buffer_size`) for pre-trigger history on connect.
- Live queue while a client is connected.
- TCP keepalive + optional `$comment` application heartbeat.
- Heavy work in a FreeRTOS task; ISR path stays lean.

## Requirements

- ESP32, ESPHome `framework: type: esp-idf`.
- Parent `pulse_inspector` with at least one channel.
- `wifi:` (or other network) — server starts at `AFTER_WIFI`.

## Installation

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/Anat0l/esphome-pulse-inspector
      ref: main
      path: components
    components: [pulse_inspector, pulse_inspector_vcd]

pulse_inspector:
  id: inspector
  channels:
    - id: ch_a
      input_pin: GPIO16

pulse_inspector_vcd:
  inspector_id: inspector
```

Capture:

```bash
ncat <device-ip> 9000 > capture.vcd
```

Recommended client: `python tools/vcd_capture_tcp.py <ip> 9000 capture.vcd`

## Main options

| Option | Default | Description |
|---|---|---|
| `inspector_id` | required | Parent `PulseInspector` |
| `port` | `9000` | TCP listen port (one client at a time) |
| `buffer_size` | `4096` | Ring buffer events (pre-trigger) |
| `send_timeout` | `30s` | `SO_SNDTIMEO` for stuck clients |
| `live_queue_depth` | `1024` | Live stream queue; overflow drops events |
| `app_keepalive_interval` | `15s` | VCD `$comment` heartbeat; `0s` disables |
| `tcp_keepalive` | enabled | lwIP keepalive `idle` / `interval` / `count` |

Effective dead-client detection ≈ `idle + interval × count` (~60 s with defaults).

## Signal names

Each channel becomes wire `chN_gpioM` (`N` = channel index, `M` = GPIO number).

## Diagnostics

```
[I][pulse_inspector_vcd]: Listening for VCD downloads on tcp/9000
[I][pulse_inspector_vcd]: Client connected, starting live VCD stream
[I][pulse_inspector_vcd]: Client disconnected (live overflow during session: 0)
```

Non-zero `live overflow` — increase `live_queue_depth` or improve Wi-Fi.

## Troubleshooting

| Symptom | Action |
|---|---|
| Disconnect after 1–3 min | Enable keepalive/heartbeat; check Wi-Fi drops in log |
| `live overflow > 0` | Raise `live_queue_depth`; fewer channels; better RF |
| Empty / truncated file | Use `tools/vcd_capture_tcp.py` instead of buffered `nc` |
| Second client hangs | By design: one client; wait for first to close |

## Limitations

- Single TCP client per `pulse_inspector_vcd` instance.
- VCD wire ids are single-byte (`!` …); max ~94 channels per exporter.
- Timestamps are microseconds since stream start, not wall clock.

## License

MIT.
