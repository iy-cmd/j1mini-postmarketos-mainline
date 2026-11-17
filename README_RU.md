# Mainline Linux 7.3 и postmarketOS для Samsung Galaxy J1 Mini 3G (`SM-J105H` / `SM-J105F`)

[English version (README.md)](README.md)

Полноценный порт **Mainline Linux 7.3.1** и **postmarketOS (Alpine Linux + SXMO i3)** для смартфона **Samsung Galaxy J1 Mini 3G** (`j1mini3g`, `SM-J105H`, `SM-J105F`, Spreadtrum/Unisoc SC7730 / SC8830 / SC7727S).

Включает аппаратное ускорение графики **Mali-400 (Lima)** с плавным VSync-композитингом без артефактов, рабочий звук (динамик, наушники, микрофон) через **PipeWire**, мультитач **MELFAS MIP4**, масштабированный под 480×800 интерфейс **SXMO i3**, лёгкий браузер **GNOME Web** и USB-сеть с раздачей интернета с ПК.

---

## Таблица поддержки железа

| Компонент | Статус | Примечания |
|---|---|---|
| **CPU (4× Cortex-A7)** | Работает | Все 4 ядра активны в SMP (`7.3.1-j1mini+`) |
| **Дисплей (`S6D77A1` 480×800)** | Работает | Аппаратный контроллер `sprd-dispc` @ 58 Гц, атомарный VSYNC page-flip |
| **GPU (`Mali-400 MP1`)** | Работает | Mainline `lima` (OpenGL 2.1 / GLES 2.0, Mesa 26.2) + Xorg Glamor + `picom` VSync |
| **Тачскрин (`MELFAS MIP4`)** | Работает | Мультитач по I2C (`0x48`), `libinput`, жесты SXMO |
| **Звук (Динамик, Наушники, Микрофон, Jack)** | Работает | VBC r2p0 + AP DMA + кодек SC2723, профили ALSA UCM2 + **PipeWire / WirePlumber** |
| **Аппаратные кнопки** | Работает | Power, Home, Громкость +/- (**Громкость Вниз** открывает/закрывает клавиатуру `svkbd`) |
| **Память (8 ГБ eMMC + microSD)** | Работает | Полный раздел `SYSTEM` 2.1 ГБ (`mmcblk0p25`) + файл подкачки 384 МБ |
| **USB Gadget (Сеть + Serial)** | Работает | `CDC-NCM` Ethernet (`192.168.7.2`) + `CDC-ACM` консоль (`/dev/ttyACM0`), DWC2 PIO |
| **Интернет по USB** | Работает | Авто-NAT + DNS + `TTL=66` (работает даже при раздаче с мобильных точек доступа) |
| **PMIC / Батарея / Fuel Gauge** | Работает | Регуляторы SC2723, EIC, ADC, FGU |
| **Wi-Fi (`Spreadtrum SC2331 / Marlin 1`)** | Не поддерживается | SDIO `0000:2331` — нет mainline-драйвера, требуются закрытые прошивки Android |
| **Сотовый модем / Bluetooth** | Не поддерживается | Закрытый стек модема Spreadtrum CP / WCN |

---

## Характеристики устройства

- **Устройство**: Samsung Galaxy J1 Mini 3G (`j1mini3g`, `SM-J105H` / `SM-J105F`, `hw_revision=2`)
- **SoC**: Spreadtrum / Unisoc SC8830 / SC7727S (семейство `SC7730`, 28 нм HPm)
- **CPU**: 4 ядра ARM Cortex-A7 @ 1.2 ГГц
- **GPU**: ARM Mali-400 MP1 @ 384 МГц (32 КБ L2-кэш)
- **ОЗУ**: 768 МБ LPDDR2 (715 МБ доступно Linux + 384 МБ swap)
- **PMIC / Аудиокодек**: Spreadtrum SC2723 (шина ADI)
- **Дисплей**: 4.0" 480×800 TFT LCD (`S6D77A1`, MIPI DSI video mode, 96 DPI)
- **Память**: 8 ГБ eMMC 5.0 (`mmcblk0p20` = `KERNEL`, `mmcblk0p21` = `RECOVERY`, `mmcblk0p25` = `SYSTEM`)

---

## Готовые образы для прошивки (GitHub Releases)

Все готовые образы скачиваются со страницы [**Releases**](../../releases):

| Файл релиза | Назначение |
|---|---|
| `postmarketos-j1mini-odin.tar.md5` | **Полный пакет для Odin в 1 клик** (`boot.img` + sparse `system.img`) — шить в слот **AP** |
| `postmarketos-j1mini-twrp.zip` | **Полный ZIP для TWRP в 1 клик** (ядро Mainline 7.3.1 + готовая система postmarketOS) |
| `boot.img` | Сырой образ ядра для раздела `KERNEL` (`/dev/mmcblk0p20`) |
| `boot.tar.md5` | Только ядро для Odin (слот **AP**) |
| `j1mini-mainline-twrp.zip` | Только ядро для TWRP Recovery |
| `SHA256SUMS.txt` | Контрольные суммы SHA-256 всех файлов |

---

## Установка postmarketOS

> **Внимание**: прошивка заменяет Android в разделах `KERNEL` (`mmcblk0p20`) и `SYSTEM` (`mmcblk0p25`). Сделайте резервную копию важных данных! TWRP в разделе `RECOVERY` (`mmcblk0p21`) не затрагивается.

### Способ 1: Samsung Odin / Heimdall / Thor (Рекомендуется)

1. Выключите телефон.
2. Зайдите в **Download Mode**: зажмите `Громкость Вниз + Home + Питание`, затем нажмите `Громкость Вверх`.
3. Подключите телефон к ПК по USB.
4. Откройте **Odin3** (Windows) или **Thor / Heimdall** (Linux).
5. Укажите `postmarketos-j1mini-odin.tar.md5` в поле **AP** и нажмите **Start**.
6. Телефон прошьёт ядро и систему postmarketOS и автоматически перезагрузится в SXMO i3.

### Способ 2: TWRP Recovery (в 1 клик)

1. Скопируйте `postmarketos-j1mini-twrp.zip` на SD-карту или отправьте через `adb push postmarketos-j1mini-twrp.zip /tmp/`.
2. Загрузитесь в **TWRP** (`Громкость Вверх + Home + Питание`).
3. Нажмите **Install** → выберите `postmarketos-j1mini-twrp.zip` → свайп для прошивки.
4. Нажмите **Reboot System**.

### Способ 3: Быстрое обновление ядра напрямую по SSH (без TWRP и Odin)

Когда postmarketOS уже запущена и подключена по USB (`192.168.7.2`), новое ядро можно прошить одной командой с автоматической проверкой SHA-256:

```bash
./scripts/flash_ssh.sh out_build/boot.img root@192.168.7.2
```

*(Скрипт снимает read-only флаг GPT командой `blockdev --setrw /dev/mmcblk0p20`, записывает `boot.img`, сверяет хеш и перезагружает телефон.)*

---

## Вход и управление

- **Логин**: `shatalovie`
- **Пароль**: `1234` *(рекомендуется сменить командой `passwd` после первого входа)*
- **Оболочка**: SXMO (Simple X Mobile) на `i3` + VSync-композитор `picom`
- **Браузер**: GNOME Web (`epiphany 50.6`), настроенный под 480×800 и Mali-400
- **Установленные утилиты**: `st`, `tmux`, `fish`, `htop`, `btop`, `mpv`, `ffmpeg`, `mesa-demos`
- **Экранная клавиатура**: кнопка **Громкость Вниз** открывает и закрывает `svkbd` (`480×280` внизу экрана).
- **Жесты SXMO**: свайпы от краёв экрана открывают меню, переключают окна и закрывают приложения.

---

## Подключение по USB, SSH и раздача интернета

При подключении к ПК по USB телефон поднимает два устройства:

1. **USB Ethernet (`CDC-NCM`)** — адрес телефона `192.168.7.2/24`
2. **USB Serial консоль (`CDC-ACM`)** — `/dev/ttyACM0` (`115200 8N1`)

### Подключение по SSH

```bash
sudo ./scripts/setup_host_network.sh
ssh shatalovie@192.168.7.2
```

### Автоматическая раздача интернета с Linux-ПК (NetworkManager)

1. Создайте постоянный профиль NetworkManager с раздачей сети (`shared`):
   ```bash
   sudo nmcli con add type ethernet con-name j1mini-usb-nat \
     ifname "*" mac 02:00:00:73:56:02 \
     ipv4.method shared ipv4.addresses 192.168.7.1/24 \
     ipv6.method link-local
   ```
2. Установите dispatcher-скрипт (разрешает форвардинг через `DOCKER-USER` при установленном Docker и пробрасывает DNS на текущий шлюз хоста):
   ```bash
   sudo install -m 755 scripts/host-usb-internet-dispatcher.sh \
     /etc/NetworkManager/dispatcher.d/99-j1mini-usb-forward
   ```
3. На самом телефоне скрипт `/etc/local.d/usb-internet.start` автоматически выставляет маршрут по умолчанию `192.168.7.1`, DNS `192.168.7.1` и `net.ipv4.ip_default_ttl=66` (для обхода ограничений TTL мобильных точек доступа).

---

## Сборка ядра из исходников

Требуется **Podman** (рекомендуется) или **Docker** на Linux:

```bash
./build_from_source.sh
```

Скрипт автоматически:
1. Соберёт контейнер с кросс-компилятором ARM (`debian:trixie-slim` + `gcc-arm-linux-gnueabihf`).
2. Скачает Linux `v7.3.1` в постоянный volume (`j1mini-linux-src`) и применит патчи SC7730/SC8830.
3. Наложит все правки J1 Mini из `overlay/`, `source/sc7730-samsung-j1mini.dts` и `source/j1mini_defconfig`.
4. Соберёт готовые образы в `out_build/`: `boot.img`, `boot.tar.md5` и `j1mini-mainline-twrp.zip`.

---

## Основные технические исправления в порте

1. **Графика и дисплей без артефактов (`sprd-dispc` + `lima` + `picom`)**:
   - Включён аппаратный контроллер дисплея `&dispc` (`480×800 @ 58 Гц`) с `cached_buffers = false` (когерентный DMA, чтобы CPU-кэш не портил рендер Mali-400 при сканировании экрана).
   - Исправлена гонка page-flip в `sprd_dispc.c`: теневые регистры слоёв пишутся до взвода события, а затем `d->event`, очистка `INT_UPDATE_DONE`, `wmb()` и `DPI_REG_UPDATE` выполняются атомарно под `event_lock` с резервным завершением по `INT_VSYNC`.
   - Подключён минимальный VSync-композитор `picom` (`backend = "xrender"` поверх Glamor/Mali-400, `vsync = true`, `unredir-if-possible = false`), полностью убравший тиринг, моргания и мелкие тайловые артефакты.
   - Убран отладочный спам `pr_info` на каждое касание в `melfas_mip4.c`.
2. **Чистый звук через PipeWire (VBC r2p0 + AP DMA + SC2723)**:
   - Включены `&ap_dma`, `&vbc`, `&audio_pcm` и `&sc2723_codec` (`simple-audio-card`, детекция наушников на `pmic_eic 8`).
   - PulseAudio (дававший срывы DMA и громкий писк на 160-фреймовом FIFO VBC) полностью вырезан и заменён на **PipeWire + WirePlumber + `pipewire-pulse`**.
   - Квант PipeWire и период ALSA подобраны точно кратно FIFO VBC (`1600` фреймов @ `44100 Гц`), добавлены профили ALSA UCM2 (`Samsung/j1mini`).
3. **Адаптация под 480×800 и лёгкий браузер**:
   - Настроен ровный `96×96 DPI` (`DisplaySize 127 212`), компактные шрифты в `Xresources` / `i3` / `conky` / GTK3 / GTK4, экранная клавиатура `svkbd` (`-g 480x280+0+520`) привязана к кнопке Громкость Вниз.
   - Вместо тяжёлого Firefox установлен **GNOME Web (`epiphany 50.6`)** с оптимальными флагами под Mali-400 OpenGL 2.1 (`GDK_DISABLE=gl,vulkan`, `GSK_RENDERER=cairo`, `WEBKIT_DISABLE_DMABUF_RENDERER=1`, `WEBKIT_DISABLE_COMPOSITING_MODE=1`, `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1`) — вся система с открытым браузером потребляет ~170 МБ ОЗУ.

---

## Лицензия

Патчи и оверлеи ядра распространяются под лицензией **GPL-2.0** (как и ядро Linux). Скрипты и конфигурационные файлы — под лицензией MIT.
