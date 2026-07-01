# pulse_inspector_logger (`PulseInspectorLogger`)

Example **child** component on top of [`pulse_inspector`](../pulse_inspector/README.en.md).
Subscribes to one channel, counts edges in the channel task, and logs a
summary from ESPHome `loop()`.

Russian version: [README.md](README.md).

## Configuration

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [pulse_inspector, pulse_inspector_logger]

pulse_inspector:
  id: inspector
  channels:
    - id: ch_a
      input_pin: GPIO16
      output_pin: GPIO17

pulse_inspector_logger:
  - channel_id: ch_a
    report_interval: 5s
```

## Options

| Option | Default | Description |
|---|---|---|
| `channel_id` | required | `PulseInspectorChannel` id |
| `report_interval` | `5s` | Log summary interval |

## ESPHome metadata

`DEPENDENCIES`: `esp32`, `pulse_inspector` · `MULTI_CONF`: `true`

## License

MIT.
