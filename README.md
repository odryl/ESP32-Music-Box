# ESP32 Music Box

A simple, robust music box for toddlers built on the ESP32. Press a button to play a random WAV track from an SD card through a MAX98357A I2S amplifier. Hold the button to cycle the volume up. That's it — deliberately so.

---

## Features

- **Random playback** — plays a random WAV file from `/tunes` on the SD card, never repeating the same track twice in a row
- **Single-button control** — short press plays a track, long press (≥ 800 ms) cycles through four volume presets
- **Safe volume levels** — presets capped at 20–35% software gain, appropriate for a toddler device
- **Auto-stop** — playback halts cleanly at end-of-file; press again to play another track
- **Serial diagnostics** — verbose logging over UART for easy debugging during development

---

## Hardware Requirements

### Bill of Materials

| Component | Description | Notes |
|---|---|---|
| **ESP32 devboard** | Any standard 38-pin ESP32 (e.g. DOIT, AZ-Delivery) | Must expose GPIO 21, 22, 25, 26 |
| **MAX98357A** | I2S Class D mono amplifier breakout | Adafruit #3006 or equivalent clone |
| **Speaker** | 4 Ω or 8 Ω, 2–3 W minimum | Fits inside whatever enclosure you use |
| **Micro-SD module** | Built-in SDMMC or breakout with SD_MMC 1-bit support | Many ESP32 boards include one |
| **Micro-SD card** | Class 10 recommended | FAT32 formatted |
| **Momentary button** | Normally-open push button | Panel-mount or PCB type |
| **5 V power supply** | USB or regulated 5 V | USB power bank works well for portability |

### Wiring

#### MAX98357A → ESP32

| MAX98357A Pin | ESP32 GPIO | Description |
|---|---|---|
| BCLK | GPIO 26 | Bit clock |
| LRC | GPIO 25 | Left/right word select |
| DIN | GPIO 22 | Serial data |
| GND | GND | Ground |
| VIN | 5 V | Power (3.3 V also works at lower volume) |

> **GAIN pin:** leave floating for 9 dB gain (default). Tie to GND for 12 dB or to VIN for 6 dB if you want to set a hardware ceiling.

#### Button → ESP32

| Connection | ESP32 |
|---|---|
| One leg | GPIO 21 |
| Other leg | GND |

The firmware enables the internal pull-up on GPIO 21. No external resistor is needed.

#### SD Card (1-bit SDMMC)

The ESP32's SDMMC peripheral is used in 1-bit mode. Default pin mapping for most devboards:

| Signal | GPIO |
|---|---|
| CLK | GPIO 14 |
| CMD | GPIO 15 |
| D0 | GPIO 2 |

> If your board uses different SDMMC pins, refer to your board's schematic. The firmware uses `SD_MMC.begin("/sdcard", true)` — the `true` flag selects 1-bit mode.

---

## SD Card Setup

1. Format the card as **FAT32**.
2. Create a folder called **`tunes`** in the root of the card.
3. Copy your WAV audio files into `/tunes`. Files in subdirectories are **not** scanned.

```
SD card root
└── tunes/
    ├── song1.wav
    ├── song2.wav
    └── song3.wav
```

### WAV File Requirements

| Property | Requirement |
|---|---|
| Format | PCM WAV (uncompressed) |
| Sample rate | 8–44.1 kHz (44.1 kHz recommended) |
| Bit depth | 16-bit |
| Channels | Mono or stereo (MAX98357A outputs mono; stereo files are mixed down) |

> **Tip:** Use [Audacity](https://www.audacityteam.org/) or `ffmpeg` to convert files:
> ```bash
> ffmpeg -i input.mp3 -ar 44100 -ac 1 -sample_fmt s16 output.wav
> ```

---

## Software Setup

### Prerequisites

- [Arduino IDE](https://www.arduino.cc/en/software) 1.8+ or 2.x
- [ESP32 Arduino core](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html) (Espressif Systems)

### Installing the ESP32 Core

1. Open Arduino IDE → **File → Preferences**
2. Add this URL to *Additional Boards Manager URLs*:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Open **Tools → Board → Boards Manager**, search for `esp32`, and install the **esp32 by Espressif Systems** package.

### Installing Libraries

Install the following library via **Tools → Manage Libraries**:

| Library | Author | Version tested |
|---|---|---|
| **ESP8266Audio** | Earle F. Philhower III | ≥ 1.9.7 |

> Despite the name, ESP8266Audio works on ESP32 and provides `AudioGeneratorWAV`, `AudioFileSourceFS`, and `AudioOutputI2S`.

### Uploading the Sketch

1. Clone or download this repository.
2. Open `music_box.ino` in Arduino IDE.
3. Select your board: **Tools → Board → ESP32 Arduino → ESP32 Dev Module** (or your specific board).
4. Set **Tools → Partition Scheme → Default** (or any scheme with at least 1 MB app space).
5. Select the correct port under **Tools → Port**.
6. Click **Upload**.

---

## Configuration

All tuneable parameters are defined at the top of `music_box.ino`:

```cpp
// Directory on the SD card to scan for WAV files
static const char* TRACK_DIR = "/tunes";

// Maximum number of tracks held in memory (increase if needed)
static const int MAX_TRACKS = 64;

// Software gain presets (0.0 = silent, 1.0 = full scale)
float volumePresets[] = {0.20f, 0.25f, 0.30f, 0.35f};

// Starting preset index (0 = quietest, 3 = loudest)
int currentVolumePreset = 1;

// Button timing
const unsigned long debounceMs  = 30;   // ms to debounce button edges
const unsigned long longPressMs = 800;  // ms hold to trigger long-press action
```

---

## Usage

| Action | Result |
|---|---|
| **Short press** (< 800 ms) | Stops current track (if any) and plays a new random one |
| **Long press** (≥ 800 ms) | Cycles to the next volume preset; releases without changing track |

Volume cycles through four levels: **20% → 25% → 30% → 35% → 20% → ...**

---

## Serial Debugging

Connect at **115200 baud** to see diagnostic output. Useful prefixes:

| Prefix | Meaning |
|---|---|
| `[INFO]` | Normal operational messages |
| `[WARN]` | Non-fatal issues (e.g. end of track) |
| `[ERROR]` | Failures (SD mount, file open, etc.) |
| `[EVENT]` | Button actions |
| `[DEBUG]` | Low-level button state changes |
| `[HEARTBEAT]` | 1-second status line (audio state, track count, gain) |

A healthy startup looks like:

```
==================================================
ESP32 MUSIC BOX DEBUG
==================================================
[INFO] Button pin: 21
[INFO] Initializing I2S output...
[INFO] Initializing SD_MMC in 1-bit mode...
[INFO] Card type: SDHC/SDXC
[INFO] Total WAV tracks found: 12
[INFO] SD initialization and track scan succeeded
--------------------------------------------------
[INFO] Controls:
[INFO]  - Short press: play random track
[INFO]  - Long press : cycle volume preset
--------------------------------------------------
```

---

## Troubleshooting

**No sound / silent output**
- Check BCLK, LRC, and DIN wiring to the MAX98357A.
- Confirm the speaker is connected to the OUTP/OUTN pins of the MAX98357A.
- Try a lower-frequency WAV file (22 kHz mono) to rule out I2S timing issues on your board.

**SD card not detected**
- Ensure the card is FAT32 formatted (not exFAT).
- Try a different, smaller card — some ESP32 SDMMC drivers have compatibility issues with larger SDXC cards.
- Confirm 1-bit SDMMC pin mapping matches your board.

**No tracks found**
- Verify the folder is named exactly `tunes` (lowercase) at the SD card root.
- Confirm files have a `.wav` extension (not `.WAV` — the firmware normalises case, but double-check).
- Check serial output for `[SKIP]` lines showing files being rejected.

**Button not responding**
- Confirm one leg is connected to GPIO 21 and the other to GND.
- Watch serial output for `[DEBUG] Raw button edge` messages to verify GPIO is being read.

---

## Project Structure

```
music_box/
├── music_box.ino   # Main sketch (all logic in one file)
└── README.md       # This file
```

---

## Licence

MIT — do whatever you like with it. If you build one for your kid, that's excellent.
