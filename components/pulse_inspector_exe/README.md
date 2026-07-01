# pulse_inspector_exe (`PulseInspectorExe`)

Декодер линии **Necta Executive** (MEI Protocol A) поверх
[`pulse_inspector`](../pulse_inspector/README.md). Два канала GPIO
(executive / peripheral) декодируются как **8E1 UART** (9600 baud, even
parity). Публикует сырые кадры, семантику кредита/vend и счётчики в
лог, triggers и HA sensors.

Физически тот же edge-driven UART-движок, что у MDB (11 бит-таймов на
байт); отличается интерпретация 9-го бита (чётность, а не master/slave).

English version: [README.en.md](README.en.md).

## Терминология каналов

| YAML | Роль |
|---|---|
| `executive_channel` | Линия, с которой executive шлёт команды (0x31 STATUS, 0x32 CREDIT). |
| `peripheral_channel` | Линия ответов VMC / ASU / CPP. |

Индексы — позиции в `pulse_inspector.channels` (0…15).

## Установка

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [pulse_inspector, pulse_inspector_exe]

pulse_inspector:
  id: inspector
  channels:
    - id: ch_exe_exec
      input_pin: GPIO16
      output_pin: GPIO17
    - id: ch_exe_periph
      input_pin: GPIO18
      output_pin: GPIO19

pulse_inspector_exe:
  id: exe
  inspector_id: inspector
  executive_channel: 0
  peripheral_channel: 1
  parity: even
  suppress_idle_polls: true
  on_vend_complete:
    - logger.log:
        format: "Vend %s value=%u"
        args: ['ok ? "OK" : "FAIL"', 'value']
```

## Основные параметры

| Параметр | По умолчанию | Описание |
|---|---|---|
| `inspector_id` | — | Родительский `PulseInspector`. |
| `executive_channel` | обязательный | Индекс канала executive TX. |
| `peripheral_channel` | обязательный | Индекс канала ответов. |
| `baud` | `9600` | Скорость. |
| `parity` | `even` | `none` / `even` / `odd` / `mark` / `space`. |
| `inter_byte_timeout` | `0us` (auto) | Группировка байтов в кадр. |
| `suppress_idle_polls` | `true` | Фильтр idle 0x31/0x32 ↔ 0x00/0xFE. |
| `vmc_online_timeout` | `5s` | Таймаут «VMC offline». |
| `idle_master_frames` / `idle_slave_frames` | MEI defaults | Паттерны для подавления. |

## YAML-триггеры

- `on_master_frame` / `on_slave_frame` — `(data, mode, len, suppressed)`
- `on_vend_complete` — `(ok, value, selection_price, audit_pairs_pending)`
- `on_credit_change` — `(credit, delta, base_units, exact_change_only)`
- `on_vmc_status` — `(status_raw, vending_inhibited, free_vend, audit_pairs)`

## Платформы Home Assistant

```yaml
sensor:
  - platform: pulse_inspector_exe
    exe_id: exe
    current_credit:
      name: VMC credit
    vends_ok_total:
      name: Vends OK
```

Диагностика: `frames_decoded`, `framing_errors`, `parity_errors`,
`master_bytes`, `slave_bytes`, `idle_polls`, `events_dropped`
(переполнение очереди кадров и импульсы при нехватке mutex).

Семантика: `current_credit`, `current_credit_base_units`,
`current_selection_price`, `last_vend_value`, `vends_ok_total`,
`vends_failed_total`, `money_inserted_total`, `money_change_total`,
`audit_pairs_pending`, `last_audit_address`, `last_audit_value`,
`scaling_factor`.

Также `binary_sensor` и `text_sensor` — см. исходники.

## Метаданные ESPHome

| Поле | Значение |
|---|---|
| `CODEOWNERS` | `@anat0l` |
| `DEPENDENCIES` | `esp32`, `pulse_inspector` |
| `MULTI_CONF` | `true` |

## Лицензия

MIT.
