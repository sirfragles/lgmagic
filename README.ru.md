# LG Magic Remote (MR20) — драйвер Linux, демон и нативные инструменты на C

**Язык:** [English🇬🇧](README.md) **Русский🇷🇺**

![LG Magic Remote](images/lgmagic_remote.png)

## Обзор

Этот проект — **нативный наследник на C** оригинального проекта LG Magic
Remote. Драйвер ядра Linux для пульта MR20 (Bluetooth HID-устройство
`000f:3412`) остаётся фундаментом, а оригинальный инструментарий на
Python **полностью заменён на C**: один бинарник `lgmagic` (только
libc/libm) и один системный демон `lgmagicd` (sd-bus + polkit).

Архитектура v2 — разделение **«тонкое ядро, толстый userspace»**:

```
                        raw_only=1 (по умолчанию)
 ┌──────────────┐  декодирует отчёты LG    ┌────────────────────────────┐
 │ lgmagic.ko  │ ───────────────────────→ │ evdev "LG Magic Remote"    │
 │ (ядро)       │  кнопки (lg_btn_map),    │   EV_KEY + REL_WHEEL       │
 │              │  колесо→REL_WHEEL,       │ evdev "LG Magic Remote IMU"│
 │              │  IMU (EV_ABS),           │   EV_ABS + MSC counter     │
 │              │  БЕЗ airmouse             └──────┬──────────┬──────────┘
 └──────────────┘                                 │ IMU (никогда не
                                                  │  захватывается!)
                                                  ▼          │ EVIOCGRAB
                                        ┌──────────────────┴─────────────┐
                                        │ lgmagicd (root, systemd)      │
                                        │  профиль · калибровка · карта  │
                                        │  колесо×scroll_speed · airmouse│
                                        │  состояние: /var/lib/lgmagic/ │
                                        └───┬──────────────────┬─────────┘
                               uinput     │                  │  sd-bus (org.lgmagic)
                   ┌───────── "lgmagicd keyboard <identity>" ─┼── polkitd
                   ▼                                          ▼
            на пульт: одна виртуальная мышь          CLI `lgmagic`
            + клавиатура (имя содержит MAC)  (libc/libm, свой D-Bus клиент)
```

Что это означает на практике:

- **Пульт работает сам по себе.** Ядро декодирует кнопки и колесо
  напрямую (`raw_only=1`, по умолчанию в v2) — пульт продолжает
  работать, даже когда демон не запущен.
- **Демон добавляет остальное.** `lgmagicd` захватывает evdev
  клавиатуры (только *после* того, как его виртуальные устройства
  готовы) и добавляет поверх airmouse, профили, маппинг кнопок,
  скорость колеса и калибровку — через **одну виртуальную мышь + одну
  виртуальную клавиатуру на каждый пульт** — пара названа по
  идентификатору пульта (`lgmagicd keyboard <MAC>` /
  `lgmagicd mouse <MAC>`, `unknown`, если MAC не читается), а
  зажатые клавиши отслеживаются отдельно для каждого пульта. Когда
  демон останавливается — включая падения — захват снимается вместе с
  его файловыми дескрипторами, и пульт возвращается к сырому вводу от
  ядра.
- **Ноль зависимостей у CLI** — `lgmagic` собран только на libc/libm и
  общается с демоном по D-Bus через небольшой собственный клиент (без
  libsystemd, без Python). Демон линкует libsystemd (sd-bus) и пишет
  логи в journal.
- **Привилегированные записи идут через демон + polkit.** Две акции:
  `org.lgmagic.profile-set` (разрешена активным сессиям) и
  `org.lgmagic.modify-input` (административная аутентификация).
  Чтение состояния устройств не требует root. Настройки живут в
  `/etc` и `/var`, никогда в `/usr`; глобальный конфиг BlueZ не
  затрагивается.
- **Пакеты для трёх дистрибутивов** — `.deb` (Ubuntu/Debian), `.rpm`
  (Fedora) и PKGBUILD для Arch, все с регистрацией модуля в DKMS;
  сборка и релиз через CI.
- **Побайтовая совместимость** с оригинальными скриптами Python,
  проверенная по закоммиченным эталонным данным (см. TESTING.md).

Оригинальные скрипты Python остаются в `scripts/` только как эталон.

## Возможности

### Модуль ядра (`lgmagic.ko`)

- Полное декодирование кнопок (питание, цифры, навигация, медиа,
  цветные кнопки) статической картой клавиш — не зависит от режима,
  идентично v1
- `raw_only=1` (по умолчанию): колесо отдаёт **только** `REL_WHEEL` без
  эмуляции клавиш, airmouse в ядре не генерируется
- `raw_only=0`: полное поведение v1 (airmouse в ядре с калибровкой
  bias/scale и низкочастотной фильтрацией, эмуляция клавиш колеса /
  BTN_LEFT в режиме airmouse)
- Отдельное evdev-устройство `LG Magic Remote IMU` с сырыми данными
  6 осей (акселерометр + гироскоп) и аппаратным счётчиком-меткой
  (при `raw_only=1` присутствует всегда)
- Калибровка из `/lib/firmware` (по Bluetooth MAC каждого пульта, с
  общим запасным вариантом)

### Демон `lgmagicd`

- Находит evdev-устройства клавиатуры и IMU пульта (сопряжение по
  Bluetooth MAC из sysfs), следит за hotplug
- Захватывает клавиатуру **после** создания виртуальной мыши и
  клавиатуры — перезапуск или падение никогда не оставляет пульт
  мёртвым
- Профили по устройствам: маппинг кнопок, скорость колеса,
  чувствительность, путь калибровки — применяются сразу, без
  перезапуска
- Airmouse через демон со знаками, масштабом и LPF как в v1
- sd-bus интерфейс (`org.lgmagic`, `/org/lgmagic/Manager`) с проверкой
  polkit на каждом изменяющем методе; идентификаторы устройств
  валидируются («unknown» или BT MAC из 17 символов — всё остальное
  даёт `org.lgmagic.Error.InvalidArguments`), а свойство `ApiVersion`
  (`"0.0"`) позволяет клиентам проверять совместимость; evdev-устройство
  IMU никогда не захватывается, поэтому `lgmagic imu` работает
  параллельно

### Бинарник `lgmagic`

| Подкоманда | Назначение |
|---|---|
| `lgmagic analyze` | Декодирование отчётов HIDRAW пульта (замена `lgmagic.py`) |
| `lgmagic imu` | Чтение IMU через evdev: сырой вывод, запись `--csv`, ориентация `--ahrs`, куб `--cube` в терминале, airmouse `--mouse` через uinput |
| `lgmagic calibrate` | Подбор калибровки акселерометра (Левенберг–Марквардт) / гироскопа из CSV-записи |
| `lgmagic calib2bin` | Преобразование калибровочного JSON в 32-байтовый blob прошивки ядра |
| `lgmagic config` | Просмотр / изменение конфигурации TOML (включая `migrate` из JSON v1) |
| `lgmagic setup` | Интерактивный мастер: выбор режима, настройка, калибровка, установка — всё сразу |
| `lgmagic device` | `list` устройств демона / `status` устройства (root не нужен) |
| `lgmagic profile` | `list` профилей, `show` активного, `set` — переключение (polkit: profile-set) |
| `lgmagic button` | `list` карты клавиш, `map`/`reset` кнопок (polkit: modify-input) |
| `lgmagic scroll` | Скорость колеса и чувствительность airmouse (polkit: profile-set) |
| `lgmagic diagnose` | Сбор версии, ядра, модуля, устройств, конфига и состояния демона в отчёт для багрепортов |

## Требования

- **Linux** с работающим ядром
- **DKMS** и **заголовки ядра** — для сборки модуля (пакеты делают это
  автоматически; на Fedora на целевой машине нужен совпадающий
  `kernel-devel`, на Arch `dkms` ставится из AUR)
- **libsystemd** (библиотека времени выполнения для `lgmagicd`) и
  **polkit** (рекомендуется; без работающего polkitd демон отклоняет
  все изменяющие вызовы, кроме вызовов от root)
- **gcc + make** — только при сборке из исходников

Сам бинарник `lgmagic` во время работы не требует ничего, кроме
libc/libm.

## Установка

### 1. Пакеты релиза (рекомендуется)

Скачайте пакет для своего дистрибутива с последнего
[GitHub Release](https://github.com/sirfragles/lgmagic/releases).
Каждый Release содержит по одному `.deb` на поддерживаемую версию
дистрибутива (Ubuntu 22.04/24.04/26.04, Debian 12/13 — суффикс ревизии
`~<дистрибутив>` называет его), по одному `.rpm` на версию Fedora
(43/44) и пакет Arch:

```bash
# Ubuntu 24.04 (выберите ~суффикс вашей версии)
sudo apt install ./lgmagic-dkms_0.0.1-1~ubuntu24.04_amd64.deb

# Fedora 44 (43 тоже лежит на странице Release)
sudo dnf install ./lgmagic-0.0.1-1.fc44.x86_64.rpm

# Arch
sudo pacman -U ./lgmagic-0.0.1-1-x86_64.pkg.tar.zst
```

Пакеты ставят `/usr/bin/lgmagic`, `/usr/libexec/lgmagicd`
(`/usr/lib/lgmagicd` на Arch), юнит systemd, политику polkit,
конфигурацию D-Bus, TOML-конфиг по умолчанию, udev-правило и дерево
исходников DKMS — и регистрируют модуль в DKMS, который собирает
`lgmagic.ko` под ваше ядро и пересобирает его при обновлениях ядра.
Демон **не запускается автоматически**: запустите мастер — он его
включит.

### 2. Сборка из исходников

```bash
make              # модуль ядра + lgmagic + lgmagicd
make check        # сборка инструментов и полный набор тестов
sudo make install # бинарники, юнит, политика, dbus conf, tmpfiles, конфиг
sudo modprobe lgmagic
```

### 3. Ручная установка DKMS

```bash
sudo mkdir -p /usr/src/lgmagic-0.0.1
sudo cp Makefile dkms.conf COPYING /usr/src/lgmagic-0.0.1/
sudo cp -r kernel include /usr/src/lgmagic-0.0.1/
sudo dkms add -m lgmagic -v 0.0.1
sudo dkms build -m lgmagic -v 0.0.1
sudo dkms install -m lgmagic -v 0.0.1
# DKMS собирает только модуль — инструменты ставятся отдельно:
make tools && sudo make install
```

## Быстрый старт

После установки запустите мастер — он проведёт через выбор режима
ввода, калибровку, настройку демона и тест airmouse:

```bash
sudo lgmagic setup
```

Шаги мастера:

1. **Проверка окружения** — root, загруженный модуль, найденные
   устройства (включая `/dev/uinput`)
2. **Режим ввода** — по умолчанию в v2 **режим демона**
   (`raw_only=1 imu_evdev=1`, включает `lgmagicd`), либо поведение v1
   **airmouse в ядре** (`raw_only=0 airmouse=1 imu_evdev=1`)
3. **Параметры модуля** — записывает `/etc/modprobe.d/lgmagic.conf` и
   перезагружает модуль
4. **Калибровка акселерометра** — «медленно вращайте пульт по всем
   осям» (запись 20 с, подгонка Левенберга–Марквардта, проверка
   качества)
5. **Калибровка гироскопа** — «положите пульт и не трогайте его»
   (запись 10 с, средний bias)
6. **Настройка калибровки** — вопросы про LPF alpha и чувствительность
7. **Blob прошивки** — **только в режиме airmouse в ядре**:
   `lgmagic_calib_XX_XX_XX_XX_XX_XX.bin` для Bluetooth MAC вашего
   пульта (+ запасной `lgmagic_calib.bin`) в `/lib/firmware/`. В
   режиме демона шаг пропускается намеренно — калибровочный JSON
   является единственным источником, а blob был бы второй, устаревающей
   копией.
8. **Состояние демона** (в режиме демона) — записывает
   `/var/lib/lgmagic/<MAC>/calibration.json` и
   `/etc/lgmagic/devices.d/<MAC>.toml`, включает `lgmagicd`
   (`systemctl enable --now`, по возможности); записи
   (`calib_accel.csv` / `calib_gyro.csv`) лежат рядом с JSON в
   `/var/lib/lgmagic/<MAC>/`
9. **Перезагрузка модуля** — проверка через `dmesg` («Loading LG Magic
   calibration»)
10. **Тест airmouse** — «двигайте пультом, Ctrl+C завершает» (в режиме
    демона читает статус демона, с запасным автономным тестом)
11. **Пользовательская конфигурация** — `~/.config/lgmagic/config.toml`
    с путём калибровки и настройками airmouse
12. **Итог** — что было сделано и как это повторить или отменить

`--non-interactive` принимает все значения по умолчанию (для
скриптов).

## Использование

Запустите `lgmagic --help` или `lgmagic <подкоманда> --help` для
подробностей.

```bash
# Анализатор пакетов HIDRAW (автоопределение пульта по VID/PID 000f:3412)
lgmagic analyze                        # или --device /dev/hidrawN / --list

# Сырой вывод IMU (автоопределение evdev-устройства "IMU")
lgmagic imu

# Запись сырых отсчётов для калибровки
lgmagic imu --csv samples.csv --duration 20

# Углы ориентации (Madgwick AHRS) / куб в терминале (подразумевает --ahrs)
lgmagic imu --calib calib.json --ahrs
lgmagic imu --calib calib.json --cube

# Автономный airmouse через uinput (нужно udev-правило + группа input, или root)
lgmagic imu --calib calib.json --mouse

# Калибровка из записи
lgmagic calibrate samples.csv calib_accel.json --accel
lgmagic calibrate samples.csv calib_gyro.json --gyro

# 32-байтовый blob прошивки
lgmagic calib2bin calib.json lgmagic_calib.bin --alpha 0.2 --mouse_k 0.5
sudo cp lgmagic_calib.bin /lib/firmware/

# Конфигурация (TOML)
lgmagic config                    # действующая конфигурация
lgmagic config set mouse_k 0.5    # сохранить в ~/.config/lgmagic/config.toml
lgmagic config migrate            # импорт config.json из v1 в TOML
lgmagic config path               # расположение файлов конфигурации

# Демон (чтение без sudo; записи проверяются polkit)
lgmagic device list                # пульты, которыми управляет демон
lgmagic device status              # или lgmagic device status <MAC>
lgmagic profile list               # профили устройства по умолчанию
lgmagic profile set <MAC> tv       # переключить профиль (polkit: profile-set)
lgmagic button list                # текущая карта клавиш
lgmagic button map <MAC> KEY_UP KEY_VOLUMEUP   # (polkit: modify-input)
lgmagic button reset <MAC>
lgmagic scroll speed <MAC> 2.0     # множитель колеса (polkit: profile-set)
lgmagic diagnose                   # отчёт для багрепортов
```

Если демон не запущен, подкоманды демона выводят
`lgmagicd is not running — try: sudo systemctl enable --now lgmagicd`.

### Конфигурация

Конфигурация CLI в v2 — TOML (`config migrate` импортирует JSON-файлы
v1 и оставляет их на месте). Приоритет: встроенные значения <
`/etc/lgmagic/config.toml` < `~/.config/lgmagic/config.toml` <
`--config FILE` < флаги CLI.

| Ключ | Тип | По умолчанию | Значение |
|---|---|---|---|
| `imu_device` | строка | автопоиск | путь evdev устройства IMU |
| `hidraw_device` | строка | автопоиск | путь hidraw пульта |
| `default_calib` | строка | — | калибровочный JSON для `--calib` |
| `lpf_alpha` | число | 0.2 | низкочастотный фильтр для `--mouse` |
| `mouse_scale` | число | 30.0 | скорость указателя для `--mouse` |
| `madgwick_beta` | число | 0.1 | усиление фильтра Маджвика |
| `alpha` | число | 0.2 | LPF airmouse (записывается в blob) |
| `mouse_k` | число | 0.5 | чувствительность airmouse (записывается в blob) |
| `gyro_scale_default` | число | 0.07 | рекомендуемый масштаб гироскопа (см. калибровку) |

### Конфигурация демона

Демон читает свои файлы (никогда пользовательский конфиг):

- `/etc/lgmagic/devices.d/<MAC>.toml` — настройки каждого пульта,
  пишутся мастером (администратором):
  ```toml
  profile = "default"
  calib = "/var/lib/lgmagic/AA_BB_CC_DD_EE_FF/calibration.json"
  airmouse = true

  [profiles.default]
  scroll_speed = 1.0
  sensitivity = 30.0

  [profiles.default.button_map]
  "KEY_ENTER" = "BTN_LEFT"     # нажатие колеса -> клик мыши
  ```
- `/var/lib/lgmagic/state.toml` — активный профиль каждого устройства,
  принадлежит демону (атомарная запись)
- После ручных правок — `Reload()`: `lgmagic button reset <MAC>` тоже
  перезагружает, или `sudo systemctl reload lgmagicd`

Приоритет: встроенные значения < `devices.d` < `state.toml` (активный
профиль). Повреждённый файл калибровки отклоняется с записью в journal
— прежняя калибровка продолжает действовать, демон работает дальше.

### polkit

Две акции покрывают все изменяющие методы:

| Акция | Методы | Активная сессия | Прочие сессии |
|---|---|---|---|
| `org.lgmagic.profile-set` | SetProfile, SetScrollSpeed, SetSensitivity | разрешено, без пароля | отказано |
| `org.lgmagic.modify-input` | MapButton, ResetButtons, SetCalibPath, Reload | пароль (auth_admin_keep) | пароль |

Все чтения (`device list/status`, `profile list`, `button list`,
`diagnose`) свободны от polkit. Без работающего polkitd демон
отказывает закрыто: авторизован только root.

### Ручная калибровка (без мастера)

1. **Запись** сырых отсчётов: `lgmagic imu --csv samples.csv`
2. **Подгонка** акселерометра (во время записи медленно вращайте пульт
   во всех ориентациях):
   `lgmagic calibrate samples.csv calib_accel.json --accel`
3. **Подгонка** гироскопа (пульт лежит неподвижно):
   `lgmagic calibrate samples.csv calib_gyro.json --gyro`
4. **Объедините** секции `accel` и `gyro` в один JSON; задайте
   `gyro.scale` разумное значение (около `0.07`, см. значение по
   умолчанию `gyro_scale_default`)
5. **Преобразуйте и установите**: `lgmagic calib2bin calib.json …` +
   копирование в `/lib/firmware/` (см. выше), затем
   `sudo modprobe -r lgmagic && sudo modprobe lgmagic`

## Параметры модуля

| Параметр | Значения | Описание |
|---|---|---|
| `raw_only` | 0/1 | по умолчанию в v2 **1**: чистое декодирование — без airmouse в ядре, колесо только как `REL_WHEEL` (остальное добавляет демон). `0` = поведение v1 1:1 |
| `airmouse` | 0/1 | включить airmouse в ядре (имеет смысл только при `raw_only=0`) |
| `airmouse_threshold` | int | порог гироскопа, активирующий управление указателем (по умолчанию 300) |
| `imu_evdev` | 0/1 | отдавать сырой IMU как отдельное evdev-устройство |
| `debug` | 0–2 | подробность (0 = тихо … 2 = подробно) |

```bash
# При загрузке
sudo modprobe lgmagic raw_only=1 imu_evdev=1 debug=1
# Или постоянно в /etc/modprobe.d/lgmagic.conf (мастер записывает его сам)
# Или на лету через sysfs
echo 0 > /sys/module/lgmagic/parameters/debug
```

Обновление с v1: параметр по умолчанию меняется на `raw_only=1`, что
отключает airmouse в ядре. Мастер предлагает выбор режима — выберите
«airmouse в ядре», и он запишет `raw_only=0 airmouse=1`, сохранив
настройку v1.

## Раскладка файловой системы

| Путь | Содержимое |
|---|---|
| `/usr/bin/lgmagic` | бинарник инструментов |
| `/usr/libexec/lgmagicd` | демон (`/usr/lib/lgmagicd` на Arch) |
| `/usr/lib/systemd/system/lgmagicd.service` | юнит systemd |
| `/usr/lib/tmpfiles.d/lgmagic.conf` | каталог `/var/lib/lgmagic` |
| `/usr/share/polkit-1/actions/org.lgmagic.policy` | две акции polkit |
| `/usr/share/dbus-1/system.d/org.lgmagic.conf` | политика D-Bus демона |
| `/lib/modules/$(uname -r)/kernel/drivers/input/misc/lgmagic.ko` | модуль (через DKMS) |
| `/usr/src/lgmagic-0.0.1/` | дерево исходников DKMS |
| `/etc/udev/rules.d/51-lgimu.rules` | udev-правила (IMU evdev, hidraw, uinput) |
| `/etc/modprobe.d/lgmagic.conf` | параметры модуля (записывает мастер) |
| `/etc/lgmagic/config.toml` | системный конфиг CLI (conffile) |
| `/etc/lgmagic/devices.d/<MAC>.toml` | настройки демона для каждого пульта |
| `/etc/lgmagic/calib.json` | калибровочный JSON (по умолчанию мастера) |
| `/var/lib/lgmagic/state.toml` | активные профили (принадлежит демону) |
| `/var/lib/lgmagic/<MAC>/calibration.json` | калибровка пульта (демон) |
| `/lib/firmware/lgmagic_calib.bin` | blob калибровки, общий запасной |
| `/lib/firmware/lgmagic_calib_XX_XX_XX_XX_XX_XX.bin` | blob калибровки, по пульту (BT MAC) |
| `~/.config/lgmagic/config.toml` | пользовательский конфиг CLI |

## Разработка

```bash
make              # модуль + инструменты + демон
make check        # юнит-тесты, тесты паритета, smoke-тесты CLI (см. TESTING.md)
make clean
```

Набор тестов (475 проверок в 14 бинарниках) покрывает переносимое ядро
— TOML, JSON, матричную/кватернионную математику, round-trip CSV,
фильтр Маджвика по эталонной трассе, подгонку Левенберга–Марквардта
(включая перекрёстную проверку аналитического и численного якобиана),
blob калибровки побайтово против `struct.pack` из Python, карты
клавиш, профили, сопряжение, пайплайн airmouse с паритетом v1 и
D-Bus-клиент против фальшивой шины. В CI на Linux гарнесс
фальшивого устройства прогоняет всю поверхность демона сквозным
тестом: маппинг, профили, калибровку (включая отклонение
повреждённого файла калибровки), колесо, airmouse, отказ polkit,
переподключение устройства и снятие EVIOCGRAB после SIGKILL, SIGTERM и
SIGINT. Подробности: [TESTING.md](TESTING.md).

CI запускается на каждый push и pull request четырьмя задачами:
**Ubuntu** (сборка, тесты, e2e, сборка .deb + установка + проверка
DKMS), **Fedora** (rpmbuild в контейнере), **Arch** (makepkg в
контейнере) и **macOS** (переносимые юнит-тесты под clang). Тег
релиза (`v*`) собирает все три пакета и прикрепляет их к GitHub
Release.

### Связь с оригинальными скриптами Python

Инструменты на C воспроизводят поведение оригинальных скриптов в
точности — включая формат CSV, матрицу выравнивания, константы
фильтров и раскладку blob прошивки — и проверяются по эталонным
данным, сгенерированным реализациями на Python. Несколько осознанных,
задокументированных улучшений:

- `lgmagic analyze` автоопределяет пульт по VID/PID вместо
  зашитого `/dev/hidraw7`
- `lgmagic imu --cube` подразумевает `--ahrs` (один `--cube` в Python
  показывал статичный куб)
- калибровка только `--gyro` записывает единичную коррекцию
  акселерометра вместо пустых массивов (пустые массивы ломали `--ahrs`)
- `--duration` и `--print-calib` — расширения

Скрипты остаются в `scripts/` только как эталон и генератор эталонных
данных; они больше не часть поддерживаемого рабочего процесса.

## Структура проекта

```
├── kernel/            # модуль ядра (lgmagic.ko)
├── include/           # lgmagic_calib.h — структура калибровки,
│                      #   дословно общая между ядром и userspace
├── tools/
│   ├── src/           # lgmagic (multi-call, libc/libm) + lgmagicd
│   ├── include/       # внутренние заголовки
│   └── tests/         # юнит / паритет / smoke / e2e тесты
├── data/              # config.toml, юнит, политика polkit, dbus conf, tmpfiles
├── testdata/          # эталонные фикстуры (сгенерированы скриптами Python)
├── debian/            # пакетирование Debian/Ubuntu (lgmagic-dkms)
├── rpm/               # пакетирование Fedora (lgmagic.spec)
├── arch/              # пакетирование Arch (PKGBUILD + .install)
├── scripts/           # оригинальные инструменты Python (устаревший эталон)
├── dkms.conf          # конфигурация DKMS (только модуль)
├── Makefile           # верхнеуровневая сборка
└── .github/workflows/ # автоматизация CI + релизов
```

## Совместимость

- **Проверено с**: LG Magic Remote MR20
- **Версии ядра**: 4.15+ (проверено на 6.11)
- **Архитектуры**: пакеты и DKMS собираются под текущее ядро
  (x86_64/arm64, little-endian)

## Лицензия

GPL-2.0-or-later — как у ядра Linux. Порт алгоритма Madgwick AHRS в
`tools/src/madgwick.c` основан на реализации в общественном достоянии
С. Маджвика (x-io.co.uk).

Copyright © 2025 Ilya Chelyadin, 2026 sirfragles. Проект основан на
lg-magic (https://github.com/brainrom/lg-magic) Ильи Челядина и не
связан с LG Electronics.
