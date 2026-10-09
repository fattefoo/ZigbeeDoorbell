#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected in Tools->Zigbee mode"
#endif

#include "esp_sleep.h"
#include "Zigbee.h"

/* Zigbee binary sensor device configuration */
#define DEVICE_ENDPOINT_NUMBER 1
#define BUTTON_PIN_BITMASK(GPIO) (1ULL << GPIO)
#define DEEP_SLEEP_TIMER_US 1800000000  // 30 Minuten
#define PREF_NAME "Prefs"

// Hardware Konfiguration
#define ADC_PIN 0                  // GPIO 0 hängt am Spannungsteiler
#define BUTTON_PIN BOOT_PIN        // lokaler Taster
#define WAKEUP_PIN GPIO_NUM_4      // Optokoppler-Ausgang

// Spannungsteiler 100k/100k -> am ADC kommt die halbe Spannung an
const float dividerRatio = 2.0f;
// Kalibrierte Referenzspannung des ESP32-C6
const float vRef = 3.3f;

// Batterie Grenzwerte (echte Akkuspannung, nach dem Teiler zurückgerechnet)
const float batteryFullVoltage = 3.60f;   // 100 %
const float batteryEmptyVoltage = 3.20f;  // 0 %

// Zeitfenster, in dem ein ausgelöster Klingelton gesendet wird
#define RING_WINDOW_MS 5000

volatile bool sensorValue = false;

// RTC-Retained: überlebt den Deep Sleep, im Gegensatz zu normalen Variablen
RTC_DATA_ATTR uint32_t lastRing = 0;

ZigbeeBinary zbBinaryDevice = ZigbeeBinary(DEVICE_ENDPOINT_NUMBER);

// ------------------------------------------------------------------
// Hilfsfunktionen
// ------------------------------------------------------------------
// Füllt voltageAtPin (was der ADC sieht) und batteryVoltage (echte Spannung).
// Gibt false zurück, wenn die Messung ungültig ist.
bool readBattery(float &batteryVoltage) {
  analogSetPinAttenuation(ADC_PIN, ADC_11db);

  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += analogReadMilliVolts(ADC_PIN);
  }
  float voltageAtPin = (sum / 16.0f) / 1000.0f;

  batteryVoltage = voltageAtPin * dividerRatio;

  // Plausibilitätsprüfung: unter 2 V am Pin kommt nichts sinnvolles an
  if (voltageAtPin < 2.0f || batteryVoltage > 5.0f) {
    return false;
  }
  return true;
}

uint8_t batteryPercentageFrom(float batteryVoltage) {
  float span = batteryFullVoltage - batteryEmptyVoltage;
  if (span <= 0.0f) {
    return 0;
  }
  float percentage = ((batteryVoltage - batteryEmptyVoltage) / span) * 100.0f;
  if (percentage > 100.0f) percentage = 100.0f;
  if (percentage < 0.0f) percentage = 0.0f;
  return (uint8_t)(percentage + 0.5f);
}

void measureBattery() {
  float batteryVoltage = 0.0f;

  if (!readBattery(batteryVoltage)) {
    Serial.println("Batterie-Messung unplausibel, uebersprungen.");
    return;
  }

  uint8_t percentage = batteryPercentageFrom(batteryVoltage);

  // Achtung: setBatteryVoltage erwartet 100-mV-Einheiten (34 == 3.4 V),
  // nicht Millivolt.
  float voltageUnitsRaw = batteryVoltage * 10.0f + 0.5f;
  if (voltageUnitsRaw > 255.0f) voltageUnitsRaw = 255.0f;
  uint8_t voltageUnits = (uint8_t)voltageUnitsRaw;

  zbBinaryDevice.setBatteryPercentage(percentage);
  zbBinaryDevice.setBatteryVoltage(voltageUnits);
  zbBinaryDevice.reportBatteryPercentage();

  Serial.printf("Batterie: %.2f V (%u %%) -> %.1f V-Einheiten\n",
                batteryVoltage, percentage, batteryVoltage);
}

void print_wakeup_reason() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  switch (cause) {
    case ESP_SLEEP_WAKEUP_GPIO: Serial.println("Aufgewacht durch GPIO-Weckquelle"); break;
    case ESP_SLEEP_WAKEUP_TIMER: Serial.println("Aufgewacht durch Timer"); break;
    default: Serial.printf("Aufgewacht durch unbekannte Ursache: %d\n", cause); break;
  }
}

// ------------------------------------------------------------------
// Klingeln
// ------------------------------------------------------------------
// Setzt den Binary Input für die Dauer von RING_WINDOW_MS und setzt ihn
// danach wieder zurück, damit der Coordinator ein Ereignis sieht und
// keinen Dauerzustand.
void ring() {
  sensorValue = true;
  lastRing = millis();
  zbBinaryDevice.setBinaryInput(true);
  // setBinaryInput() setzt das Attribut nur lokal, deshalb explizit melden
  zbBinaryDevice.reportBinaryInput();
  Serial.println("Klingel ausgeloest");
}

void unring() {
  if (!sensorValue) {
    return;
  }
  sensorValue = false;
  zbBinaryDevice.setBinaryInput(false);
  zbBinaryDevice.reportBinaryInput();
  Serial.println("Unring!");
}

// ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(WAKEUP_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  zbBinaryDevice.setManufacturerAndModel("Fambego Industries", "Zigbee Doorbell Sensor");
  zbBinaryDevice.setPowerSource(ZB_POWER_SOURCE_BATTERY, 100, 42);

  zbBinaryDevice.addBinaryInput();
  zbBinaryDevice.setBinaryInputApplication(BINARY_INPUT_APPLICATION_TYPE_SECURITY_OTHER);
  zbBinaryDevice.setBinaryInputDescription("Doorbell");
  // Startzustand explizit auf "nicht gedrückt" setzen
  zbBinaryDevice.setBinaryInput(false);

  zbBinaryDevice.addBinaryOutput();
  zbBinaryDevice.setBinaryOutputApplication(BINARY_OUTPUT_APPLICATION_TYPE_SECURITY_OTHER);
  zbBinaryDevice.setBinaryOutputDescription("Test Doorbell");
  zbBinaryDevice.onBinaryOutputChange([](bool state) {
    if (state) {
      Serial.println("Test Doorbell");
    }
  });

  Zigbee.addEndpoint(&zbBinaryDevice);

  if (!Zigbee.begin()) {
    Serial.println("Zigbee failed to start!");
    Serial.println("Rebooting...");
    ESP.restart();
  }
  Serial.println("Zigbee started successfully!");

  Serial.println("Connecting to network");
  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(100);
  }
  Serial.println("Connected");

  print_wakeup_reason();

  // Nur bei einem GPIO-Wakeup senden. Der Grund wird hier ausgewertet,
  // nicht in wake_up() - sonst senden wir noch bevor die Uhr für
  // millis() vernünftig läuft.
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_GPIO) {
    ring();
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    delay(2000);
  }

  measureBattery();
}

void loop() {
  // Taster lokal auswerten (Entprellung über millis, kein busy-wait)
  static bool buttonWasPressed = false;
  static uint32_t buttonPressedAt = 0;

  bool pressed = (digitalRead(BUTTON_PIN) == LOW);

  if (pressed && !buttonWasPressed) {
    buttonPressedAt = millis();
  }

  if (pressed != buttonWasPressed) {
    delay(50);  // Entprellung
    if ((digitalRead(BUTTON_PIN) == LOW) == pressed) {
      buttonWasPressed = pressed;
      if (!pressed) {
        if (millis() - buttonPressedAt > 3000) {
          Serial.println("Resetting Zigbee to factory and rebooting in 1s.");
          delay(1000);
          Zigbee.factoryReset();
          for (int i = 0; i < 20; i++) {
            delay(100);
          }
          ESP.restart();
        } else {
          ring();
        }
      }
    }
  }

  // Binary Input nach RING_WINDOW_MS wieder zurücksetzen
  if (sensorValue && (millis() - lastRing) >= RING_WINDOW_MS) {
    unring();
  }

  // Deep Sleep, sobald nichts mehr zu tun ist
  if (!sensorValue) {
    Serial.println("Entering Deep Sleep...");
    esp_deep_sleep_enable_gpio_wakeup(BUTTON_PIN_BITMASK(WAKEUP_PIN), ESP_GPIO_WAKEUP_GPIO_HIGH);
    esp_sleep_enable_timer_wakeup(DEEP_SLEEP_TIMER_US);
    esp_deep_sleep_start();
  }

  delay(10);
}