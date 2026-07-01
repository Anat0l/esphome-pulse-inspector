# pulse_inspector (`PulseInspector`)

Hub component of the PulseInspector family. Transparently passes digital
signals from input GPIO to output GPIO **in ISR** (IRAM-safe) and queues
every edge for child decoders.

Russian version: [README.md](README.md).

## Features

- Multiple independent channels (`input_pin` → `output_pin`).
- ISR-level mirroring (microsecond-scale latency).
- Per-channel `PulseItem` queue + background task.
- Child API: `add_on_pulse_callback()`, `add_on_packet_callback()` /
  `trigger_packet()`.
- Diagnostics every 5 s at `DEBUG`.

## Requirements

- ESP32, **`esp-idf`** framework (not Arduino).

## Configuration

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [pulse_inspector]

esp32:
  framework:
    type: esp-idf

pulse_inspector:
  id: inspector
  channels:
    - id: ch_a
      input_pin: GPIO16
      output_pin: GPIO17
```

## Channel options

| Option | Default | Description |
|---|---|---|
| `input_pin` | required | Input, pull-up, any-edge IRQ |
| `output_pin` | optional | Mirror output; omit for receive-only |
| `invert_in` / `invert_out` | `false` | Logic inversion |
| `queue_size` | `256` | Pulse queue depth (16..8192) |

## ESPHome metadata

`DEPENDENCIES`: `esp32` · `MULTI_CONF`: `false` · `CODEOWNERS`: `@anat0l`

## License

MIT.
