# pulse_inspector_logger (`PulseInspectorLogger`)

Пример **дочернего** компонента поверх [`pulse_inspector`](../pulse_inspector/README.md).
Подписывается на один канал, считает фронты в контексте FreeRTOS-задачи
канала и периодически печатает сводку из `loop()` основного цикла ESPHome.

Используйте как **copy-paste шаблон** для своих декодеров.

English version: [README.en.md](README.en.md).

## Возможности

- Привязка к каналу через `channel_id: <id канала>`.
- Настраиваемый интервал отчёта (`report_interval`).
- Потокобезопасный счётчик (`std::atomic`) — правильный паттерн для
  callback'ов из задачи канала.

## Требования

- Родительский `pulse_inspector` с объявленным каналом.
- ESP32 + `esp-idf`.

## Установка

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
  - id: logger_a
    channel_id: ch_a
    report_interval: 5s
```

## Параметры

| Параметр | Тип | По умолчанию | Описание |
|---|---|---|---|
| `channel_id` | id | обязательный | `PulseInspectorChannel` из `pulse_inspector.channels`. |
| `report_interval` | time | `5s` | Как часто печатать сводку (`INFO`). |

## Метаданные ESPHome

| Поле | Значение |
|---|---|
| `CODEOWNERS` | `@anat0l` |
| `DEPENDENCIES` | `esp32`, `pulse_inspector` |
| `MULTI_CONF` | `true` (несколько логгеров на разные каналы) |

## Пример вывода

```
[I][pulse_inspector_logger] GPIO16: 42 edges in last 5000 ms (total=128, last_level=1)
```

## Как сделать свой декодер

1. Скопируйте каталог `pulse_inspector_logger` → `my_decoder`.
2. В `setup()` оставьте `channel_->add_on_pulse_callback(...)`.
3. В callback — быстрая логика (state machine); публикацию в HA — через
   `defer()` / `loop()` / `std::atomic`.

## Лицензия

MIT.
