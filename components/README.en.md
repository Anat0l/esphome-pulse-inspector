# ESPHome external components: PulseInspector family

Russian version: [README.md](README.md).

ESP32 (ESP-IDF) external components. Hub [`pulse_inspector`](pulse_inspector/README.en.md)
mirrors GPIO→GPIO in ISR and feeds edge streams to child modules.

## Components

| Component | YAML key | Purpose | Docs |
|---|---|---|---|
| PulseInspector | `pulse_inspector` | Hub | [README.en.md](pulse_inspector/README.en.md) |
| Logger | `pulse_inspector_logger` | Child template | [README.en.md](pulse_inspector_logger/README.en.md) |
| VCD | `pulse_inspector_vcd` | TCP logic analyzer | [README.en.md](pulse_inspector_vcd/README.en.md) |
| MDB | `pulse_inspector_mdb` | MDB decoder | [README.en.md](pulse_inspector_mdb/README.en.md) |
| EXE | `pulse_inspector_exe` | Executive decoder | [README.en.md](pulse_inspector_exe/README.en.md) |

## Install from GitHub

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/Anat0l/esphome-pulse-inspector
      ref: main
      path: components
    components: [pulse_inspector, pulse_inspector_mdb]
```

## Quick start

See [Russian README](README.md#быстрый-старт) or [`../example.yaml`](../example.yaml).

## License

MIT.
