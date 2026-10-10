#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected in Tools->Zigbee mode"
#endif

#include "AudioFileSourceLittleFS.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"
#include "LittleFS.h"
#include "Preferences.h"
#include "Zigbee.h"

// I2S Pins für ESP32-C6 & MAX98357A
#define I2S_LRC 1
#define I2S_BCLK 4
#define I2S_DOUT 18

#define DEVICE_ENDPOINT_NUMBER 1
#define PREF_NAME "Prefs"
#define PREF_GAIN "Gain"
#define PREF_DURATION "Duration"

// Datei im LittleFS-Partition
const char *BELL_FILE = "/bell.wav";

// Cluster-Grenzen (identisch zum Zigbee-Cluster, damit der Coordinator
// keinen Out-of-Range-Wert schickt)
const float GAIN_MIN = 0.0f;
const float GAIN_MAX = 4.0f;
const uint32_t DURATION_MIN = 1000;
const uint32_t DURATION_MAX = 7500;

// Taster: kurzer Druck = Klingel, langer Druck (>3s) = Factory Reset
#define BUTTON_PIN BOOT_PIN
#define LONG_PRESS_MS 3000
#define DEBOUNCE_MS 50

// ------------------------------------------------------------------
// State
// ------------------------------------------------------------------
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

// Wird sowohl aus der loop() als auch aus den Zigbee-Callbacks gesetzt,
// die in einem eigenen Task laufen können.
volatile bool doorbellActive = false;

// Anforderungen, die von Callbacks kommen und in loop() abgearbeitet werden
volatile bool gainDirty = false;
volatile bool durationDirty = false;
float pendingGain = 0.0f;
uint32_t pendingDuration = 5000;

uint32_t ringStarted = 0;
uint32_t durationMs = 5000;
float lastAppliedGain = -1.0f;

bool buttonPressed = false;

// Audio Objekte (Pointer werden in der Loop dynamisch verwaltet)
AudioGeneratorWAV *wav = nullptr;
AudioFileSourceLittleFS *file = nullptr;
AudioOutputI2S *out = nullptr;

Preferences preferences;

ZigbeeBinary zbBinaryDevice = ZigbeeBinary(DEVICE_ENDPOINT_NUMBER);
ZigbeeAnalog zbAnalogDevice = ZigbeeAnalog(DEVICE_ENDPOINT_NUMBER + 1);
ZigbeeAnalog zbAnalogDuration = ZigbeeAnalog(DEVICE_ENDPOINT_NUMBER + 2);

// ------------------------------------------------------------------
// Synchronisierte State-Helfer
// ------------------------------------------------------------------
void setDoorbellActive(bool state) {
  portENTER_CRITICAL(&stateMux);
  doorbellActive = state;
  portEXIT_CRITICAL(&stateMux);
}

bool getDoorbellActive() {
  portENTER_CRITICAL(&stateMux);
  bool v = doorbellActive;
  portEXIT_CRITICAL(&stateMux);
  return v;
}

static uint32_t clampDuration(uint32_t value) {
  if (value < DURATION_MIN) return DURATION_MIN;
  if (value > DURATION_MAX) return DURATION_MAX;
  return value;
}

static float clampGain(float value) {
  if (!(value == value)) return lastAppliedGain < 0.0f ? 0.0f : lastAppliedGain;  // NaN
  if (value < GAIN_MIN) return GAIN_MIN;
  if (value > GAIN_MAX) return GAIN_MAX;
  return value;
}

// ------------------------------------------------------------------
// Callbacks (Zigbee-Task)
// ------------------------------------------------------------------
void ringDoorbellCallback(bool state) {
  Serial.printf("Zigbee Binary Output Change: %s\n", state ? "TRUE" : "FALSE");
  setDoorbellActive(state);
}

void onAnalogOutputChange(float value) {
  pendingGain = value;
  gainDirty = true;
}

void onDurationChange(float value) {
  pendingDuration = clampDuration((uint32_t)value);
  durationDirty = true;
}

// ------------------------------------------------------------------
// Audio
// ------------------------------------------------------------------
// Startet die Wiedergabe neu. Gibt false zurück, wenn der Stream nicht
// geöffnet werden konnte - in dem Fall sind keine Ressourcen belegt.
bool startPlayback() {
  if (!LittleFS.exists(BELL_FILE)) {
    Serial.printf("%s nicht gefunden! Bitte Datei hochladen.\n", BELL_FILE);
    return false;
  }

  if (file == nullptr) {
    file = new AudioFileSourceLittleFS(BELL_FILE);
    if (file == nullptr) {
      Serial.println("Konnte Dateiobjekt nicht allozieren!");
      return false;
    }
  }

  if (wav == nullptr) {
    wav = new AudioGeneratorWAV();
    if (wav == nullptr) {
      Serial.println("Konnte WAV-Generator nicht allozieren!");
      delete file;
      file = nullptr;
      return false;
    }
  }

  // stop() ist auch beim Wiederanlauf nötig, damit der Generator nicht
  // in einem inkonsistenten Zustand bleibt.
  wav->stop();

  if (!wav->begin(file, out)) {
    Serial.println("Fehler beim Starten des Streams!");
    wav->stop();
    delete file;
    file = nullptr;
    return false;
  }

  Serial.println("Audio Stream gestartet.");
  return true;
}

void stopPlayback() {
  if (wav != nullptr) {
    wav->stop();
  }
  if (file != nullptr) {
    delete file;
    file = nullptr;
  }
  ringStarted = 0;
  Serial.println("Audio gestoppt, Speicher bereinigt.");
}

bool isPlaying() {
  return wav != nullptr && wav->isRunning();
}

// ------------------------------------------------------------------
// Taster-Task: entkoppelt von loop(), damit das Audio nicht unterbrochen wird
// ------------------------------------------------------------------
void buttonTask(void *parameter) {
  (void)parameter;
  bool wasPressed = false;
  uint32_t pressedAt = 0;

  for (;;) {
    bool pressed = (digitalRead(BUTTON_PIN) == LOW);

    if (pressed && !wasPressed) {
      pressedAt = millis();
    }

    // Entprellung: erst nach stabilem LOW-Signal reagieren
    if (pressed != wasPressed) {
      delay(DEBOUNCE_MS);
      if ((digitalRead(BUTTON_PIN) == LOW) == pressed) {
        wasPressed = pressed;

        if (!pressed) {
          uint32_t held = millis() - pressedAt;
          if (held >= LONG_PRESS_MS) {
            Serial.println("Resetting Zigbee to factory and rebooting in 1s.");
            delay(1000);
            Zigbee.factoryReset();
            // factoryReset() loest den Reboot selbst aus; falls nicht,
            // erzwingen wir ihn nach kurzer Wartezeit.
            for (int i = 0; i < 20; i++) {
              delay(100);
            }
            ESP.restart();
          } else {
            Serial.println("Lokaler Button-Druck: Klingel gestartet");
            buttonPressed = true;
          }
        }
      }
    }

    delay(10);
  }
}

// ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // 1. Filesystem Setup
  // formatOnFail bleibt bewusst false: ein fehlgeschlagener Mount soll das
  // hochgeladene Image nicht ungefragt loeschen, sondern sichtbar melden.
  // Das Partitions-Label muss zum gewaehlten Partition Scheme passen
  // ("Zigbee 4MB with spiffs" bzw. "Zigbee 8MB with spiffs").
  if (!LittleFS.begin(false)) {
    Serial.println(F("========================================="));
    Serial.println(F("LittleFS Mount Fehler!"));
    Serial.println(F("Die Partition passt nicht zum Dateisystem."));
    Serial.println(F("Wahrscheinlich wurde das bell-Image mit"));
    Serial.println(F("falschem Offset oder falscher Groesse gebaut."));
    Serial.println(F("Klingel bleibt stumm, Zigbee startet trotzdem."));
    Serial.println(F("========================================="));
  } else if (!LittleFS.exists(BELL_FILE)) {
    Serial.println(F("========================================="));
    Serial.printf("%s nicht gefunden!\n", BELL_FILE);
    Serial.println(F("Dateisystem ist gemountet, aber leer."));
    Serial.println(F("Klingel bleibt stumm, Zigbee startet trotzdem."));
    Serial.println(F("========================================="));
  }

  preferences.begin(PREF_NAME, true);
  float gain = clampGain(preferences.getFloat(PREF_GAIN, 0.0f));
  durationMs = clampDuration(preferences.getUInt(PREF_DURATION, 5000));
  preferences.end();
  pendingGain = gain;

  // 2. Audio Setup (Hardware-Komponenten einmalig initialisieren)
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(gain);
  out->SetOutputModeMono(true);
  lastAppliedGain = gain;

  // 3. Button Setup
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  zbBinaryDevice.setManufacturerAndModel("Fambego Industries", "Zigbee Doorbell");

  zbBinaryDevice.addBinaryInput();
  zbBinaryDevice.setBinaryInputApplication(BINARY_INPUT_APPLICATION_TYPE_SECURITY_OTHER);
  zbBinaryDevice.setBinaryInputDescription("Klingelt");
  zbBinaryDevice.setBinaryInput(false);  // initialer, definierter Zustand

  // Schalter zum Testen der Klingel
  zbBinaryDevice.addBinaryOutput();
  zbBinaryDevice.setBinaryOutputApplication(BINARY_OUTPUT_APPLICATION_TYPE_HVAC_OTHER);
  if (!zbBinaryDevice.setBinaryOutputDescription("Klingeln")) {
    Serial.println("Binary Output Description konnte nicht gesetzt werden!");
  }
  zbBinaryDevice.setBinaryOutput(false);
  zbBinaryDevice.onBinaryOutputChange(ringDoorbellCallback);
  Zigbee.addEndpoint(&zbBinaryDevice);

  // Gain Control
  zbAnalogDevice.addAnalogOutput();
  zbAnalogDevice.setAnalogOutputApplication(ESP_ZB_ZCL_AI_APP_TYPE_OTHER);
  zbAnalogDevice.setAnalogOutputDescription("Lautstärke");
  zbAnalogDevice.setAnalogOutputResolution(0.025);
  zbAnalogDevice.setAnalogOutputMinMax(GAIN_MIN, GAIN_MAX);
  zbAnalogDevice.onAnalogOutputChange(onAnalogOutputChange);
  Zigbee.addEndpoint(&zbAnalogDevice);

  // Ring Duration
  zbAnalogDuration.addAnalogOutput();
  zbAnalogDuration.setAnalogOutputApplication(ESP_ZB_ZCL_AI_APP_TYPE_OTHER);
  zbAnalogDuration.setAnalogOutputDescription("Klingeldauer");
  zbAnalogDuration.setAnalogOutputResolution(100.0);
  zbAnalogDuration.setAnalogOutputMinMax(DURATION_MIN, DURATION_MAX);
  zbAnalogDuration.onAnalogOutputChange(onDurationChange);
  Zigbee.addEndpoint(&zbAnalogDuration);

  // When all EPs are registered, start Zigbee in End Device mode
  if (!Zigbee.begin()) {
    Serial.println("Zigbee failed to start!");
    Serial.println("Rebooting...");
    ESP.restart();
  }

  Serial.println("Connecting to network");
  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(10);
  }
  Serial.println("Zigbee & Audio Ready.");

  Serial.printf("Set Gain: %f\n", gain);
  zbAnalogDevice.setAnalogOutput(gain);
  Serial.printf("Set Duration: %lu\n", (unsigned long)durationMs);
  zbAnalogDuration.setAnalogOutput((float)durationMs);

  // Taster-Task erst starten, wenn Zigbee steht, damit der kurze
  // Verbindungs-Block nicht als Tastendruck interpretiert wird.
  xTaskCreate(buttonTask, "button", 4096, NULL, 1, NULL);
}

void loop() {
  // 1. Ausstehende Cluster-Änderungen anwenden (Gain wird nur bei
  //    tatsächlicher Änderung gesetzt und persistiert)
  if (gainDirty) {
    gainDirty = false;
    float value = clampGain(pendingGain);
    if (out != nullptr && value != lastAppliedGain) {
      out->SetGain(value);
      lastAppliedGain = value;
      preferences.begin(PREF_NAME, false);
      preferences.putFloat(PREF_GAIN, value);
      preferences.end();
    }
  }

  if (durationDirty) {
    durationDirty = false;
    if (pendingDuration != durationMs) {
      durationMs = pendingDuration;
      preferences.begin(PREF_NAME, false);
      preferences.putUInt(PREF_DURATION, durationMs);
      preferences.end();
    }
  }

  // 2. Lokaler Taster: löst die Klingel aus, schaltet sie nicht ab
  if (buttonPressed) {
    buttonPressed = false;
    if (!getDoorbellActive()) {
      setDoorbellActive(true);
      zbBinaryDevice.setBinaryOutput(true);
    }
  }

  bool active = getDoorbellActive();

  // 3. Wiedergabe starten
  if (active && !isPlaying()) {
    if (startPlayback()) {
      ringStarted = millis();
      zbBinaryDevice.setBinaryInput(true);
    } else {
      // Stream lässt sich nicht öffnen -> Zustand zurücksetzen,
      // damit wir nicht in einer Endlosschleife hängen bleiben.
      setDoorbellActive(false);
      zbBinaryDevice.setBinaryOutput(false);
      zbBinaryDevice.setBinaryInput(false);
    }
  }

  // 4. Wiedergabe stoppen
  if (!active && isPlaying()) {
    stopPlayback();
    zbBinaryDevice.setBinaryInput(false);
  }

  // 5. Laufende Wiedergabe bedienen
  if (isPlaying()) {
    if (!wav->loop()) {
      // Dateiende -> von vorne beginnen
      wav->stop();
      if (!wav->begin(file, out)) {
        Serial.println("Neustart des Streams fehlgeschlagen.");
        stopPlayback();
        setDoorbellActive(false);
        zbBinaryDevice.setBinaryInput(false);
      } else {
        Serial.println("WAV-Datei beendet. Starte von vorne...");
      }
    }

    // Maximaldauer prüfen
    if (ringStarted != 0 && (millis() - ringStarted) >= durationMs) {
      Serial.println("Konfigurierte Dauer erreicht.");
      stopPlayback();
      setDoorbellActive(false);
      zbBinaryDevice.setBinaryInput(false);
      zbBinaryDevice.setBinaryOutput(false);
    }
  }

  // Kleiner Delay zur Stabilisierung der CPU
  delay(1);
}