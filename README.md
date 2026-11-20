# ADS-B on the LilyGO T3-S3 and T-Echo

## Bilingual firmware for LilyGO receivers

| Receiver | Firmware download | Guide and checksums |
| --- | --- | --- |
| **T-Deck Plus** · ESP32-S3, SX1262, 320 × 240 TFT | [Download the T-Deck Plus ZIP](https://github.com/111blackeagle111/sx1262-adsb/releases/download/tdeck-plus-v0.1.0/tdeck-plus-adsb.zip) | [Installation, features and SHA-256](https://github.com/111blackeagle111/sx1262-adsb/releases/tag/tdeck-plus-v0.1.0) |
| **Original T-Echo** · nRF52840, SX1262, 200 × 200 e-paper | [Download the T-Echo UF2](https://github.com/111blackeagle111/sx1262-adsb/releases/download/techo-v0.1.0/techo-adsb.uf2) | [Installation, features and SHA-256](https://github.com/111blackeagle111/sx1262-adsb/releases/tag/techo-v0.1.0) |

Both releases support English and Italian. The T-Deck Plus includes a traffic map, microSD replay and an RF activity indicator. The T-Echo includes manual standby with the Frecce Tricolori screen and can clear packet history when internal storage is nearly full. Built on [Salvatore Sanfilippo’s sx1262-adsb receiver](https://github.com/antirez/sx1262-adsb).

This firmware turns the **LilyGO T-Deck Plus** with its 320 × 240 color display, the **LilyGO T3-S3** with an OLED and the **original LilyGO T-Echo** with an e-paper display into pocket aircraft receivers. All three use an unmodified SX1262 radio (no hardware modifications needed). Connect an antenna to the LoRa antenna connector (even the stock antenna works) and power the board by USB or battery. It listens on 1090 MHz for the ADS-B messages in which aircraft broadcast their identity, position and movement. The display shows the flight's callsign or the aircraft's six-character address in large type. As reports arrive, the board fills in altitude, speed, direction and coordinates. The sampler, decoder and flight display logic are written in C. You can retrieve the recordings over USB after an outing. Note: **the technical details used in order to turn the SX1262 into an ADSB receiver are quite interesting, check them later**.

<img src="docs/receivers-outdoors.jpg" width="560" alt="Two handheld receivers outdoors. The T-Echo's e-paper screen shows flight RYR9JW and the T3-S3's OLED shows aircraft 4BAA79.">

The T-Echo shows **RYR9JW at 34,175 ft**. Beside it, the T3-S3 shows a position report from **4BAA79 at 21,150 ft**. The camera caught only a few lit OLED rows in each video frame, so we combined rows from neighboring frames to make the screen readable.

![Two pages from flight THY4DC on the LilyGO. One shows 1675 ft, 141 knots and a descent of 704 feet per minute. The other shows the aircraft's latitude and longitude.](docs/thy4dc-oled.png)

![Live view showing aircraft 4BAA79 at 21150 ft, alongside the frequency settings screen with its signal graph and Clear history option.](docs/live-and-settings-oled.png)

These are screenshots from the board. The upper pair shows the same packet three seconds apart as the flight details rotate. Below are the live view and the settings menu. [How to read the display](#reading-the-display)

1090 MHz is outside the SX1262's specified 150–960 MHz range. The radio can tune there on the tested board, but the board's RF front end attenuates the signal. Reception favors strong aircraft signals. One outdoor recording from aircraft `48C12B` yielded a groundspeed of 420 knots and a track of 203°. The receiver now decodes those same 256 power readings as a self-test at startup. [Semtech SX1262 specifications and datasheet](https://www.semtech.com/products/wireless-rf/lora-connect/sx1262) · [Recorded waveform and expected message](main/selftest.h)

## Install without compiling

Choose the matching download above for the T-Deck Plus or original T-Echo. The T3-S3 with a 4 MB flash and monochrome OLED has a [separate ZIP and installer](firmware/t3s3-adsb.zip).

The T-Deck Plus release uses ESP-IDF 5.5.5. The application reports build ID `8469bac`. Its firmware sources are available under [tag tdeck-plus-v0.1.0](https://github.com/111blackeagle111/sx1262-adsb/tree/tdeck-plus-v0.1.0). The [T-Echo source and guide](https://github.com/111blackeagle111/sx1262-adsb/tree/techo-v0.1.0) accompany the bilingual UF2. [Package checksums](firmware/SHA256SUMS) · [Third-party notices](firmware/NOTICES.txt)

### T-Deck Plus

Extract the T-Deck ZIP and follow its **README.txt**. It contains the bootloader, the 16 MB target's partition table and the receiver application. Enter BOOT by holding the trackball, pressing and releasing RESET, then releasing the trackball. Check the detected flash size and save and verify the original 16 MB before replacing another firmware. [Step-by-step T-Deck installation and restore commands](install/T-Deck-Plus.txt)

### T-Echo

1. Connect USB and double-press the physical **RESET** button. Open the drive named `TECHOBOOT`.
2. Open `INFO_UF2.TXT`. This image is built for board ID **nRF52840-TEcho-v1** with **S140 version 6.1.1**. Those details matter because they determine where the application starts. Before changing firmware you can copy `CURRENT.UF2` from the drive to your computer as a backup.
3. Copy **techo-adsb.uf2** onto that drive. The drive disappears when the write finishes. The receiver starts automatically and the e-paper refreshes. It also starts when powered from the battery.

The UF2 writes application flash at `0x26000`. Updates preserve the bootloader, SoftDevice, frequency setting and packet journal. To update while the receiver is connected to a serial terminal, type `bootloader` and press Return to open the same drive. During normal use, browse and change settings with the **user button**. The application can appear as `nRF52840 DK` in a USB device list.

### T3-S3

1. Extract the entire ZIP to a writable folder. Open a terminal there. Install the USB flasher with `python -m pip install "esptool>=4.7,<6"`. Use `python3` on macOS/Linux or `py` on Windows if that is how you run Python. On systems that require a virtual environment, run `python3 -m venv .venv` and use `.venv/bin/python` for both commands.
2. Connect USB. Hold **BOOT**, tap and release **RESET**, then release **BOOT**. Close any serial monitor.
3. Run `python install.py`. It finds a single Espressif USB port automatically. With several devices connected, use `python install.py --port COM5` or supply the actual port on your computer. When the write verifies, tap **RESET** if the OLED hasn't started.

The installer checks that the connected chip reports 4 MB of flash. When replacing other firmware, it then reads all 4 MB of flash into a timestamped backup in the current folder and verifies it against the board. It then clears the area the new receiver will use for recordings. When updating an existing receiver, it writes only the bootloader, partition table and application. The frequency and recordings survive. [Installer source](install/t3s3.py)

## Supported hardware

**T3-S3 SX1262.** LilyGO fits this board with an ESP32-S3FH4R2, 4 MB flash, 2 MB quad PSRAM and a 128 × 64 SSD1306 OLED. We used the high-band board. Reception on LilyGO's 400–520 MHz version is unverified. V1.2 and V1.3 share the documented pinout. The exact PCB revision of the tested unit was not recorded. **Check the radio variant on the board or its product listing before flashing.** LilyGO sells several radios under the T3-S3 name. [LilyGO's T3-S3 SX1262 hardware guide and board photos](https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/blob/master/docs/en/t3_s3_sx1262/t3_s3_sx1262_hw.md)

**Original T-Echo SX1262.** This port uses the nRF52840, the high-band SX1262 and the 200 × 200 DEPG0150BN e-paper panel. The pinout, panel orientation and two SPI buses follow the original FreakWAN T-Echo port. T-Echo Lite, Plus, color panels and other bootloader layouts are untested. [LilyGO T-Echo board and pinout](https://github.com/Xinyuan-LilyGO/T-Echo) · [FreakWAN T-Echo port](https://github.com/antirez/freakwan/tree/main/techo-port)

On the T-Echo, an attenuated coax test at 880 MHz recovered the callsign `LAB123` in four of eight transmissions. A second run recovered seven of nine messages. The T-Echo combined the even and odd reports and showed **45.24999° N, 9.75003° E**, **12,500 ft** and **240 kt** on the flight card. At 1090 MHz the board recovered the exact 112-bit private TEST word once in eight transmissions at the stronger setting. That capture peaked at −54.5 dBm over a −84.5 dBm floor. The T3-S3 has also received aircraft outdoors. The T-Echo's outdoor range is still unmeasured. [Five T-Echo recordings used in the replay test](host/fixtures/techo-bench.tsv.gz)

**SX127x boards are incompatible**, including the T3-S3 SX1276 and SX1278 variants and older LoRa32 boards fitted with those chips. The sampler runs code inside the SX1262 itself. An SX127x has different registers and cannot run that code, even when connected to the same ESP32-S3. Other manufacturers' SX1262 boards are untested and may need changes to the SPI pins, oscillator, antenna switch or display driver. [LilyGO board variants](https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series)

## How it receives a packet

1. **Capture power on the radio.** The firmware uses the SX1262's GFSK receive path for its 1 MHz filter. At startup the microcontroller uploads a modified version of Semtech's spectral-scan program into the radio's volatile memory. The program reads signal strength (RSSI) every 0.75 µs and keeps a rolling average of 80 readings. When that 60 µs average rises 2.5 dB above the measured noise floor, it keeps 84 readings up to the crossing and 172 more. Keeping the earlier readings lets the decoder find a preamble that arrived while the average was still rising. [On-radio sampling loop](main/sampler.c)
2. **Keep listening during a decode.** One ESP32-S3 core polls the SX1262 every 250 µs. It reads each completed recording over SPI, puts it in a queue and rearms the radio. The other core can spend much longer working on the previous recording. On the single-core nRF52840, a higher-priority task polls once per RTOS tick (about 0.98 ms) and preempts decoding and e-paper updates. Both boards collect the individual RSSI samples inside the radio at the same speed. The SX1262 holds one completed capture at a time, so a transmission arriving before rearm can still be missed. [Radio driver](main/radio.c) · [Capture and decoder tasks](main/app.c)
3. **Recover the bits from the shape of the signal.** A Mode S bit lasts 1 µs. A 0.5 µs pulse in the first half represents one, and a pulse in the second half represents zero. The radio's filter blurs neighboring pulses together. The decoder finds the preamble, then scores candidate bit sequences against the waveform predicted by measured radio responses. A 64-state trellis keeps plausible alternatives as it goes. The firmware accepts 112-bit DF17 ADS-B messages after checking their 24-bit CRC. It can also combine repeated 56-bit DF11 all-call replies to recover an aircraft address. [Decoder](main/decoder.c) · [Sequence search](main/demod/sequence.c) · [Calibration tables](models)

### An aircraft packet, from power readings to bits

The LilyGO recorded the following transmission from aircraft `4BAA79`. Each dot is one reading from the SX1262. The radio measured a quiet level of −95.5 dBm before this capture. The strongest sample reached −61 dBm. The four preamble pulses occupy the first 8 µs, and the 112 message bits occupy the next 112 µs.

![The recorded RSSI rises out of the noise for 120 µs. A time-aligned strip marks the preamble, header, ICAO address, payload and parity.](docs/4baa79-waveform.png)

Between 40 and 48 µs the aircraft sent `01011000`. Its first five bits, `01011`, identify a type 11 airborne position report. The dots below are the same RSSI readings converted to the power scale used by the decoder. The solid line connects the radio model's predictions at those sample times. Changing just bit 36 changes several predictions because the filter spreads each pulse in time. The decoder compares these errors across the frame and checks the recovered bytes against the CRC.

![Eight decoded bits aligned with measured samples, the fitted response and the response for a one-bit alternative. The lower strip marks the ideal early or late half-microsecond pulse for each bit.](docs/4baa79-bits.png)

The aircraft sent an odd and an even Compact Position Reporting (CPR) message 1.120 seconds apart. Either report gives the altitude, **1,675 ft**. The ESP32 combined the pair and displayed **37.47551° N, 15.17177° E** at the time of the even report. The same pair gave the same position in dump1090. After building the host tools, run `host/build/replay < docs/4baa79-position.tsv` to recover the two frames from the samples. [Raw captures in time order](docs/4baa79-position.tsv) · [Onboard position decoder](main/cpr.c)

## Build from source

### T-Echo

Install Arduino CLI, CMake, Git and Python 3. On Linux, the BSP also calls `adafruit-nrfutil` from your PATH when packaging the image. Install Adafruit’s nRF52 board package, then run the build script from the root of this repository. It uses the PCA10056 pin numbering used by FreakWAN and links the application for S140 6.1.1.

```sh
INDEX=https://adafruit.github.io/arduino-board-index/package_adafruit_index.json
arduino-cli core update-index --additional-urls "$INDEX"
arduino-cli core install adafruit:nrf52@1.3.0 --additional-urls "$INDEX"
./techo/build.sh
```

Copy `build-techo/techo-adsb.uf2` to `TECHOBOOT`. The script fetches the pinned LilyGO checkout for its Adafruit GFX drawing library. Set `LILYGO_T_ECHO` to use an existing checkout. The three boards compile the same radio program, demodulator, CRC checks, CPR and packet-card logic. The T-Echo keeps its calibration tables in flash and reserves space for 12 deferred captures. A second queue holds 16 fresh radio captures. In sustained noise this queue can fill faster than the Nordic can decode it. The USB `status` counter `queue_full` counts captures lost this way. [T-Echo board code](techo) · [Adafruit nRF52 core](https://github.com/adafruit/Adafruit_nRF52_Arduino/tree/1.3.0) · [T-Echo e-paper driver](techo/paper.cpp)

### T3-S3

Use **ESP-IDF v5.1.1** with the ESP32-S3 toolchain. Follow Espressif's setup guide for the prerequisites and run `./install.sh esp32s3` from the ESP-IDF directory on Linux or macOS. Then open a shell in the root of this receiver repository. The commands below assume you installed ESP-IDF in `~/esp/esp-idf`. The project selects the ESP32-S3 target. [ESP-IDF v5.1.1 setup guide](https://docs.espressif.com/projects/esp-idf/en/v5.1.1/esp32s3/get-started/index.html)

```sh
. "$HOME/esp/esp-idf/export.sh"
idf.py build
```

Connect a USB data cable. To enter the ROM loader, hold **BOOT**, press and release **RESET**, then release **BOOT**. Set `PORT` to the serial device that appears *after* that sequence. For example, use `PORT=/dev/cu.usbmodem21101` on macOS or `PORT=/dev/ttyACM0` on Linux, substituting your device's actual name.

### First installation over MicroPython, FreakWAN or another firmware

Back up the whole 4 MB flash if you want to restore the existing installation and its files. These commands use esptool 4.x from the ESP-IDF environment. Wait for the verification to report `verify OK` before proceeding.

```sh
BACKUP="../lilygo-before-adsb-$(date +%Y%m%d-%H%M%S).bin"
python -m esptool --chip esp32s3 -p "$PORT" --before no_reset --after no_reset read_flash 0 0x400000 "$BACKUP"
python -m esptool --chip esp32s3 -p "$PORT" --before no_reset --after no_reset verify_flash 0 "$BACKUP"
```

**On first installation only**, erase the region the new firmware will use for its packet journal. This erases 960 KiB starting at `0x100000`. The receiver formats that region on first boot. [Flash partition layout](partitions.csv)

```sh
python -m esptool --chip esp32s3 -p "$PORT" --before no_reset --after no_reset erase_region 0x100000 0xf0000
```

### Flash and start listening

```sh
idf.py -p "$PORT" flash monitor
```

If the console says `waiting for download`, tap **RESET** with BOOT released. The OLED opens in **LIVE**. On USB, `SXREADY` means the radio passed its trigger checks and is armed. Type `status` and press Return to check it again. Exit the ESP-IDF monitor with **Ctrl+]**. [Espressif's USB reset notes](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/troubleshooting.html)

### T-Deck Plus color display and USB console

This experimental ESP32-S3 target uses the SX1262 in receive mode on the 1090 MHz ADS-B channel. It starts at the same 1089.800 MHz receiver dial as the T3-S3. The 320 × 240 TFT, SX1262 and microSD share the SPI bus. The display sends small strips that changed since the last frame. [LilyGO T-Deck Plus pin mapping](https://github.com/Xinyuan-LilyGO/documentation/blob/master/en/products/t-deck-series/t-deck-plus/index.md#pin-mapping) · [Meshtastic radio wiring and TCXO](https://github.com/meshtastic/firmware/blob/master/variants/esp32s3/t-deck/variant.h)

Identify the RF variant from the SKU on the order or packaging. LilyGO lists H736-A and H736-03 for 868 MHz, H737-A and H737-03 for 915 MHz, H755-A for 433 MHz and H772-01 for 920 MHz. The USB device name and the Meshtastic region setting identify the board and configured region respectively. Reception at 1090 MHz depends on the RF front end and antenna. [LilyGO T-Deck Plus variants](https://lilygo.cc/products/t-deck-plus-meshtastic)

Build with ESP-IDF 5.5.5 using a dedicated build directory. This target uses 16 MB flash and octal PSRAM. Back up and verify the original 16 MB flash before flashing the experimental image. For manual download mode, hold the trackball button while pressing and releasing RST, then release the trackball. [Espressif flash backup and verification commands](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/basic-commands.html)

```sh
idf.py -B build-tdeck-plus -DADS_B_TDECK_PLUS=ON build
```

The T-Deck Plus starts in English until you save a language preference. Open **System → Language** (Italian: **Sistema → Lingua**), roll to **English** or **Italiano**, then click to save. Hold to cancel. The setting applies immediately and survives a restart or power cycle. Both languages use the same local ISO date and time.

The startup screen asks you to confirm or enter the local date and time, then opens **Traffic list**. Roll up or down to select an aircraft, then click to open **Flight details**. In the traffic list, the first horizontal roll highlights **Map**. Roll left or right to choose **Map**, **SD Replay** or **System**, then click to open it. Rolling up or down from the buttons returns the selection to the aircraft. These three buttons are also available while waiting for the first aircraft. Holding and releasing the trackball on the list highlights **System** for confirmation. Once you open **System**, you can read the estimated battery voltage, open **RF reception**, or adjust the backlight and dial. Roll to adjust and press to return to the menu. The ADC measures a shared power rail. A reading above 4.60 V and up to 5.50 V is shown as USB power. Brightness and tuning changes apply immediately and reset on reboot. [LilyGO power schematic](https://github.com/Xinyuan-LilyGO/T-Deck/blob/master/schematic/schematic.pdf)

To open **Traffic map**, highlight **Map** in the list and click. In **Pan**, roll in four directions to pan. A short press switches to **Zoom**, where up and down change one zoom level around the current center. Press again to pan. In **System** and on the live map, **Hold to return to LIVE** means to hold the trackball for about one second and release to return to live reception. On a Replay SD map the same gesture returns to the recorded flight card. The map shows the receiver's position once the GPS has a current fix and plots aircraft with a recent ADS-B position. Before a fix, the initial view is around Rome. Map tiles use `maps/osm/z/x/y.png` or `map/z/x/y.png` on a FAT32 card, 256 × 256 pixels with no interlacing, at zoom levels 1–13. The tiles are read on demand in the SD task and cached in PSRAM. [Meshtastic offline tile layout](https://github.com/meshtastic/device-ui/blob/master/maps/README.md)

Press **L** or choose **System → Screen off** to put the TFT and its backlight to sleep while reception and SD logging continue. Press the trackball or any keyboard key to wake the display on the same page at the previous brightness. The wake input is consumed. USB accepts `screen on`, `screen off` and `screen status`.

While waiting for the first aircraft, **Listening** shows the RF capture count since boot. The amber dot beside the count label blinks once per second while the receiver responds to polling, even if the count is steady. The operating screens show estimated battery percentage or USB power, and **System** shows the voltage estimate. Under **System**, select **GPS reception** to see the fix, satellites used, valid NMEA count, and UART speed. The GPS monitor listens on GPIO44 at 38400 or 9600 baud and uses the read-only UBX-MON-VER request to identify compatible receivers. The `gps version` console command repeats the request. Bluetooth LE is disabled in this firmware. [LilyGO T-Deck Plus pin mapping](https://wiki.lilygo.cc/products/t-deck-series/t-deck-plus/#pin-mapping) · [u-blox MIA-M10Q data sheet](https://content.u-blox.com/sites/default/files/documents/MIA-M10Q_DataSheet_UBX-22015849.pdf)

Use `raw on` and `raw off` over USB to start and stop exporting T-Deck RSSI captures. Each `SXRAW` line contains the boot ID, capture number, previous and current poll times in milliseconds since boot, receiver dial in Hz, noise floor and trigger in half-dB RSSI codes, queue decision and 256 hexadecimal sample bytes. Queue decisions are `Q` for queued, `F` for filtered and `D` for dropped because the decoder queue was full. Raw export has separate loss counters, `raw_queue_drops` and `raw_usb_drops`. The `status` reply on both devices includes `uptime_ms` for aligning T-Deck captures with T-Echo journal records. USB round-trip time and radio polling limit the precision of that comparison.

Insert a FAT32 microSD before boot to log live CRC-valid DF17 messages in `SXADSB/XXXXXXXX.JSL`, where `XXXXXXXX` is the boot ID. Each JSON line uses the T-Echo journal format and contains all 14 message bytes, the dial frequency, capture times since boot and the 256 RSSI samples for that message. Filtered or undecoded captures remain available via `raw on` over USB. Reception also works with the card absent, and the firmware never formats the card. SD writes run in a separate task and can still contend for the shared SPI bus. Over USB, `sd status` shows the number of synced records and drops from the SD queue. Run `sd stop` before removing the card. The receiver drains results already queued when the stop was requested. In `sd status`, `stopped` means the journal synced and the card was unmounted. The state `stopped_error` means the card was unmounted after a data error. An unmount failure produces `error` and prevents a software restart. Reboot with the card inserted to start a new session. [Espressif shared SPI bus guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/peripherals/sdspi_share.html)

In the traffic list, highlight **SD Replay** with the trackball or the **R** key, then click to open it. You can also open **SD Replay** from **System**. Choose a closed `.JSL` journal and click. The replay list shows all the aircraft in the recording, with the most recently received first. Open a flight to see the last available measurements in aeronautical and metric units and the local ISO time of the last message when recorded. If the decoder found a valid position for that aircraft, **Open on map** centers the map on its last recorded position and shows the time of that fix when recorded. Hold and release the trackball to go back one screen, or press **R** to return to live traffic. The radio continues receiving into the live history and journal. The picker shows up to 32 files and loads up to 8192 valid DF17 messages per recording. Over USB, `replay sd` opens the picker, `replay play NAME.JSL` opens a matching file, and `replay status` reports the selected aircraft and timestamps.

To copy a closed journal without removing the card, send `sd read <boot-id> <offset>` over USB. Use the eight hexadecimal digits of the filename and a decimal byte offset starting at zero. Each `SXSDREAD` reply with `state=ok` returns up to 512 original file bytes in `data` as hexadecimal. Decode those bytes, advance the offset by their length, and repeat until `state=eof`. The SD reader leaves live reception and logging active. To copy the current journal, use **System → Restart** first and request the previous boot ID after startup.

At startup, **Date and time** asks for the local date and time. If the clock is still running after **System → Restart**, **Confirm** keeps its time including seconds. After a power-on reset, the screen offers the last confirmed time saved in flash. Check it and choose **Confirm** or **Edit**. On first use or to edit, enter 12 digits in `YYYYMMDDHHMM` order. Hold **sym** for the digit keys and press Enter to save. With the trackball, click a digit to advance and click **Save** to confirm. **System → Date and time** also opens the editor. Over USB use `time set YYYYMMDDHHMM` and `time status`. The receiver keeps listening during setup. A running clock adds the local ISO `local_time` field to new captures. Use **System → Restart** or `restart` on USB for a restart that closes the SD journal cleanly. [ESP32-S3 system time](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/system_time.html)

With a detected MIA-M10Q, a short trackball press in **GPS reception** requests software standby or wakes the receiver. Hold the trackball to return to **System**. The console also accepts `gps standby` and `gps wake`. The application checks that UART output stops and resumes, and wakes the receiver on reboot. [u-blox MIA-M10Q integration manual](https://content.u-blox.com/sites/default/files/documents/MIA-M10Q_IntegrationManual_UBX-21028173.pdf)

**GPS reception** polls UBX-NAV-SAT every two seconds while open. It shows acquired signals and satellites used for navigation by constellation. Dashes indicate that a recent report is unavailable, while zero is a reported count. The USB command `gps satellites` prints the latest table as acquired/used pairs. [u-blox M10 SPG 5.10 interface description](https://content.u-blox.com/sites/default/files/u-blox-M10-SPG-5.10_InterfaceDescription_UBX-21035062.pdf)

Open the USB serial console at 115200 baud. `SXREADY` and `status` with `ready=1` confirm that the sampler passed its self-test and was armed. Live ADS-B reception is confirmed by a line beginning with `SXADSB` containing `df=17 crc=ok` and a 28-digit hexadecimal `word`. `status` also reports the number of decoded DF17 messages, raw captures, radio retries and queue saturation.

Use `tune 1090000000` to try a different dial setting. The console accepts 1089000000 to 1091000000 Hz. `status` reports the applied dial and any rejected frequency. The firmware starts at 1089800000 Hz after a reboot.

USB commands `ui left`, `ui right`, `ui up`, `ui down`, `ui open` and `ui hold` mirror the trackball gestures. In LIVE, `status` reports `list_focus` as 0 for aircraft, 1 for Mappa, 2 for Replay SD or 3 for Sistema. The console also accepts `ui list`, `ui rf`, `ui system`, `ui next` and `ui prev`. Use `ui map` and `map status` to open and inspect the map directly. Host tests replay recorded captures through the decoder and the same RGB565 renderer used by the TFT. On 16 labelled T-Echo transmissions at 1090 MHz, the shared decoder recovers 10 exact 112-bit words, up from 5 before the whole-reply timing update. It also recovers the independently recorded word for block 871. On ESP32-S3, USB `status` reports `decoder_ram=internal` or `decoder_ram=psram` for the decoder workspace. The host build uses the zlib development library for tile decoding. [Recorded samples and expected words](host/fixtures/techo-1090-quiet.tsv.gz) · [Regression checks](host/check.sh)

```sh
cmake -S host -B host/build
cmake --build host/build
ctest --test-dir host/build --output-on-failure
sh host/check-tdeck.sh host/build/replay host/build/test_tdeck build-tdeck-plus/ui-preview
```

For a hardware replay build, pass the generated header as `-DADS_B_TDECK_REPLAY="$PWD/build-tdeck-plus/ui-preview/tdeck_replay_data.h"` in a separate build directory. On that build, `replay on` shows recorded traffic marked **REPLAY**, and `replay off` returns to live reception. The standard build starts and stays in live mode.

To **restore the previous firmware**, enter the ROM loader and run `python -m esptool --chip esp32s3 -p "$PORT" write_flash 0 "$BACKUP"` with the verified backup file, then tap RESET.

### Package the three downloads

After building the standard T3-S3, T-Deck Plus and T-Echo targets above, run the following from the repository root. Supply the actual build directories if they differ. The command validates board selections, flash settings, partition tables and the application-only UF2 before replacing the downloads and checksums in `firmware/`.

```sh
python3 install/package.py --t3-build build --tdeck-build build-tdeck-plus --techo-build build-techo
(cd firmware && sha256sum -c SHA256SUMS)
python3 -B install/test_package.py
```

## Use the receiver

| On the aircraft display | T3-S3 BOOT or T-Echo user button |
| --- | --- |
| Browse the last 64 packet cards | Tap once per older packet |
| Return to live reception | Hold for 0.8 seconds |
| Open frequency settings | Double-click |

On the T-Echo, hold the top capacitive button and count aloud **1001 1002 1003**, then release it to switch the display light on or off. The light changes when you release the sensor: if it was on, it turns off; if it was off, it turns on. To show or hide the battery icon, hold the sensor and count aloud **1001 1002 1003 1004 1005 1006 1007 1008**. As soon as the icon disappears or reappears, the command has been recognized and you can release the sensor. The longer gesture changes battery visibility once and leaves the light unchanged. These counting instructions give a comfortable margin over the firmware's unchanged two-second light and five-second battery thresholds. Brief touches do nothing. The light starts off and the battery icon starts visible after a restart. ADS-B reception and display updates continue with either setting. When the battery is hidden, the first text row returns to its original position.

The T-Echo shows an estimated battery percentage at the top right, updated every 30 seconds while running on battery. It averages eight voltage readings from the board's battery divider and estimates LiPo charge from the voltage. The full endpoint is an ADC reading of 4.14 V, based on this receiver showing 91% with the previous curve after charging had finished. This empirical adjustment is approximate. An out-of-range reading shows `--%`. With USB power connected, it shows `USB` and does not measure or display a percentage: LilyGO documents that the ADC battery reading is inaccurate while charging. USB detection also works with a charger without a data connection. The red charging LED stays on during charging and turns off when fully charged; the firmware cannot read that LED's status. Type `battery` over USB to inspect the power source; `mv=0 percent=-1` means the battery estimate is unavailable on USB. [LilyGO charging and LED documentation](https://github.com/Xinyuan-LilyGO/T-Echo/blob/main/README.MD#charger-led-status)

### T-Echo e-paper refresh

![The T-Echo shows LAB123's altitude, then its coordinates. The callsign stays in place as the detail lines refresh.](docs/techo-refresh.gif)

On a flight or history page, the T-Echo writes new black strokes and erases old ones. It gives the white paper immediately around those strokes a brief black-to-white pulse. This clears the spaces inside letters and numbers while the rest of the page stays still. When the altitude gives way to coordinates, the callsign remains readable throughout. The animation shows the physical display at normal speed.

On our T-Echo at room temperature, changing between a text-heavy flight card and the TEST page took **about 0.38 seconds**. The previous driver took **0.62 seconds** for the same change. After **200 consecutive page changes**, an iPhone photograph of one erased line showed about **3%** of the old lettering’s contrast, measured against a factory-clean copy of the same page. The 200 changes took 379–385 ms each.

The frequency screen washes the white background once when you enter or leave it. As the RSSI graph scrolls, only pixels that actually change get a pulse. A trace can leave a faint trail while you tune. The panel runs its factory clean at power-on. To request another, **hold the user button for three seconds** from a flight or history screen. It returns to LIVE at the usual 0.8 seconds and keeps listening during the clean.

The SSD1681 has two RAM bits per pixel. Together they select one of four waveforms. The driver uses them to leave a pixel alone, draw it black, erase it to white, or briefly invert and restore nearby white paper. On text pages each phase lasts four frames. A changing stroke gets both phases in the same direction. The neighboring white paper gets four frames in each direction. This adapts the fast waveform in the Badger 2040 experiments to the SSD1681. The RSSI graph uses a single six-frame pulse. The firmware powers down the panel's analog supply after each update. [T-Echo driver](techo/paper.cpp) · [Badger waveforms](https://github.com/antirez/uc8151_micropython/blob/cc2a0de91386f08ee6c08e6e315bcfef850ffc49/uc8151.py#L449) · [SSD1681 datasheet, sections 6.5–6.7 and command 0Ch](https://cdn-learn.adafruit.com/assets/assets/000/099/573/original/SSD1681.pdf)

### Reading the display

An aircraft broadcasts its identity, position and speed in separate messages. Each card shows what the receiver knew about that aircraft when the selected packet arrived. The name stays in large letters while the details below it change page every three seconds. In the first screenshot, THY4DC was at **1,675 feet**, moving at **141 knots**, and **descending 704 feet per minute**.

| On the screen | What it means |
| --- | --- |
| `LIVE` · `H49/64` · `1/3` | Live mode follows incoming packets. In history, `H49/64` means the 49th newest of 64 stored cards. The separate `1/3` means the first of three pages for that card. |
| `23s` · `4m` · `2h` · `1d` · `saved` | The top-right corner is the time since reception in seconds, minutes, hours or days. A packet from an earlier boot says `saved` because the board recorded only its uptime. On startup, `LIVE` can show a saved packet while waiting for a new one. |
| `Air 7` | Seven distinct aircraft addresses appear in the last 64 packet cards. Repeated packets from one aircraft count once. Those aircraft may have passed at different times. |
| `THY4DC` · `ICAO 4BAA79` | The **callsign** identifies the flight. The six-character **ICAO address** identifies the aircraft sending the message. Its address appears in large type until the receiver hears its callsign. |
| `1675 ft` · `GNSS … m` · `On ground` | A height in feet is the aircraft's pressure altitude. `GNSS` is height from satellite navigation, in metres. `On ground` comes from a surface report. |
| `GS 141 kt` · `IAS` · `TAS` | Ground speed is how fast the aircraft moves across the ground. Airspeed may be marked indicated (`IAS`) or true (`TAS`). A knot is one nautical mile per hour. A large speed with just `kt` is ground speed. |
| `Track 264 deg` · `Hdg` | Track is the direction of travel over the ground. 0° is north, 90° east and 270° west. Heading is the direction the nose points. Wind can make the two differ. |
| `V/S -704 fpm` · `Sel … ft` | Vertical speed is in feet per minute. A minus sign means descending and a plus sign means climbing. `Sel` is the altitude selected on the aircraft's controls. |
| `37.47551 N` · `15.17177 E` | The aircraft's latitude comes first, then longitude, in decimal degrees. The letters mean north, south, east or west. |
| `TC11 Position` · `TC19 Velocity` · `TC29 Target` | The type code (`TC`) says what this packet carried. Here the aircraft sent a position, a speed report or a target setting. The card can include details learned from earlier packets. |
| `DF11 All-call` · `TEST CRC OK` | An all-call reply identifies an aircraft by address. A test card comes from a bench transmission. The number after `CRC OK` counts accepted test messages, `Seq` is the transmitter's sequence number, and the two rows of hexadecimal digits are the message itself. |
| `1090 MHz` · `Listening` | The receiver is waiting for its first decoded packet on the aircraft channel. |

When several aircraft report close together, live mode lets the current flight finish one turn through its pages before switching. Tap to browse if you want to read a particular packet. The history view stays on it while new ones arrive. [Aircraft identification](https://mode-s.org/1090mhz/content/ads-b/2-identification.html) · [Airborne velocity](https://mode-s.org/1090mhz/content/ads-b/5-airborne-velocity.html)

For the first position the board needs one odd and one even airborne position report from the same aircraft within ten seconds. After that, a single report can update a fix up to a minute old. This lets the board follow an aircraft onto the ground. If the aircraft is first heard on the ground, the board waits for an airborne fix. [Airborne position decoding](https://mode-s.org/1090mhz/content/ads-b/3-airborne-position.html) · [Surface position decoding](https://mode-s.org/1090mhz/content/ads-b/4-surface-position.html)

### Frequency settings and signal graph

The transmit frequency for 1090 MHz ADS-B is fixed. The default **receiver dial is 1089.800 MHz** because that setting placed the aircraft signal in a more sensitive part of this board's receive filter. In settings, **tap** to cycle through increase, decrease, step size, default, discard and clear history. **Hold** to apply the selected action. The radio retunes immediately after a frequency change. **Double-click** to save the frequency across power cycles and return to the aircraft display.

To start a new outing with an empty list, tap until **Clear history** appears, then hold the button for 0.8 seconds until **History cleared** appears. This resets the aircraft count and permanently deletes the saved packet recordings on the board, including those available over USB. The separate background power recordings and the tuned frequency are kept.

`RX` is the frequency the receiver is listening to, in MHz. **A** is average signal strength over 200 ms and **P** is the peak over one second. Both are in dBm. A reading of −75 dBm is stronger than −93 dBm.

The graph plots the last 2.56 seconds on a fixed scale from −120 to −40 dBm. Higher on the screen means a stronger signal. The line shows the average in each 20 ms column and a dot marks its peak. A **vertical stroke** marks a hardware trigger and reaches the peak measured in that capture. A 120 µs aircraft message occupies just 0.6% of one column. In a coax test the radio captured eight bursts at 250 ms intervals. The average stayed near −93 dBm while the bursts peaked near −75 dBm.

When a packet-shaped burst arrives, the graph shows a close-up of **192 µs of signal**, held for three seconds. A label such as `Hold -75 dBm` gives the peak of that burst. The screen then returns to the background signal level, which continued sampling during the hold. Even in busy traffic you get at least one second of live signal between holds. Retuning clears the held trace immediately.

The display looks for a 36 µs interval at least 6 dB above the measured floor and the average at each end of the capture. This lets you see a burst even when its bits are too weak to decode. An aircraft card still requires a valid message.

### T-Echo GPS standby and console observer

The T-Echo requests GPS standby at application startup by holding the L76K WAKEUP signal (P1.02/34) low. This stops satellite acquisition and navigation, rather than disconnecting power. The shared peripheral supply and GPS reset are left alone. The [Quectel hardware manual](https://files.waveshare.com/upload/d/db/Quectel_L76K_Hardware_Design_V1.0.pdf) documents standby through WAKEUP; its specified current is not a measurement on this board.

In a USB serial monitor at 115200 baud, type `gps` and press Return. The first call starts a receive-only observer at 9600 baud; wait a few seconds and repeat `gps` to read its status. Observation does not wake the GPS, send serial commands or drive its reset or command input. The observer runs in a separate task and starts only when requested. Type `gps stop` to release its UART and task; this stops observation without changing the requested GPS mode. Observation starts afresh on the next `gps` command. For a comparison, `gps wake` releases WAKEUP and `gps standby` holds it low again; both also start observation. These mode changes last until the next application restart, which always requests standby.

`requested=standby/continuous` reports the requested mode and `wake_level` reads back the MCU-side WAKEUP signal. These are separate from observed NMEA activity. After `gps standby`, allow at least five seconds for previous observations to expire and compare the counters over time; receiving no new bytes supports the documented standby behavior, but does not measure current or establish complete power removal.

`SXGPS` reports `activity=active` only after receiving a valid NMEA sentence within the last five seconds. `no_data` means no valid sentence has been received yet, and `no_recent_data` means the last one is older than five seconds. Neither silence nor `monitor=0` proves that the GPS is off. `bytes` counts received bytes, `nmea` counts sentences with valid checksums, and `last_ms` is the age of the last valid sentence (`-1` before the first). `fix=yes/no` is the module's latest RMC/GGA fix report; `satellites` is the GGA count of satellites used, with `-1` for unknown. Fix and satellite reports also expire after five seconds. `invalid` counts rejected sentences and `overflow` counts discarded oversized or non-ASCII input lines; these are not a measurement of all UART byte loss. GPS activity proves consumption, but this command does not measure current. The observer does not add GPS information to the display or change ADS-B position decoding.

## Retrieve or replay recordings

The firmware keeps accepted packets and periodic background recordings in separate rotating logs. The T-Echo reserves 28 KiB of internal flash for its filesystem. It keeps up to 36 accepted or supporting traces and eight background captures across two generations of each log. It can show up to 64 packet cards during a session. After a restart it rebuilds the cards from the traces still in flash. The host build produces the replay decoder and, on macOS and Linux, `receiverctl` for reading the board over USB. Close any serial monitor using the port first.

```sh
cmake -S host -B host/build -DCMAKE_BUILD_TYPE=Release
cmake --build host/build
ctest --test-dir host/build --output-on-failure
host/build/receiverctl "$PORT" status
host/build/receiverctl "$PORT" dump > field.jsonl
```

The dump is one JSON object per line. Each capture includes the 256 raw RSSI readings, the noise floor and the actual receiver setting, `dial_hz`. The older `signal_hz` field always identifies the nominal 1090 MHz aircraft channel. For an off-band bench recording, use the generator setting for the carrier frequency. Accepted packets also include the decoded bytes. In a serial monitor, `screen` returns the T3-S3 OLED image as eight pages of 128 bytes in hexadecimal. On the T-Echo it returns the eight lines selected for the e-paper display. Type `paper` for the last update time and counts of factory cleans (`full`), text repaints (`repaint`), graph updates (`fast`), skipped updates and errors. Type `paper clean` to request a full refresh over USB.

The replay tests feed saved RSSI samples through the same C decoder used on both boards. They check 24 expected DF17 packets in 98 outdoor and bench captures, four earlier bench test messages, 15 background captures with no expected message, and both airborne position reports illustrated above. The expected words are kept out of the decoder input. These tests check bit recovery on known recordings. Measuring range or missed transmissions requires a live RF comparison. [Replay tests and fixtures](host)
