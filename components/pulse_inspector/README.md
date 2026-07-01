# pulse_inspector (`PulseInspector`)

Базовый hub-компонент семейства PulseInspector. Прозрачно пропускает
цифровой сигнал с входного GPIO на выходной **в ISR** (IRAM-safe) и
параллельно складывает каждый фронт в очередь FreeRTOS для дочерних
декодеров и инструментов.

English version: [README.en.md](README.en.md).

## Возможности

- Несколько независимых **каналов** (пары `input_pin` → `output_pin`).
- Зеркалирование уровня в прерывании (задержка микросекундного порядка).
- Очередь `PulseItem` на канал + фоновая задача.
- Публичный API для дочерних компонентов:
  - `add_on_pulse_callback()`
  - `add_on_packet_callback()` / `trigger_packet()` (для декодеров).
- Диагностика: счётчики edges / overflow каждые 5 с (`DEBUG`).

## Требования

- ESP32, фреймворк **`esp-idf`** (не Arduino).
- Рекомендуется `build_flags: -DCONFIG_GPIO_CTRL_FUNC_IN_IRAM` в
  `platformio_options`, если ISR активен во время flash-операций.

## Установка

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
      invert_in: false
      invert_out: false
      queue_size: 256
    - id: ch_sniff
      input_pin: GPIO18   # только приём, без output_pin
```

## Параметры канала

| Параметр | Тип | По умолчанию | Описание |
|---|---|---|---|
| `input_pin` | pin | обязательный | Вход. Pull-up, прерывание по любому фронту. |
| `output_pin` | pin | — | Выход зеркалирования. Без него — receive-only. |
| `invert_in` | bool | `false` | Инверсия входа. |
| `invert_out` | bool | `false` | Инверсия выхода. |
| `queue_size` | int 16…8192 | `256` | Глубина очереди импульсов. |

## Метаданные ESPHome

| Поле | Значение |
|---|---|
| `CODEOWNERS` | `@anat0l` |
| `DEPENDENCIES` | `esp32` |
| `MULTI_CONF` | `false` (один hub на устройство) |

## Дочерние компоненты

Любой модуль может подписаться на канал по `id`:

```yaml
pulse_inspector_logger:
  - channel_id: ch_a
```

См. [шаблон](../pulse_inspector_logger/README.md) и
[индекс компонентов](../README.md).

## Диагностика в логах

```
[D][pulse_inspector] ch0 GPIO16: edges=128 (+42) overflows=0 (+0) processed=128
```

Рост `overflows` — увеличьте `queue_size` или снизьте нагрузку на линию.

## Лицензия

MIT.
