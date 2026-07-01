# Внешние компоненты ESPHome: семейство PulseInspector

English version: [README.en.md](README.en.md).

Набор внешних компонентов для ESP32 (фреймворк **ESP-IDF**). Базовый
хаб [`pulse_inspector`](pulse_inspector/README.md) (`PulseInspector`)
прозрачно зеркалирует GPIO→GPIO на уровне ISR и отдаёт поток фронтов
дочерним модулям через callback API.

## Компоненты

| Компонент | YAML-ключ | Назначение | README |
|---|---|---|---|
| **PulseInspector** | `pulse_inspector` | Hub: каналы input→output, ISR, очередь импульсов | [pulse_inspector/README.md](pulse_inspector/README.md) |
| **PulseInspector Logger** | `pulse_inspector_logger` | Шаблон дочернего компонента | [pulse_inspector_logger/README.md](pulse_inspector_logger/README.md) |
| **PulseInspector VCD** | `pulse_inspector_vcd` | Сетевой логический анализатор (VCD/TCP) | [pulse_inspector_vcd/README.md](pulse_inspector_vcd/README.md) |
| **PulseInspector MDB** | `pulse_inspector_mdb` | Декодер MDB + HA | [pulse_inspector_mdb/README.md](pulse_inspector_mdb/README.md) |
| **PulseInspector EXE** | `pulse_inspector_exe` | Декодер линии Executive | [pulse_inspector_exe/README.md](pulse_inspector_exe/README.md) |

## Подключение с GitHub

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/Anat0l/esphome-pulse-inspector
      ref: main
      path: components
    components:
      - pulse_inspector
      - pulse_inspector_mdb   # только нужные модули
```

## Быстрый старт

```yaml
esp32:
  framework:
    type: esp-idf

external_components:
  - source:
      type: git
      url: https://github.com/Anat0l/esphome-pulse-inspector
      ref: main
      path: components
    components:
      - pulse_inspector
      - pulse_inspector_logger

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

Полный пример: [`../example.yaml`](../example.yaml).

## Лицензия

MIT.
