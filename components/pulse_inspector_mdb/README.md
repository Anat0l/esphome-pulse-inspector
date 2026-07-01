# pulse_inspector_mdb (`PulseInspectorMdb`)

Декодер шины **MDB** (Multi-Drop Bus, NAMA) поверх
[`pulse_inspector`](../pulse_inspector/README.md). Преобразует фронты GPIO
в 9-bit UART (9600 9N1 по умолчанию), разбирает кадры master/slave и
публикует семантические события (vend, cash sale, bill accepted, …) в
лог, YAML-триггеры и Home Assistant sensors.

English version: [README.en.md](README.en.md).

## Режимы подключения

1. **Однопроводный** (`channel: N`) — TX и RX на одной линии; 9-й бит
   различает адрес master (bit9=1) и данные slave (bit9=0).
2. **Двухпроводный** (`tx_channel` + `rx_channel`) — отдельные GPIO на
   линии VMC (master) и периферии (slave).

Каналы задаются **индексом** в списке `pulse_inspector.channels` (0…15).

## Установка

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [pulse_inspector, pulse_inspector_mdb]

pulse_inspector:
  id: inspector
  channels:
    - id: ch_mdb_tx
      input_pin: GPIO16
      output_pin: GPIO17
    - id: ch_mdb_rx
      input_pin: GPIO18
      output_pin: GPIO19

pulse_inspector_mdb:
  id: mdb
  inspector_id: inspector
  tx_channel: 0
  rx_channel: 1
  baud: 9600
  suppress_idle_polls: true
  on_vend_success:
    - logger.log: "Vend OK"
```

## Основные параметры

| Параметр | По умолчанию | Описание |
|---|---|---|
| `inspector_id` | — | Родительский `PulseInspector`. |
| `channel` | — | Однопроводный режим (индекс канала). |
| `tx_channel` / `rx_channel` | — | Двухпроводный режим (оба обязательны). |
| `baud` | `9600` | Скорость UART. |
| `suppress_idle_polls` | `false` | Скрывать idle POLL/ACK в логе. |
| `log_raw_frames` | `false` | Hex-дамп каждого кадра. |
| `selection_map` | — | Список `{item, name}` для имён напитков. |

## YAML-триггеры

`on_event`, `on_vend_request`, `on_vend_approved`, `on_vend_denied`,
`on_vend_success`, `on_vend_failure`, `on_cash_sale`, `on_session_begin`,
`on_session_end`, `on_bv_error`, `on_bill_accepted`, `on_coin_deposited`,
`on_changer_error`, `on_cashless_error`, `on_sale_cycle_begin`,
`on_sale_cycle_end`.

## Платформы Home Assistant

Объявляются отдельно (см. `sensor.py`, `binary_sensor.py`, `text_sensor.py`):

```yaml
sensor:
  - platform: pulse_inspector_mdb
    mdb_id: mdb
    frames_decoded:
      name: MDB frames
    vend_success_count:
      name: Vends OK
```

Доступные поля sensor: `last_item`, `last_price`, `session_funds`,
`vend_success_count`, `vend_failure_count`, `frames_decoded`,
`framing_errors`, `master_bytes`, `slave_bytes`, `events_dropped`
(переполнение очереди событий и импульсы, отброшенные при нехватке mutex),
`proprietary_frames`, счётчики купюр (`bills_*`), `sale_cycles_total`,
`last_sale_duration`.

## Метаданные ESPHome

| Поле | Значение |
|---|---|
| `CODEOWNERS` | `@anat0l` |
| `DEPENDENCIES` | `esp32`, `pulse_inspector` |
| `MULTI_CONF` | `true` |

## Связанные компоненты

- Захват осциллограммы: [`pulse_inspector_vcd`](../pulse_inspector_vcd/README.md)
- Полный пример: [`../../example.yaml`](../../example.yaml)

## Лицензия

MIT.
