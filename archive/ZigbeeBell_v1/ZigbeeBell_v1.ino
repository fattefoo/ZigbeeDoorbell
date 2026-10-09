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

// Zustandssteuerung & Gain
uint8_t button = BOOT_PIN;
unsigned long ringStarted = 0;
float duration = 5000.0;
float lastAppliedGain = -1.0; // Um unnötige Updates zu vermeiden
bool doorbellActive = false; // Der entscheidende State für die Loop
const char *bellMp3 = "/bell.wav";

// Audio Objekte (Pointer werden in der Loop dynamisch verwaltet)
AudioGeneratorWAV *wav = nullptr;
AudioFileSourceLittleFS *file = nullptr;
AudioOutputI2S *out = nullptr; 

Preferences preferences;

ZigbeeBinary zbBinaryDevice = ZigbeeBinary(DEVICE_ENDPOINT_NUMBER);
ZigbeeAnalog zbAnalogDevice = ZigbeeAnalog(DEVICE_ENDPOINT_NUMBER + 1);
ZigbeeAnalog zbAnalogDuration = ZigbeeAnalog(DEVICE_ENDPOINT_NUMBER + 2);

// Callback: Wird aufgerufen, wenn der Zigbee-Status sich ändert
void ringDoorbellCallback(bool state) {
  Serial.print("Zigbee Binary Output Change: ");
  Serial.println(state ? "TRUE" : "FALSE");
  doorbellActive = state; 
}

void onAnalogOutputChange(float value) {
  if(out != nullptr) {
    preferences.begin(PREF_NAME, false);
    preferences.putFloat(PREF_GAIN, value);
    preferences.end();
    out->SetGain(value);
  }
}

void onDurationChange(float value) {
  preferences.begin(PREF_NAME);
  preferences.putFloat(PREF_DURATION, value);
  preferences.end();
  duration = value;
}

void setup() {
  Serial.begin(115200);
  
  // 1. Filesystem Setup
  if (!LittleFS.begin()) {
    Serial.println(F("LittleFS Mount Fehler!"));
    return;
  }
  if (!LittleFS.exists(bellMp3)) {
    Serial.printf("%s nicht gefunden! Bitte Datei hochladen.\n", bellMp3);
  }

  float gain;
  preferences.begin(PREF_NAME, true);
  gain = preferences.getFloat(PREF_GAIN, 0.0);
  duration = preferences.getFloat(PREF_DURATION, 5000);
  preferences.end();

  // 2. Audio Setup (Hardware-Komponenten einmalig initialisieren)
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(gain); 
  out->SetOutputModeMono(true);
  wav = new AudioGeneratorWAV();

  // 3. Button Setup
  pinMode(button, INPUT_PULLUP);

  zbBinaryDevice.setManufacturerAndModel("Fambego Industries", "Zigbee Doorbell");

  zbBinaryDevice.addBinaryInput();
  zbBinaryDevice.setBinaryInputApplication(BINARY_INPUT_APPLICATION_TYPE_SECURITY_OTHER);
  zbBinaryDevice.setBinaryInputDescription("Klingelt");

  // Schalter zum Testen der Klingel
  zbBinaryDevice.addBinaryOutput();
  zbBinaryDevice.setBinaryOutputApplication(BINARY_OUTPUT_APPLICATION_TYPE_HVAC_OTHER);
  if(!zbBinaryDevice.setBinaryOutputDescription("Klingeln")) {
    Serial.println("Binary Output Desctription konnte nicht gesetzt werden!");
  }
  // Callback registrieren
  zbBinaryDevice.onBinaryOutputChange(ringDoorbellCallback);
  Zigbee.addEndpoint(&zbBinaryDevice);
  
  // Gain Control
  zbAnalogDevice.addAnalogOutput();
  zbAnalogDevice.setAnalogOutputApplication(ESP_ZB_ZCL_AI_APP_TYPE_OTHER);
  zbAnalogDevice.setAnalogOutputDescription("Lautstärke");
  zbAnalogDevice.setAnalogOutputResolution(0.025);
  zbAnalogDevice.setAnalogOutputMinMax(0.0, 4.0);
  zbAnalogDevice.onAnalogOutputChange(onAnalogOutputChange);
  Zigbee.addEndpoint(&zbAnalogDevice);
  
  // Ring Duration
  zbAnalogDuration.addAnalogOutput();
  zbAnalogDuration.setAnalogOutputApplication(ESP_ZB_ZCL_AI_APP_TYPE_OTHER);
  zbAnalogDuration.setAnalogOutputDescription("Klingeldauer");
  zbAnalogDuration.setAnalogOutputResolution(100.0);
  zbAnalogDuration.setAnalogOutputMinMax(1000.0, 7500.0);
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

  Serial.print("Set Gain: ");
  Serial.println(gain);
  zbAnalogDevice.setAnalogOutput(gain);
  Serial.print("Set Duration: ");
  Serial.println(duration);
  zbAnalogDuration.setAnalogOutput(duration);
}

void loop() {
  if (digitalRead(button) == LOW) {  // Push button pressed
    // Key debounce handling
    delay(100);
    int startTime = millis();
    while (digitalRead(button) == LOW) {
      delay(50);
      if ((millis() - startTime) > 3000) {
        // If key pressed for more than 3secs, factory reset Zigbee and reboot
        Serial.println("Resetting Zigbee to factory and rebooting in 1s.");
        delay(1000);
        Zigbee.factoryReset();
      }
      delay(50);
    }

    // Wenn der Button nur kurz gedrückt wurde (Release vor 3 Sek)
    // Wir simulieren hier das Drücken der Klingel lokal
    Serial.println("Lokaler Button-Druck: Klingel gestartet");
    doorbellActive = !doorbellActive; 
    zbBinaryDevice.setBinaryOutput(doorbellActive);
  }
  
  // FALL A: Der State ist TRUE und wir spielen noch nichts ab -> STARTE AUDIO
  if (doorbellActive && (wav == nullptr || !wav->isRunning())) {
    Serial.println("Starte Wiedergabe...");
    file = new AudioFileSourceLittleFS(bellMp3);
    
    if(ringStarted == 0) {
      ringStarted = millis();
    }

    if (wav->begin(file, out)) {
      Serial.println("Audio Stream gestartet.");
    } else {
      Serial.println("Fehler beim Starten des Streams!");
    }
    zbBinaryDevice.setBinaryInput(doorbellActive);
  }

  // FALL B: Der State ist FALSE und Audio läuft noch -> STOPPE UND REINIGE SPEICHER
  if (!doorbellActive && (wav != nullptr && wav->isRunning())) {
    Serial.println("Status auf FALSE gesetzt. Stoppe Audio...");
    wav->stop();
    
    // WICHTIG: Ressourcen freigeben!
    if (file != nullptr) {
      delete file;
    }
    file = nullptr;
    Serial.println("Speicher bereinigt.");
    zbBinaryDevice.setBinaryInput(doorbellActive);
  }

  // FALL C: Audio läuft -> Loop-Methode des Generators aufrufen
  // if (wav != nullptr && wav->isRunning()) {
  //   if (!wav->loop() || millis()-ringStarted >= duration) {
  //     Serial.println("Datei Ende erreicht.");
  //     wav->stop();
      
  //     // Wenn die Datei von selbst endet, räumen wir ebenfalls auf
  //     if (file != nullptr) {
  //       delete file;
  //       file = nullptr;
  //     }

  //     doorbellActive = false;
  //     zbBinaryDevice.setBinaryInput(doorbellActive);
  //   }
  // }
  if (wav != nullptr && wav->isRunning()) {
    bool finished = !wav->loop();
    
    if (finished) {
      Serial.println("WAV-Datei beendet. Starte von vorne...");
      // Audio erneut starten (Puffer zurücksetzen)
      wav->begin(file, out);
    }

    // Prüfen, ob die konfigurierte maximale Klingeldauer erreicht ist
    if (millis() - ringStarted >= (unsigned long)duration) {
      Serial.println("Konfigurierte Dauer erreicht. Stoppe Audio...");
      wav->stop();
      
      // Ressourcen nur beim tatsächlichen Ende freigeben
      if (file != nullptr) {
        delete file;
        file = nullptr;
      }

      doorbellActive = false;
      ringStarted = 0;
      zbBinaryDevice.setBinaryInput(doorbellActive);
    }
  }

  // Kleiner Delay zur Stabilisierung der CPU
  delay(1);
}
