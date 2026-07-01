# pulse_inspector_mdb (`PulseInspectorMdb`)

**MDB** (Multi-Drop Bus, NAMA) decoder on [`pulse_inspector`](../pulse_inspector/README.en.md).
Decodes GPIO edges as 9-bit UART, parses master/slave frames, publishes
events to logs, YAML triggers, and HA sensors.

Russian version: [README.md](README.md).

## Wiring modes

1. **Single-wire** (`channel: N`) — bit 9 distinguishes master vs slave.
2. **Two-wire** (`tx_channel` + `rx_channel`) — separate VMC and peripheral taps.

Channel indices refer to `pulse_inspector.channels` (0..15).

## Example

```yaml
pulse_inspector_mdb:
  id: mdb
  inspector_id: inspector
  tx_channel: 0
  rx_channel: 1
  suppress_idle_polls: true
```

## Triggers

`on_vend_request`, `on_vend_success`, `on_vend_failure`, `on_cash_sale`,
`on_bill_accepted`, `on_coin_deposited`, and more — see Russian README.

## HA sensors

```yaml
sensor:
  - platform: pulse_inspector_mdb
    mdb_id: mdb
    frames_decoded:
      name: MDB frames
```

## ESPHome metadata

`DEPENDENCIES`: `esp32`, `pulse_inspector` · `MULTI_CONF`: `true`

## License

MIT.
