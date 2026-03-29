/**
 * @file music_box.ino
 * @brief ESP32-based toddler music box.
 *
 * Plays random WAV tracks from an SD card via a MAX98357A I2S amplifier.
 * A single button handles two actions:
 *   - Short press  : play a random track (avoids repeating the last track)
 *   - Long press   : cycle through software volume presets
 *
 * Hardware:
 *   - ESP32 devboard
 *   - MAX98357A I2S mono amplifier breakout
 *   - Micro-SD card module via SDMMC (1-bit mode)
 *   - Momentary push button with internal pull-up
 *
 * Dependencies (Arduino library manager):
 *   - ESP8266Audio  (AudioGeneratorWAV, AudioFileSourceFS, AudioOutputI2S)
 *   - Arduino ESP32 core SD_MMC driver (built-in)
 */

#include <Arduino.h>
#include "SD_MMC.h"

#include "AudioFileSourceFS.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"

// =========================
// PIN CONFIG
// =========================

/** GPIO pin the momentary push button is wired to. Uses internal pull-up,
 *  so the button should pull this pin LOW when pressed. */
static const int PIN_BUTTON   = 21;

// MAX98357A I2S pins
static const int PIN_I2S_BCLK = 26; ///< Bit clock (BCLK / SCK)
static const int PIN_I2S_LRC  = 25; ///< Left/right word select (LRCK / WS)
static const int PIN_I2S_DOUT = 22; ///< Serial data out (DIN on the amplifier)

// =========================
// AUDIO / FILE CONFIG
// =========================

/** Absolute path on the SD card where WAV tracks are stored.
 *  All *.wav files directly inside this directory will be discovered. */
static const char* TRACK_DIR = "/tunes";

/** Maximum number of tracks that can be held in memory.
 *  Increase if the SD card holds more than 64 tracks. */
static const int MAX_TRACKS = 64;

/**
 * Software gain presets passed to AudioOutputI2S::SetGain().
 * Values are linear multipliers (0.0 = silent, 1.0 = full scale).
 * Kept low intentionally for a toddler device.
 * The MAX98357A hardware volume is fixed; only these software levels change.
 */
float volumePresets[] = {0.20f, 0.25f, 0.30f, 0.35f};
const int volumePresetCount = sizeof(volumePresets) / sizeof(volumePresets[0]);
int currentVolumePreset = 1; // Default: 0.25 (second-lowest preset)

/** Flat list of resolved SD paths for every discovered WAV file. */
String trackList[MAX_TRACKS];
int trackCount = 0;

/** Index of the most-recently played track; used to prevent immediate repeats. */
int lastTrackIndex = -1;

// =========================
// BUTTON TIMING
// =========================

/** Minimum stable time (ms) before a button state change is accepted. */
const unsigned long debounceMs = 30;

/** Hold duration (ms) that separates a long press from a short press. */
const unsigned long longPressMs = 800;

// --- Internal debounce state (do not modify directly) ---
bool lastRawButtonState  = HIGH; ///< Raw GPIO reading from previous loop iteration
bool stableButtonState   = HIGH; ///< Debounced, confirmed button state
unsigned long lastDebounceTime   = 0;  ///< Timestamp of last raw-state edge
unsigned long buttonPressStartMs = 0;  ///< Timestamp when a stable press began
bool longPressHandled    = false; ///< Guards against firing long-press action multiple times

// =========================
// HEARTBEAT
// =========================

/** Timestamp of the last 1-second serial heartbeat print. */
unsigned long lastHeartbeatMs = 0;

// =========================
// AUDIO OBJECTS
// =========================

/**
 * Heap-allocated audio pipeline objects.
 * Created/destroyed for each track to ensure clean state between plays.
 * Always check for nullptr before use; allocation can fail on low-memory boards.
 */
AudioGeneratorWAV  *wav  = nullptr; ///< WAV decoder (drives the loop pump)
AudioFileSourceFS  *file = nullptr; ///< File source reading from SD_MMC
AudioOutputI2S     *out  = nullptr; ///< I2S output driver (created once in initAudio)

// =========================
// HELPERS
// =========================

/** @brief Print a message to Serial prefixed with [INFO]. */
void logInfo(const char* msg) {
  Serial.print("[INFO] ");
  Serial.println(msg);
}

/** @brief Print a message to Serial prefixed with [WARN]. */
void logWarn(const char* msg) {
  Serial.print("[WARN] ");
  Serial.println(msg);
}

/** @brief Print a message to Serial prefixed with [ERROR]. */
void logError(const char* msg) {
  Serial.print("[ERROR] ");
  Serial.println(msg);
}

/** @brief Print a visual divider line to Serial for readability. */
void printDivider() {
  Serial.println("--------------------------------------------------");
}

/**
 * @brief Return true if the filename ends with ".wav" (case-insensitive).
 * @param name  Filename or path string to test.
 */
bool hasWavExtension(const String& name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".wav");
}

/**
 * @brief Check whether a file exists and can be opened on the given filesystem.
 * @param fs    Filesystem reference (e.g. SD_MMC).
 * @param path  Absolute path to test.
 * @return true if the file opens successfully.
 */
bool fileExists(fs::FS &fs, const char* path) {
  File f = fs.open(path);
  if (!f) return false;
  f.close();
  return true;
}

/**
 * @brief Recursively list directory contents to Serial for diagnostics.
 *
 * Useful at startup to verify the SD card layout without a card reader.
 *
 * @param fs       Filesystem to traverse.
 * @param dirname  Starting directory path.
 * @param levels   How many levels of subdirectories to recurse into.
 */
void listDirRecursive(fs::FS &fs, const char * dirname, uint8_t levels) {
  Serial.print("[INFO] Listing: ");
  Serial.println(dirname);

  File root = fs.open(dirname);
  if (!root) {
    logError("Failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    logError("Path is not a directory");
    return;
  }

  File f = root.openNextFile();
  while (f) {
    if (f.isDirectory()) {
      Serial.print("[DIR ] ");
      Serial.println(f.name());
      if (levels) {
        listDirRecursive(fs, f.path(), levels - 1);
      }
    } else {
      Serial.print("[FILE] ");
      Serial.print(f.name());
      Serial.print("  SIZE=");
      Serial.println((unsigned long)f.size());
    }
    f = root.openNextFile();
  }
}

/**
 * @brief Stop any active playback and free the decoder and file source.
 *
 * Safe to call even when nothing is playing. After this returns, both
 * `wav` and `file` are nullptr; `out` is left intact for reuse.
 */
void stopPlayback() {
  Serial.println("[INFO] stopPlayback()");

  if (wav) {
    if (wav->isRunning()) {
      wav->stop();
    }
    delete wav;
    wav = nullptr;
  }

  if (file) {
    delete file;
    file = nullptr;
  }
}

/**
 * @brief Apply the current volume preset to the I2S output driver.
 *
 * Reads `currentVolumePreset` and calls SetGain() on `out`.
 * No-ops with a warning if `out` has not been initialised yet.
 */
void applyVolumePreset() {
  if (!out) {
    logWarn("Audio output not initialized; cannot apply volume preset");
    return;
  }

  float gain = volumePresets[currentVolumePreset];
  out->SetGain(gain);

  Serial.print("[INFO] Volume preset index: ");
  Serial.println(currentVolumePreset);

  Serial.print("[INFO] Software gain set to: ");
  Serial.println(gain, 2);
}

/**
 * @brief Scan TRACK_DIR on the SD card and populate `trackList[]`.
 *
 * Only files with a .wav extension (case-insensitive) are added.
 * Non-WAV files and directories are skipped. The function also
 * normalises paths to guard against double-slash artefacts from
 * certain SD_MMC firmware versions.
 *
 * @return true if at least one WAV track was found.
 */
bool scanTracks() {
  trackCount = 0;

  Serial.print("[INFO] Scanning track directory: ");
  Serial.println(TRACK_DIR);

  File dir = SD_MMC.open(TRACK_DIR);
  if (!dir) {
    logError("Failed to open track directory");
    return false;
  }

  if (!dir.isDirectory()) {
    logError("Track path exists but is not a directory");
    return false;
  }

  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String fullPath = String(TRACK_DIR) + "/" + String(f.name());

      // Normalise path: some SD_MMC builds return f.name() already including
      // the parent directory, which produces double slashes or duplicated
      // directory segments. Both patterns are collapsed here.
      fullPath.replace("//", "/");
      fullPath.replace(String(TRACK_DIR) + "/" + String(TRACK_DIR) + "/", String(TRACK_DIR) + "/");

      if (hasWavExtension(fullPath)) {
        if (trackCount < MAX_TRACKS) {
          trackList[trackCount] = fullPath;
          Serial.print("[TRACK] ");
          Serial.print(trackCount);
          Serial.print(": ");
          Serial.println(trackList[trackCount]);
          trackCount++;
        } else {
          logWarn("Track list full; skipping extra files");
          break;
        }
      } else {
        Serial.print("[SKIP ] Non-WAV file: ");
        Serial.println(fullPath);
      }
    }
    f = dir.openNextFile();
  }

  Serial.print("[INFO] Total WAV tracks found: ");
  Serial.println(trackCount);

  return trackCount > 0;
}

/**
 * @brief Pick a random track index, avoiding an immediate repeat.
 *
 * When only one track exists, that track is always returned.
 * Uses a busy-wait retry loop; worst-case spins once (50 % chance on 2 tracks).
 *
 * @return Valid index into `trackList`, or -1 if no tracks are loaded.
 */
int chooseRandomTrackIndex() {
  if (trackCount <= 0) return -1;
  if (trackCount == 1) return 0; // No alternative to offer

  int idx;
  do {
    idx = random(0, trackCount);
  } while (idx == lastTrackIndex); // Reject same-track repeat

  return idx;
}

/**
 * @brief Open and begin decoding a specific track by index.
 *
 * Stops any currently running playback before starting the new track.
 * Creates fresh `AudioFileSourceFS` and `AudioGeneratorWAV` objects; both
 * are freed by the next call to stopPlayback() or startPlaybackByIndex().
 *
 * @param index  Zero-based index into `trackList[]`.
 * @return true on successful start, false on any error.
 */
bool startPlaybackByIndex(int index) {
  if (index < 0 || index >= trackCount) {
    logError("Track index out of range");
    return false;
  }

  String path = trackList[index];
  Serial.print("[INFO] Requested playback index: ");
  Serial.println(index);
  Serial.print("[INFO] Requested playback path: ");
  Serial.println(path);

  // Verify the file is still accessible before tearing down current playback
  File test = SD_MMC.open(path);
  if (!test) {
    logError("Track exists in list but could not be opened from SD");
    return false;
  }
  test.close();

  stopPlayback(); // Free previous decoder/source objects

  // --- Construct the audio pipeline: file source → WAV decoder → I2S out ---

  file = new AudioFileSourceFS(SD_MMC, path.c_str());
  if (!file) {
    logError("Failed to allocate AudioFileSourceFS");
    return false;
  }

  if (!file->isOpen()) {
    logError("Audio file source created but file is not open");
    delete file;
    file = nullptr;
    return false;
  }

  Serial.println("[INFO] Audio file opened successfully");

  wav = new AudioGeneratorWAV();
  if (!wav) {
    logError("Failed to allocate AudioGeneratorWAV");
    delete file;
    file = nullptr;
    return false;
  }

  // wav->begin() links the source and output, and starts decoding
  if (!wav->begin(file, out)) {
    logError("wav->begin() failed");
    delete wav;
    wav = nullptr;
    delete file;
    file = nullptr;
    return false;
  }

  lastTrackIndex = index; // Remember so we can avoid repeating it next time

  Serial.println("[INFO] Playback started successfully");
  return true;
}

/**
 * @brief Select a random (non-repeating) track and begin playback.
 *
 * Convenience wrapper around chooseRandomTrackIndex() + startPlaybackByIndex().
 */
void playRandomTrack() {
  if (trackCount <= 0) {
    logError("No tracks available to play");
    return;
  }

  int idx = chooseRandomTrackIndex();
  if (idx < 0) {
    logError("Failed to select random track");
    return;
  }

  startPlaybackByIndex(idx);
}

/**
 * @brief Mount the SD card, log its properties, and scan for tracks.
 *
 * Uses SDMMC in 1-bit mode. In this mode CMD/CLK/D0 are used; the other
 * data lines are left free for GPIO use. Logs the full directory tree at
 * depth 2 to help diagnose card layout issues.
 *
 * @return true if the card mounted, TRACK_DIR exists, and tracks were found.
 */
bool initSD() {
  logInfo("Initializing SD_MMC in 1-bit mode...");

  // 'true' = 1-bit mode; mount point '/sdcard' is the internal VFS path
  if (!SD_MMC.begin("/sdcard", true)) {
    logError("SD_MMC.begin('/sdcard', true) failed");
    return false;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    logError("No SD card detected");
    return false;
  }

  Serial.print("[INFO] Card type: ");
  switch (cardType) {
    case CARD_MMC:  Serial.println("MMC"); break;
    case CARD_SD:   Serial.println("SDSC"); break;
    case CARD_SDHC: Serial.println("SDHC/SDXC"); break;
    default:        Serial.println("Unknown"); break;
  }

  Serial.print("[INFO] Card size MB: ");
  Serial.println((uint32_t)(SD_MMC.cardSize() / (1024ULL * 1024ULL)));

  listDirRecursive(SD_MMC, "/", 2); // Print full tree for diagnostics

  // Confirm the expected track directory exists before scanning
  File tunesDir = SD_MMC.open(TRACK_DIR);
  if (!tunesDir) {
    logError("Track directory '/tunes' not found");
    return false;
  }
  tunesDir.close();

  return scanTracks();
}

/**
 * @brief Initialise the I2S output driver and apply the default volume preset.
 *
 * `out` is created once here and reused for the lifetime of the program.
 * Only the WAV decoder and file source are recreated per track.
 */
void initAudio() {
  logInfo("Initializing I2S output...");

  out = new AudioOutputI2S();
  if (!out) {
    logError("Failed to allocate AudioOutputI2S");
    return;
  }

  out->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);

  Serial.print("[INFO] I2S BCLK pin: ");
  Serial.println(PIN_I2S_BCLK);
  Serial.print("[INFO] I2S LRC pin: ");
  Serial.println(PIN_I2S_LRC);
  Serial.print("[INFO] I2S DOUT pin: ");
  Serial.println(PIN_I2S_DOUT);

  applyVolumePreset();
}

/**
 * @brief Advance to the next volume preset (wraps around) and apply it.
 *
 * Called on confirmed long-press. Cycles:
 *   0 (0.20) → 1 (0.25) → 2 (0.30) → 3 (0.35) → 0 (0.20) → ...
 */
void cycleVolumePreset() {
  currentVolumePreset = (currentVolumePreset + 1) % volumePresetCount;
  Serial.println("[EVENT] Long press confirmed: cycling volume preset");
  applyVolumePreset();
}

/**
 * @brief Handle a confirmed short button press.
 *
 * Stops any currently playing audio, then starts a new random track.
 */
void handleShortPress() {
  Serial.println("[EVENT] Short press confirmed: playing random track");

  if (wav && wav->isRunning()) {
    Serial.println("[INFO] Audio already running; stopping current track first");
    stopPlayback();
  }

  playRandomTrack();
}

// =========================
// SETUP
// =========================

/**
 * @brief One-time initialisation: serial, GPIO, audio pipeline, and SD card.
 *
 * Execution order matters:
 *   1. Serial must be ready before any logging.
 *   2. Audio output (`out`) must exist before SD init, because track scanning
 *      logs through the same serial channel and initAudio sets the gain.
 *   3. SD init runs last; failure is non-fatal — the device boots but cannot
 *      play until a valid card with tracks is inserted and the board reset.
 */
void setup() {
  Serial.begin(115200);
  delay(1000); // Allow the serial monitor to connect before first output

  Serial.println();
  Serial.println("==================================================");
  Serial.println("ESP32 MUSIC BOX DEBUG");
  Serial.println("==================================================");

  // Button uses internal pull-up; LOW = pressed, HIGH = released
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  Serial.print("[INFO] Button pin: ");
  Serial.println(PIN_BUTTON);
  Serial.print("[INFO] Initial button state: ");
  Serial.println(digitalRead(PIN_BUTTON) == LOW ? "LOW (pressed)" : "HIGH (released)");

  // Seed with micros() for better randomness than a fixed seed
  randomSeed(micros());
  logInfo("Random seed initialized");

  initAudio(); // Must happen before initSD (sets up `out` used during scan logging)

  if (!initSD()) {
    logError("SD initialization or track scan failed");
  } else {
    logInfo("SD initialization and track scan succeeded");
  }

  printDivider();
  Serial.println("[INFO] Controls:");
  Serial.println("[INFO]  - Short press: play random track");
  Serial.println("[INFO]  - Long press : cycle volume preset");
  printDivider();
}

// =========================
// LOOP
// =========================

/**
 * @brief Main loop: audio pump, heartbeat logging, and button handling.
 *
 * Three concerns are serviced each iteration:
 *
 * 1. **Heartbeat** — Prints a one-line status summary every second so it is
 *    easy to confirm the device is alive without spamming the serial port.
 *
 * 2. **Audio pump** — `wav->loop()` must be called as fast as possible while
 *    a track is playing; it feeds decoded PCM data to the I2S DMA buffer.
 *    Returning false signals end-of-file or a decode error; both are handled
 *    by stopping playback cleanly.
 *
 * 3. **Button handling** — A two-stage state machine:
 *      a. Edge detection + debounce: ignores glitches shorter than debounceMs.
 *      b. Press classification: on release, the duration determines short vs
 *         long press. Long-press action fires mid-hold once longPressMs elapses
 *         to give immediate feedback, and is guarded by `longPressHandled` so
 *         it cannot fire twice.
 *
 * The 5 ms delay at the end keeps the loop period predictable without
 * impacting audio quality (I2S is DMA-driven and buffer-tolerant).
 */
void loop() {
  // --- 1. Heartbeat ---
  if (millis() - lastHeartbeatMs >= 1000) {
    lastHeartbeatMs = millis();

    Serial.print("[HEARTBEAT] millis=");
    Serial.print(millis());
    Serial.print(" | audio=");
    Serial.print((wav && wav->isRunning()) ? "RUNNING" : "IDLE");
    Serial.print(" | tracks=");
    Serial.print(trackCount);
    Serial.print(" | gain=");
    Serial.println(volumePresets[currentVolumePreset], 2);
  }

  // --- 2. Audio pump ---
  // wav->loop() pushes the next chunk of decoded audio into the I2S DMA buffer.
  // It returns false when the track ends or on a decode error.
  if (wav && wav->isRunning()) {
    if (!wav->loop()) {
      logWarn("Playback finished or wav->loop() returned false");
      stopPlayback();
    }
  }

  // --- 3. Button handling ---

  // 3a. Read raw GPIO and detect edges
  bool rawState = digitalRead(PIN_BUTTON);

  if (rawState != lastRawButtonState) {
    lastDebounceTime = millis(); // Reset debounce timer on any edge
    Serial.print("[DEBUG] Raw button edge: ");
    Serial.println(rawState == LOW ? "LOW" : "HIGH");
  }

  // 3b. Accept state change only after debounceMs of stability
  if ((millis() - lastDebounceTime) > debounceMs) {
    if (rawState != stableButtonState) {
      stableButtonState = rawState;

      if (stableButtonState == LOW) {
        // Press start: record timestamp and arm long-press detection
        Serial.println("[DEBUG] Stable button press detected");
        buttonPressStartMs = millis();
        longPressHandled = false;
      } else {
        // Release: classify the press by its duration
        Serial.println("[DEBUG] Stable button release detected");

        unsigned long pressDuration = millis() - buttonPressStartMs;
        Serial.print("[INFO] Button press duration ms: ");
        Serial.println(pressDuration);

        // Only fire short-press if the long-press action hasn't already fired
        if (!longPressHandled && pressDuration < longPressMs) {
          handleShortPress();
        }
      }
    }
  }

  // 3c. Long-press detection while the button is still held down.
  //     Fires once (guarded by longPressHandled) to give immediate feedback
  //     rather than waiting until the button is released.
  if (stableButtonState == LOW && !longPressHandled) {
    unsigned long heldMs = millis() - buttonPressStartMs;
    if (heldMs >= longPressMs) {
      longPressHandled = true;
      cycleVolumePreset();
    }
  }

  lastRawButtonState = rawState; // Store for next iteration's edge detection

  delay(5); // Yield briefly; audio DMA is unaffected by this short pause
}
