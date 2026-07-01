# pulse_inspector_exe (`PulseInspectorExe`)

**Necta Executive** line decoder (MEI Protocol A) on
[`pulse_inspector`](../pulse_inspector/README.en.md). Two GPIO channels
decoded as **8E1 UART** at 9600 baud. Publishes frames, credit/vend
semantics, triggers, and HA sensors.

Russian version: [README.md](README.md).

## Channel roles

| YAML | Role |
|---|---|
| `executive_channel` | Executive TX (0x31 STATUS, 0x32 CREDIT) |
| `peripheral_channel` | VMC / ASU / CPP replies |

## Example

```yaml
pulse_inspector_exe:
  id: exe
  inspector_id: inspector
  executive_channel: 0
  peripheral_channel: 1
  parity: even
  suppress_idle_polls: true
```

## Triggers

`on_master_frame`, `on_slave_frame`, `on_vend_complete`, `on_credit_change`,
`on_vmc_status`.

## ESPHome metadata

`DEPENDENCIES`: `esp32`, `pulse_inspector` · `MULTI_CONF`: `true`

## License

MIT.
