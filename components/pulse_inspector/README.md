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
      type: git
      url: https://github.com/Anat0l/esphome-pulse-inspector
      ref: main
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
      pull: none          # внешний делитель/оптрон — внутренняя подтяжка не нужна
      min_pulse_width: 20us  # отбрасывать иглы короче 20 мкс
```

## Параметры канала

| Параметр | Тип | По умолчанию | Описание |
|---|---|---|---|
| `input_pin` | pin | обязательный | Вход. Прерывание по любому фронту. |
| `output_pin` | pin | — | Выход зеркалирования. Без него — receive-only. |
| `invert_in` | bool | `false` | Инверсия входа. |
| `invert_out` | bool | `false` | Инверсия выхода. |
| `queue_size` | int 16…8192 | `256` | Глубина очереди импульсов. |
| `pull` | `up` / `down` / `none` | `up` | Внутренняя подтяжка входа (~45 кОм). При внешнем делителе или оптроне ставьте `none`. |
| `min_pulse_width` | time 0…1000us | `0us` (выкл.) | Фильтр дребезга: импульсы короче порога отбрасываются в задаче канала до раздачи подписчикам. Полезно на длинных / шумных линиях. |

### Временная база

Каждый фронт получает метку `PulseItem.t_us` — 64-битные микросекунды от
`esp_timer_get_time()`. Метка одинакова на всех вариантах ESP32 (Xtensa и
RISC-V), не зависит от `cpu_frequency` и не переполняется на практике.

> **Breaking change:** до этого исправления поле называлось
> `PulseItem.cycle` и на классическом ESP32 (Xtensa) содержало тики CPU,
> а на C3 — микросекунды. Если вы строили собственный компонент поверх
> `add_on_pulse_callback()`, замените `item.cycle` на `item.t_us` и
> уберите конверсию тиков.

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
