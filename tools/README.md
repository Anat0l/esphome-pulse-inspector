# Инструменты анализа VCD-захватов

ПК-скрипты для работы с файлами `.vcd`, которые отдаёт ESPHome-компонент
[`pulse_inspector_vcd`](../components/pulse_inspector_vcd/README.md) по TCP.

Они **не входят в прошивку** и не нужны для сборки ESPHome. Запускаются
локально на компьютере после записи захвата.

## Требования

- Python 3.8+
- Стандартная библиотека (дополнительные пакеты не нужны)
- Запись с устройства, на котором включён `pulse_inspector_vcd`

## Имена каналов в VCD

Каждый GPIO из `pulse_inspector` попадает в файл как отдельный сигнал вида
`chN_gpioM`, где `N` — индекс канала в YAML, `M` — номер пина.

Пример: `ch0_gpio16`, `ch2_gpio4`.

## Типичный порядок работы

```
ESP (pulse_inspector_vcd)  →  vcd_capture_tcp.py  →  capture.vcd
                                      ↓
                            vcd_analyze_uart.py   (подбор baud / frame)
                                      ↓
                            vcd_decode_uart.py    (просмотр байтов)
                                      ↓
                            vcd_analyze_mdb.py    (сводка MDB, опционально)
```

Для визуального просмотра формы сигнала тот же `.vcd` можно открыть в
**PulseView** или **GTKWave**.

---

## `vcd_capture_tcp.py` — запись захвата по TCP

### Назначение

Надёжный TCP-клиент для потока VCD с ESP. Пишет байты в файл без
буферизации платформы.

Рекомендуется вместо `ncat` / `nc` / перенаправления в PowerShell на
Windows: там поток иногда обрывается через несколько минут, и файл
получается меньше, чем реально отправил ESP (в логе устройства при этом
счётчик `bytes sent` продолжает расти).

### Синтаксис

```bash
python tools/vcd_capture_tcp.py <host> <port> <output.vcd> [опции]
```

### Позиционные аргументы

| Аргумент | Описание |
|----------|----------|
| `host` | IP-адрес ESP в локальной сети |
| `port` | TCP-порт из YAML (`pulse_inspector_vcd.port`, по умолчанию `9000`) |
| `output` | Путь к выходному файлу `.vcd` |

### Опции

| Опция | По умолчанию | Описание |
|-------|--------------|----------|
| `--duration N` | `0` | Остановиться через `N` секунд (`0` = до Ctrl+C или закрытия сокета устройством) |
| `--idle-timeout N` | `120` | Прервать, если `N` секунд не приходило ни одного байта |
| `--print-interval N` | `5` | Печатать прогресс в stderr каждые `N` секунд |

### Примеры

```bash
# Запись до Ctrl+C
python tools/vcd_capture_tcp.py 192.168.1.50 9000 capture.vcd

# Ровно 10 минут
python tools/vcd_capture_tcp.py 192.168.1.50 9000 capture.vcd --duration 600

# Короткий idle-timeout для диагностики «зависшего» потока
python tools/vcd_capture_tcp.py 192.168.1.50 9000 capture.vcd --idle-timeout 60
```

### Вывод и сверка

В stderr каждые 5 с (по умолчанию):

```
[client]   15.0s      123456 B  (   8230 B/s)  chunks=42
```

Сверьте `bytes` с логом ESPHome (`VCD stream: ... bytes sent ...`). Если
цифры расходятся — проблема на стороне сети или приёмника, а не прошивки.

### Завершение

| Причина | Поведение |
|---------|-----------|
| `Ctrl+C` | Корректная остановка, файл пригоден для анализа |
| Peer closed | Устройство закрыло TCP; в stderr — итоговый размер |
| Idle timeout | Нет данных дольше `--idle-timeout` |

---

## `vcd_analyze_uart.py` — подбор параметров UART

### Назначение

Офлайн-анализатор для **первичной настройки** линии перед правкой YAML и
декодеров (`pulse_inspector_mdb`, `pulse_inspector_exe`):

1. Читает VCD (timescale 1 µs, как у `pulse_inspector_vcd`).
2. По распределению интервалов между фронтами **оценивает baud**.
3. Перебирает раскладки кадра и выбирает ту, где меньше framing/parity
   ошибок.
4. По запросу выводит дамп первых байтов и группирует их в пакеты.

### Синтаксис

```bash
python tools/vcd_analyze_uart.py <capture.vcd> [опции]
```

### Опции

| Опция | По умолчанию | Описание |
|-------|--------------|----------|
| `--channel NAME` | все каналы | Имя сигнала из VCD (можно указывать несколько раз) |
| `--baud N` | авто | Зафиксировать скорость (иначе автоопределение) |
| `--frame LAYOUT` | перебор всех | Принудительная раскладка кадра (см. таблицу ниже) |
| `--dump N` | `0` | Вывести первые `N` декодированных байт лучшей раскладки |
| `--packets N` | `0` | Группировать дамп в пакеты с паузой ≥ `N` мкс (`0` = выкл.) |

### Поддерживаемые раскладки (`--frame`)

| Значение | Описание | Типичное применение |
|----------|----------|---------------------|
| `8N1` | 8 data, no parity, 1 stop | Обычный UART |
| `8N2` | 8 data, 1 stop × 2 | Редко |
| `9N1` | 8 data + 9-й бит, 1 stop | **MDB** (9-й бит = addr/data) |
| `9N2` | 9N1 + 2 stop | Редко |
| `8E1` | 8 data + even parity, 1 stop | **Necta Executive / MEI Protocol A** |
| `8E2` | 8E1 + 2 stop | Редко |
| `8O1` | 8 data + odd parity, 1 stop | Другие протоколы |

### Примеры

```bash
# Все каналы, авто baud, сравнение всех раскладок
python tools/vcd_analyze_uart.py capture.vcd

# Один канал
python tools/vcd_analyze_uart.py capture.vcd --channel ch2_gpio4

# Проверка гипотезы 9600 8E1 (Executive)
python tools/vcd_analyze_uart.py capture.vcd --channel ch3_gpio6 --baud 9600 --frame 8E1

# Дамп 200 байт, сгруппированных паузами ≥ 2 ms
python tools/vcd_analyze_uart.py capture.vcd --channel ch0_gpio16 --dump 200 --packets 2000
```

### Пример вывода

```
=== ch2_gpio4 (b): edges=18432, detected baud=9600
    8N1:  1234 bytes, framing_errors=  12 (  1.0%)
    9N1:  1230 bytes, framing_errors=   3 (  0.2%)
    8E1:  1234 bytes, framing_errors= 890 ( 72.1%), parity_errors= 890 ( 72.1%)
    best layout: 9N1
```

По строке `best layout` переносите параметры в YAML: `baud`, `invert_in`,
тип декодера (`mdb` vs `exe`).

### Маркеры в дампе (`--dump`)

| Суффикс | Значение |
|---------|----------|
| `!` | Framing error (stop bit ≠ 1) |
| `p` | Parity error (для `8E1` / `8O1`) |
| `(0)` / `(1)` | Значение 9-го бита / бита чётности |

---

## `vcd_decode_uart.py` — декодирование и печать байтов

### Назначение

Разбирает VCD, восстанавливает уровни на каждом канале и декодирует
асинхронный UART (start = спад, LSB first, stop = 1). Печатает байты с
меткой времени — удобно сопоставлять трафик master/slave на разных GPIO.

Может использоваться как **библиотека**: функции `parse_vcd()` и
`decode_uart()` импортируются из `vcd_analyze_mdb.py`.

### Синтаксис

```bash
python tools/vcd_decode_uart.py <capture.vcd> [опции]
```

### Опции

| Опция | По умолчанию | Описание |
|-------|--------------|----------|
| `--baud N` | `9600` | Скорость в бод |
| `--bits N` | `8` | Число data-бит: `5`–`9` |
| `--parity P` | `N` | `N` — нет, `E` — even, `O` — odd |
| `--invert TAG` | — | Инвертировать каналы, в имени которых есть подстрока `TAG` (можно повторять) |
| `--invert-all` | выкл. | Инвертировать все каналы |
| `--group-gap-us N` | `50000` | Пустая строка между кадрами с паузой > `N` мкс |
| `--ignore TAG` | — | Пропустить каналы с подстрокой `TAG` в имени |
| `--role` | выкл. | При `--bits 9` и `--parity N`: 9-й бит = master (`MSTR`) / slave (`slv`) |

### Примеры

```bash
# MDB 9N1, все каналы
python tools/vcd_decode_uart.py capture.vcd --baud 9600 --bits 9 --parity N --role

# Executive 8E1, инверсия линии с gpio6 в имени
python tools/vcd_decode_uart.py capture.vcd --baud 9600 --bits 8 --parity E --invert gpio6

# Только «чётные» каналы (ch0, ch2…), без ch1
python tools/vcd_decode_uart.py capture.vcd --bits 9 --role --ignore ch1
```

> **Примечание.** У этого скрипта нет отдельного `--channel`: нужный канал
> отфильтруйте через `--ignore` или смотрите только нужные строки в выводе.

### Пример вывода

```
     t, ms    dt, us  ch              role    byte  9b        bin  ascii  notes
--------------------------------------------------------------------------------
     125.3       520  ch0_gpio16      MSTR    0x33   1  00110011  3
     125.8       520  ch1_gpio18      slv     0x00   0  00000000  .
```

Колонка `dt` — интервал от предыдущего кадра. `ERR` — ошибка кадрирования
или чётности.

---

## `vcd_analyze_mdb.py` — сводка MDB-трафика

### Назначение

Быстрая статистика по захвату в предположении **MDB 9N1 @ 9600**:

- уникальные байты master (9-й бит = 1) и slave (9-й бит = 0);
- топ-10 самых частых значений;
- гистограмма пауз между кадрами.

Жёстко зашито: `baud=9600`, `data_bits=9`, `parity=N`; каналы с `ch1` в
имени инвертируются автоматически (историческое соглашение для MDB-tap).

### Синтаксис

```bash
python tools/vcd_analyze_mdb.py [capture.vcd]
```

Если путь не указан, используется `capture.vcd` в текущей директории.

Параметров командной строки нет — для тонкой настройки используйте
`vcd_decode_uart.py` или `vcd_analyze_uart.py`.

### Пример

```bash
python tools/vcd_analyze_mdb.py capture.vcd
```

### Пример вывода

```
== ch0_gpio16: 4521 frames, 4498 ok ==
  bytes with 9b=1 (addr/cmd): 12 unique
     0x133  (byte=0x33) x2100
     0x110  (byte=0x10) x45
  bytes with 9b=0 (data):     8 unique
     0x000  (byte=0x00) x1800
  top inter-frame gaps:
          520 us  x3200
         1000 us  x890
```

---

## Сценарии по протоколам

### MDB (bill validator, 9N1)

```bash
python tools/vcd_capture_tcp.py <ip> 9000 mdb.vcd --duration 120
python tools/vcd_analyze_uart.py mdb.vcd --channel ch0_gpio16
python tools/vcd_decode_uart.py mdb.vcd --baud 9600 --bits 9 --role --invert ch0
python tools/vcd_analyze_mdb.py mdb.vcd
```

Ожидаемый `best layout`: **9N1**. Частые байты `0x33` (POLL) и `0x00`
(ACK) — нормальная idle-рукопожатие MDB.

### Necta Executive / MEI Protocol A (8E1)

```bash
python tools/vcd_capture_tcp.py <ip> 9000 exe.vcd --duration 120
python tools/vcd_analyze_uart.py exe.vcd --channel ch3_gpio6 --baud 9600 --frame 8E1 --dump 100
python tools/vcd_decode_uart.py exe.vcd --baud 9600 --bits 8 --parity E --invert gpio6
```

Ожидаемый `best layout`: **8E1**. Idle-пара `0x31` / `0x32` (master) и
`0x00` / `0xFE` (peripheral).

---

## Связанная документация

- [pulse_inspector_vcd](../components/pulse_inspector_vcd/README.md) — настройка
  TCP-стрима, keepalive, типичные проблемы Wi-Fi
- [pulse_inspector_mdb](../components/pulse_inspector_mdb/README.md) — live-декодер MDB
- [pulse_inspector_exe](../components/pulse_inspector_exe/README.md) — live-декодер Executive
- [example.yaml](../example.yaml) — пример полной конфигурации
