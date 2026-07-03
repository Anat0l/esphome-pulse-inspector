# esphome pulse_inspector (PulseInspector)

Внешний компонент ESPHome для ESP32 (фреймворк ESP-IDF), реализующий
прозрачный многоканальный GPIO-инспектор импульсов (`PulseInspector`). Каждый канал зеркалит уровень
с входного пина на выходной прямо из IRAM-обработчика прерывания, а
каждый фронт складывается в очередь FreeRTOS для фоновой обработки —
туда же удобно встраивать модификацию трафика. Универсальная база без
привязки к конкретному протоколу.

English version: [README.en.md](README.en.md).

## Семейство компонентов

| Компонент | Назначение | Документация |
|---|---|---|
| `pulse_inspector` | Hub: GPIO pass-through + очередь импульсов | [components/pulse_inspector/README.md](components/pulse_inspector/README.md) |
| `pulse_inspector_logger` | Шаблон дочернего компонента | [components/pulse_inspector_logger/README.md](components/pulse_inspector_logger/README.md) |
| `pulse_inspector_vcd` | Сетевой VCD-анализатор | [components/pulse_inspector_vcd/README.md](components/pulse_inspector_vcd/README.md) |
| `pulse_inspector_mdb` | Декодер MDB + HA | [components/pulse_inspector_mdb/README.md](components/pulse_inspector_mdb/README.md) |
| `pulse_inspector_exe` | Декодер линии Executive | [components/pulse_inspector_exe/README.md](components/pulse_inspector_exe/README.md) |

Индекс: [components/README.md](components/README.md).

## Статус

Базовый hub (`pulse_inspector`):

- Прозрачное зеркалирование GPIO→GPIO в ISR, несколько каналов, callback API.
- Диагностика фронтов и переполнений очереди.

Дочерние модули (см. таблицу): VCD, декодеры MDB/Executive, шаблон logger.

Пока не реализовано:

- Универсальный API модификации трафика (инъекция пакетов из HA).

## Требования

- ESP32 (любая разновидность, поддерживаемая ESP-IDF).
- ESPHome с `framework: type: esp-idf`. Фреймворк Arduino **не**
  поддерживается: компонент напрямую использует примитивы ESP-IDF
  (`gpio_install_isr_service`, очереди и задачи FreeRTOS, прямой доступ к
  регистрам GPIO).

## Конфигурация

Подключите компонент через `external_components`:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/Anat0l/esphome-pulse-inspector
      ref: main
      path: components
    components: [pulse_inspector]
```

Затем опишите один или несколько каналов:

```yaml
pulse_inspector:
  id: inspector
  channels:
    - id: ch_a
      input_pin: GPIO16
      output_pin: GPIO17
    - id: ch_b
      input_pin: GPIO18
      output_pin: GPIO19
      invert_in: true
      invert_out: true
      queue_size: 512
    - id: ch_rx
      input_pin: GPIO21   # только приём, без зеркалирования
```

### Параметры канала

| Параметр | Тип | По умолчанию | Описание |
|---|---|---|---|
| `input_pin` | pin | обязательный | Пин, который перехватываем. Настраивается как вход с прерыванием по любому фронту. |
| `output_pin` | pin | опционально | Пин, на который зеркалится уровень входа. Не указывайте для каналов «только на приём». |
| `invert_in` | bool | `false` | Инвертировать логический уровень, прочитанный с `input_pin`. |
| `invert_out` | bool | `false` | Инвертировать логический уровень, записываемый в `output_pin`. |
| `queue_size` | int (16..8192) | `256` | Глубина очереди FreeRTOS для `PulseItem`. Увеличьте, если в диагностике появляются переполнения. |
| `pull` | `up` / `down` / `none` | `up` | Внутренняя подтяжка входа. `none` — при внешнем делителе/оптроне. |
| `min_pulse_width` | time (0..1000us) | `0us` | Фильтр дребезга: импульсы короче порога отбрасываются до раздачи дочерним компонентам. `0us` — выключен. |

Полный рабочий пример см. в [`example.yaml`](example.yaml).

## Диагностика

Каждые 5 секунд компонент выводит на уровне `DEBUG` по одной строке на
канал:

```
[pulse_inspector] ch0 GPIO16: edges=1234 (+42) overflows=0 (+0) processed=1234
```

- `edges` / `+N` с прошлой строки — сколько фронтов увидел ISR.
- `overflows` / `+N` — сколько фронтов было потеряно из-за переполнения
  очереди. Если значение растёт, увеличьте `queue_size` или уменьшите
  интенсивность трафика на линии.
- `processed` — сколько элементов успела обработать фоновая задача.

## Заметки по устройству

- `gpio_install_isr_service(ESP_INTR_FLAG_IRAM)` устанавливается один раз
  на весь компонент и корректно переживает `ESP_ERR_INVALID_STATE`.
- ISR работает через прямой доступ к регистрам (`GPIO.in`, `GPIO.out_w1ts` /
  `GPIO.out_w1tc`), поэтому безопасен во время операций с flash.
- Фоновая задача канала вызывает зарегистрированные callback'и для каждого
  `PulseItem`. Дочерние компоненты подписываются через
  `PulseInspectorChannel::add_on_pulse_callback(...)`.
- Метка времени `PulseItem.t_us` — 64-битные микросекунды от
  `esp_timer_get_time()`: одинаково на Xtensa и RISC-V, не зависит от
  `cpu_frequency`, без переполнений на тихих линиях.

## Создание компонентов поверх `pulse_inspector`

`pulse_inspector` задуман как parent/hub: дочерние компоненты (декодеры,
инъекторы, логгеры) прицепляются к каналу по `id` и получают каждый импульс
через callback.

Шаблон: [`components/pulse_inspector_logger`](components/pulse_inspector_logger).

Подробнее о подключении модулей с GitHub — в
[components/README.md](components/README.md).

### Публичный API канала

- `add_on_pulse_callback(PulseCallback)` — каждый фронт (контекст задачи канала).
- `add_on_packet_callback(PacketCallback)` / `trigger_packet(data, len)` — для декодеров.
- `get_input_gpio_num()`, `get_output_gpio_num()`, `has_output()` — аксессоры.

### Замечания про потоки

В callback'е можно делать быструю протокольную логику. Публикацию в HA и
сеть переносите в `loop()` через `defer()` / `App.scheduler` / `std::atomic`.

## Лицензия

MIT.
