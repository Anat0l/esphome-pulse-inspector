# esphome pulse_inspector (PulseInspector)

ESPHome external component for ESP32 (ESP-IDF framework): a transparent
multi-channel GPIO pulse inspector. Each channel mirrors the input pin to
the output pin from an IRAM interrupt handler while pushing every edge into
a FreeRTOS queue for background processing. Protocol-agnostic hub with an
extension point for traffic modification.

Russian version: [README.md](README.md).

## Component family

| Component | Description | Docs |
|---|---|---|
| `pulse_inspector` | Hub: GPIO pass-through + pulse queue | [components/pulse_inspector/README.en.md](components/pulse_inspector/README.en.md) |
| `pulse_inspector_logger` | Template child component | [components/pulse_inspector_logger/README.en.md](components/pulse_inspector_logger/README.en.md) |
| `pulse_inspector_vcd` | TCP VCD logic analyzer | [components/pulse_inspector_vcd/README.en.md](components/pulse_inspector_vcd/README.en.md) |
| `pulse_inspector_mdb` | MDB bus decoder + HA entities | [components/pulse_inspector_mdb/README.en.md](components/pulse_inspector_mdb/README.en.md) |
| `pulse_inspector_exe` | Necta Executive line decoder | [components/pulse_inspector_exe/README.en.md](components/pulse_inspector_exe/README.en.md) |

Index: [components/README.en.md](components/README.en.md).

## Status

Core hub: ISR mirroring, multi-channel, pulse callbacks, diagnostics.

Child modules: VCD capture, MDB/Executive decoders, logger template.

Not yet implemented: generic traffic modification API (HA packet injection).

## Requirements

- ESP32 with `framework: type: esp-idf` (Arduino not supported).

## Configuration

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [pulse_inspector]

pulse_inspector:
  id: inspector
  channels:
    - id: ch_a
      input_pin: GPIO16
      output_pin: GPIO17
```

See [`example.yaml`](example.yaml) for a full example.

## Diagnostics

```
[pulse_inspector] ch0 GPIO16: edges=1234 (+42) overflows=0 (+0) processed=1234
```

## Building on top of `pulse_inspector`

See template in [`components/pulse_inspector_logger`](components/pulse_inspector_logger).
Full developer notes (code samples): see git history or expand from
[Russian README](README.md#создание-компонентов-поверх-pulse_inspector).

## License

MIT.
