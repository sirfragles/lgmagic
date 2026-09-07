# LG Magic Remote (MR20) — драйвер Linux, демон и нативные инструменты на C

**Язык:** [English🇬🇧](README.md) **Русский🇷🇺**

![LG Magic Remote](images/lg_magic_remote.png)

## Обзор

Этот проект — **нативный наследник на C** оригинального проекта LG Magic
Remote. Драйвер ядра Linux для пульта MR20 (Bluetooth HID-устройство
`000f:3412`) остаётся фундаментом, а оригинальный инструментарий на
Python **полностью заменён на C**: один бинарник `lg-magic` (только
libc/libm) и один системный демон `lg-magicd` (sd-bus + polkit).

Архитектура v2 — разделение **«тонкое ядро, толстый userspace»**:

```
                        raw_only=1 (по умолчанию)
 ┌──────────────┐  декодирует отчёты LG    ┌────────────────────────────┐
 │ lg_magic.ko  │ ───────────────────────→ │ evdev "LG Magic Remote"    │
 │ (ядро)       │  кнопки (lg_btn_map),    │   EV_KEY + REL_WHEEL       │
 │              │  колесо→REL_WHEEL,       │ evdev "LG Magic Remote IMU"│
 │              │  IMU (EV_ABS),           │   EV_ABS + MSC counter     │
 │              │  БЕЗ airmouse             └──────┬──────────┬──────────┘
 └──────────────┘                                 │ IMU (никогда не
                                                  │  захватывается!)
                                                  ▼          │ EVIOCGRAB
                                        ┌──────────────────┴─────────────┐
                                        │ lg-magicd (root, systemd)      │
                                        │  профиль · калибровка · карта  │
                                        │  колесо×scroll_speed · airmouse│
                                        │  состояние: /var/lib/lg-magic/ │
                                        └───┬──────────────────┬─────────┘
                               uinput     │                  │  sd-bus (org.lgmagic)
                   ┌───────── "lg-magicd keyboard <identity>" ─┼── polkitd
                   ▼                                          ▼
            на пульт: одна виртуальная мышь          CLI `lg-magic`
            + клавиатура (имя содержит MAC)  (libc/libm, свой D-Bus клиент)
```

Что это означает на практике:

- **Пульт работает сам по себе.** Ядро декодирует кнопки и колесо
  напрямую (`raw_only=1`, по умолчанию в v2) — пульт продолжает
  работать, даже когда демон не запущен.
- **Демон добавляет остальное.** `lg-magicd` захватывает evdev
  клавиатуры (только *после* того, как его виртуальные устройства
  готовы) и добавляет поверх airmouse, профили, маппинг кнопок,
  скорость колеса и калибровку — через **одну виртуальную мышь + одну
  виртуальную клавиатуру на каждый пульт** — пара названа по
  идентификатору пульта (`lg-magicd keyboard <MAC>` /
  `lg-magicd mouse <MAC>`, `unknown`, если MAC не читается), а
  зажатые клавиши отслеживаются отдельно для каждого пульта. Когда
  демон останавливается — включая падения — захват снимается вместе с
  его файловыми дескрипторами, и пульт возвращается к сырому вводу от
  ядра.
- **Ноль зависимостей у CLI** — `lg-magic` собран только на libc/libm и
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

### Модуль ядра (`lg_magic.ko`)

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

### Демон `lg-magicd`

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
  (`"2.0"`) позволяет клиентам проверять совместимость; evdev-устройство
  IMU никогда не захватывается, поэтому `lg-magic imu` работает
  параллельно

### Бинарник `lg-magic`

| Подкоманда | Назначение |
|---|---|
| `lg-magic analyze` | Декодирование отчётов HIDRAW пульта (замена `lg_magic.py`) |
| `lg-magic imu` | Чтение IMU через evdev: сырой вывод, запись `--csv`, ориентация `--ahrs`, куб `--cube` в терминале, airmouse `--mouse` через uinput |
| `lg-magic calibrate` | Подбор калибровки акселерометра (Левенберг–Марквардт) / гироскопа из CSV-записи |
| `lg-magic calib2bin` | Преобразование калибровочного JSON в 32-байтовый blob прошивки ядра |
| `lg-magic config` | Просмотр / изменение конфигурации TOML (включая `migrate` из JSON v1) |
| `lg-magic setup` | Интерактивный мастер: выбор режима, настройка, калибровка, установка — всё сразу |
| `lg-magic device` | `list` устройств демона / `status` устройства (root не нужен) |
| `lg-magic profile` | `list` профилей, `show` активного, `set` — переключение (polkit: profile-set) |
| `lg-magic button` | `list` карты клавиш, `map`/`reset` кнопок (polkit: modify-input) |
| `lg-magic scroll` | Скорость колеса и чувствительность airmouse (polkit: profile-set) |
| `lg-magic diagnose` | Сбор версии, ядра, модуля, устройств, конфига и состояния демона в отчёт для багрепортов |

## Требования

- **Linux** с работающим ядром
- **DKMS** и **заголовки ядра** — для сборки модуля (пакеты делают это
  автоматически; на Fedora на целевой машине нужен совпадающий
  `kernel-devel`, на Arch `dkms` ставится из AUR)
- **libsystemd** (библиотека времени выполнения для `lg-magicd`) и
  **polkit** (рекомендуется; без работающего polkitd демон отклоняет
  все изменяющие вызовы, кроме вызовов от root)
- **gcc + make** — только при сборке из исходников

Сам бинарник `lg-magic` во время работы не требует ничего, кроме
libc/libm.

## Установка

### 1. Пакеты релиза (рекомендуется)

Скачайте пакет для своего дистрибутива с последнего
[GitHub Release](https://github.com/sirfragles/lgmagic/releases):

```bash
# Ubuntu / Debian
sudo apt install ./lg-magic-dkms_2.0.1-1_amd64.deb

# Fedora
sudo dnf install ./lg-magic-2.0.1-1.fc42.x86_64.rpm

# Arch
sudo pacman -U ./lg-magic-2.0.1-1-x86_64.pkg.tar.zst
```

Пакеты ставят `/usr/bin/lg-magic`, `/usr/libexec/lg-magicd`
(`/usr/lib/lg-magicd` на Arch), юнит systemd, политику polkit,
конфигурацию D-Bus, TOML-конфиг по умолчанию, udev-правило и дерево
исходников DKMS — и регистрируют модуль в DKMS, который собирает
`lg_magic.ko` под ваше ядро и пересобирает его при обновлениях ядра.
Демон **не запускается автоматически**: запустите мастер — он его
включит.

### 2. Сборка из исходников

```bash
make              # модуль ядра + lg-magic + lg-magicd
make check        # сборка инструментов и полный набор тестов
sudo make install # бинарники, юнит, политика, dbus conf, tmpfiles, конфиг
sudo modprobe lg_magic
```

### 3. Ручная установка DKMS

```bash
sudo mkdir -p /usr/src/lg-magic-2.0.1
sudo cp Makefile dkms.conf COPYING /usr/src/lg-magic-2.0.1/
sudo cp -r kernel include /usr/src/lg-magic-2.0.1/
sudo dkms add -m lg-magic -v 2.0.1
sudo dkms build -m lg-magic -v 2.0.1
sudo dkms install -m lg-magic -v 2.0.1
# DKMS собирает только модуль — инструменты ставятся отдельно:
make tools && sudo make install
```

## Быстрый старт

После установки запустите мастер — он проведёт через выбор режима
ввода, калибровку, настройку демона и тест airmouse:

```bash
sudo lg-magic setup
```

Шаги мастера:

1. **Проверка окружения** — root, загруженный модуль, найденные
   устройства (включая `/dev/uinput`)
2. **Режим ввода** — по умолчанию в v2 **режим демона**
   (`raw_only=1 imu_evdev=1`, включает `lg-magicd`), либо поведение v1
   **airmouse в ядре** (`raw_only=0 airmouse=1 imu_evdev=1`)
3. **Параметры модуля** — записывает `/etc/modprobe.d/lg-magic.conf` и
   перезагружает модуль
4. **Калибровка акселерометра** — «медленно вращайте пульт по всем
   осям» (запись 20 с, подгонка Левенберга–Марквардта, проверка
   качества)
5. **Калибровка гироскопа** — «положите пульт и не трогайте его»
   (запись 10 с, средний bias)
6. **Настройка калибровки** — вопросы про LPF alpha и чувствительность
7. **Blob прошивки** — **только в режиме airmouse в ядре**:
   `lg_magic_calib_XX_XX_XX_XX_XX_XX.bin` для Bluetooth MAC вашего
   пульта (+ запасной `lg_magic_calib.bin`) в `/lib/firmware/`. В
   режиме демона шаг пропускается намеренно — калибровочный JSON
   является единственным источником, а blob был бы второй, устаревающей
   копией.
8. **Состояние демона** (в режиме демона) — записывает
   `/var/lib/lg-magic/<MAC>/calibration.json` и
   `/etc/lg-magic/devices.d/<MAC>.toml`, включает `lg-magicd`
   (`systemctl enable --now`, по возможности); записи
   (`calib_accel.csv` / `calib_gyro.csv`) лежат рядом с JSON в
   `/var/lib/lg-magic/<MAC>/`
9. **Перезагрузка модуля** — проверка через `dmesg` («Loading LG Magic
   calibration»)
10. **Тест airmouse** — «двигайте пультом, Ctrl+C завершает» (в режиме
    демона читает статус демона, с запасным автономным тестом)
11. **Пользовательская конфигурация** — `~/.config/lg-magic/config.toml`
    с путём калибровки и настройками airmouse
12. **Итог** — что было сделано и как это повторить или отменить

`--non-interactive` принимает все значения по умолчанию (для
скриптов).

## Использование

Запустите `lg-magic --help` или `lg-magic <подкоманда> --help` для
подробностей.

```bash
# Анализатор пакетов HIDRAW (автоопределение пульта по VID/PID 000f:3412)
lg-magic analyze                        # или --device /dev/hidrawN / --list

# Сырой вывод IMU (автоопределение evdev-устройства "IMU")
lg-magic imu

# Запись сырых отсчётов для калибровки
lg-magic imu --csv samples.csv --duration 20

# Углы ориентации (Madgwick AHRS) / куб в терминале (подразумевает --ahrs)
lg-magic imu --calib calib.json --ahrs
lg-magic imu --calib calib.json --cube

# Автономный airmouse через uinput (нужно udev-правило + группа input, или root)
lg-magic imu --calib calib.json --mouse

# Калибровка из записи
lg-magic calibrate samples.csv calib_accel.json --accel
lg-magic calibrate samples.csv calib_gyro.json --gyro

# 32-байтовый blob прошивки
lg-magic calib2bin calib.json lg_magic_calib.bin --alpha 0.2 --mouse_k 0.5
sudo cp lg_magic_calib.bin /lib/firmware/

# Конфигурация (TOML)
lg-magic config                    # действующая конфигурация
lg-magic config set mouse_k 0.5    # сохранить в ~/.config/lg-magic/config.toml
lg-magic config migrate            # импорт config.json из v1 в TOML
lg-magic config path               # расположение файлов конфигурации

# Демон (чтение без sudo; записи проверяются polkit)
lg-magic device list                # пульты, которыми управляет демон
lg-magic device status              # или lg-magic device status <MAC>
lg-magic profile list               # профили устройства по умолчанию
lg-magic profile set <MAC> tv       # переключить профиль (polkit: profile-set)
lg-magic button list                # текущая карта клавиш
lg-magic button map <MAC> KEY_UP KEY_VOLUMEUP   # (polkit: modify-input)
lg-magic button reset <MAC>
lg-magic scroll speed <MAC> 2.0     # множитель колеса (polkit: profile-set)
lg-magic diagnose                   # отчёт для багрепортов
```

Если демон не запущен, подкоманды демона выводят
`lg-magicd is not running — try: sudo systemctl enable --now lg-magicd`.

### Конфигурация

Конфигурация CLI в v2 — TOML (`config migrate` импортирует JSON-файлы
v1 и оставляет их на месте). Приоритет: встроенные значения <
`/etc/lg-magic/config.toml` < `~/.config/lg-magic/config.toml` <
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

- `/etc/lg-magic/devices.d/<MAC>.toml` — настройки каждого пульта,
  пишутся мастером (администратором):
  ```toml
  profile = "default"
  calib = "/var/lib/lg-magic/AA_BB_CC_DD_EE_FF/calibration.json"
  airmouse = true

  [profiles.default]
  scroll_speed = 1.0
  sensitivity = 30.0

  [profiles.default.button_map]
  "KEY_ENTER" = "BTN_LEFT"     # нажатие колеса -> клик мыши
  ```
- `/var/lib/lg-magic/state.toml` — активный профиль каждого устройства,
  принадлежит демону (атомарная запись)
- После ручных правок — `Reload()`: `lg-magic button reset <MAC>` тоже
  перезагружает, или `sudo systemctl reload lg-magicd`

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

1. **Запись** сырых отсчётов: `lg-magic imu --csv samples.csv`
2. **Подгонка** акселерометра (во время записи медленно вращайте пульт
   во всех ориентациях):
   `lg-magic calibrate samples.csv calib_accel.json --accel`
3. **Подгонка** гироскопа (пульт лежит неподвижно):
   `lg-magic calibrate samples.csv calib_gyro.json --gyro`
4. **Объедините** секции `accel` и `gyro` в один JSON; задайте
   `gyro.scale` разумное значение (около `0.07`, см. значение по
   умолчанию `gyro_scale_default`)
5. **Преобразуйте и установите**: `lg-magic calib2bin calib.json …` +
   копирование в `/lib/firmware/` (см. выше), затем
   `sudo modprobe -r lg_magic && sudo modprobe lg_magic`

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
sudo modprobe lg_magic raw_only=1 imu_evdev=1 debug=1
# Или постоянно в /etc/modprobe.d/lg-magic.conf (мастер записывает его сам)
# Или на лету через sysfs
echo 0 > /sys/module/lg_magic/parameters/debug
```

Обновление с v1: параметр по умолчанию меняется на `raw_only=1`, что
отключает airmouse в ядре. Мастер предлагает выбор режима — выберите
«airmouse в ядре», и он запишет `raw_only=0 airmouse=1`, сохранив
настройку v1.

## Раскладка файловой системы

| Путь | Содержимое |
|---|---|
| `/usr/bin/lg-magic` | бинарник инструментов |
| `/usr/libexec/lg-magicd` | демон (`/usr/lib/lg-magicd` на Arch) |
| `/usr/lib/systemd/system/lg-magicd.service` | юнит systemd |
| `/usr/lib/tmpfiles.d/lg-magic.conf` | каталог `/var/lib/lg-magic` |
| `/usr/share/polkit-1/actions/org.lgmagic.policy` | две акции polkit |
| `/usr/share/dbus-1/system.d/org.lgmagic.conf` | политика D-Bus демона |
| `/lib/modules/$(uname -r)/kernel/drivers/input/misc/lg_magic.ko` | модуль (через DKMS) |
| `/usr/src/lg-magic-2.0.1/` | дерево исходников DKMS |
| `/etc/udev/rules.d/51-lgimu.rules` | udev-правила (IMU evdev, hidraw, uinput) |
| `/etc/modprobe.d/lg-magic.conf` | параметры модуля (записывает мастер) |
| `/etc/lg-magic/config.toml` | системный конфиг CLI (conffile) |
| `/etc/lg-magic/devices.d/<MAC>.toml` | настройки демона для каждого пульта |
| `/etc/lg-magic/calib.json` | калибровочный JSON (по умолчанию мастера) |
| `/var/lib/lg-magic/state.toml` | активные профили (принадлежит демону) |
| `/var/lib/lg-magic/<MAC>/calibration.json` | калибровка пульта (демон) |
| `/lib/firmware/lg_magic_calib.bin` | blob калибровки, общий запасной |
| `/lib/firmware/lg_magic_calib_XX_XX_XX_XX_XX_XX.bin` | blob калибровки, по пульту (BT MAC) |
| `~/.config/lg-magic/config.toml` | пользовательский конфиг CLI |

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

- `lg-magic analyze` автоопределяет пульт по VID/PID вместо
  зашитого `/dev/hidraw7`
- `lg-magic imu --cube` подразумевает `--ahrs` (один `--cube` в Python
  показывал статичный куб)
- калибровка только `--gyro` записывает единичную коррекцию
  акселерометра вместо пустых массивов (пустые массивы ломали `--ahrs`)
- `--duration` и `--print-calib` — расширения

Скрипты остаются в `scripts/` только как эталон и генератор эталонных
данных; они больше не часть поддерживаемого рабочего процесса.

## Структура проекта

```
├── kernel/            # модуль ядра (lg_magic.ko)
├── include/           # lg_magic_calib.h — структура калибровки,
│                      #   дословно общая между ядром и userspace
├── tools/
│   ├── src/           # lg-magic (multi-call, libc/libm) + lg-magicd
│   ├── include/       # внутренние заголовки
│   └── tests/         # юнит / паритет / smoke / e2e тесты
├── data/              # config.toml, юнит, политика polkit, dbus conf, tmpfiles
├── testdata/          # эталонные фикстуры (сгенерированы скриптами Python)
├── debian/            # пакетирование Debian/Ubuntu (lg-magic-dkms)
├── rpm/               # пакетирование Fedora (lg-magic.spec)
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

Copyright © 2025 [Ilya Chelyadin]. Проект не связан с LG Electronics.
