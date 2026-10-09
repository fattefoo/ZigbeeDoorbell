#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected in Tools->Zigbee mode"
#endif

#include "esp_sleep.h"
#include "Zigbee.h"

/* Zigbee binary sensor device configuration */
#define DEVICE_ENDPOINT_NUMBER 1
#define BUTTON_PIN_BITMASK(GPIO) (1ULL << GPIO)  // 2 ^ GPIO_NUMBER in hex
#define DEEP_SLEEP_TIMER 1800000000
#define PREF_NAME "Prefs"
#define PREF_DURATION "Duration"

// Hardware Konfiguration
const int adcPin = 0;          // Nutze einen ADC Pin (z.B. GPIO 0)
const float dividerRatio = 2.0; // Da wir 100k/100k nutzen, wird die Spannung halbiert
const float vRef = 4.2;         // Referenzspannung des ESP32

// Batterie Grenzwerte
const float batteryFullVoltage = 3.60;  // 100%
const float batteryEmptyVoltage = 3.2; // 0% (kurz vor Tiefentladung)

uint8_t button = BOOT_PIN;
uint8_t wakeupPin = GPIO_NUM_4;

// Boolean Sensor Value
bool sensorValue = false;
long debounceTime = 50;

volatile unsigned long lastRing;
volatile unsigned long lastUnring;

ZigbeeBinary zbBinaryDevice = ZigbeeBinary(DEVICE_ENDPOINT_NUMBER);

void testDoorbell(bool state) {
  if(state)
  {
    Serial.println("Test Doorbell");
  }

  zbBinaryDevice.setBinaryInput(state);
}

void measureBattery() {
  float rawADC = 0;
  for(int i=0;i<10;i++) {
    rawADC += analogRead(adcPin);
  }
  rawADC = rawADC / 10;
  float voltageAtPin = (rawADC / 4095) * vRef; // Spannung am Pin nach dem Teiler
  float batteryVoltage = voltageAtPin * dividerRatio; // Rückrechnung auf echte Akkuspannung

  // 3. Prozentwert berechnen
  float percentage = ((voltageAtPin - batteryEmptyVoltage) / (batteryFullVoltage - batteryEmptyVoltage)) * 100.0;
  // Werte begrenzen (nicht unter 0% und nicht über 100%)
  if (percentage > 100.0) percentage = 100.0;
  if (percentage < 0.0) percentage = 0.0;
  
  zbBinaryDevice.setBatteryPercentage(percentage);
  zbBinaryDevice.setBatteryVoltage((batteryVoltage*100));
  zbBinaryDevice.reportBatteryPercentage();

  Serial.print("voltageAtPin: ");
  Serial.println(voltageAtPin);
  Serial.print("percentage ");
  Serial.println(percentage);
}

void print_wakeup_reason() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  switch (cause) {
    case ESP_SLEEP_WAKEUP_GPIO: Serial.println("Aufgewacht durch GPIO-Weckquelle"); break;
    case ESP_SLEEP_WAKEUP_TIMER: Serial.println("Aufgewacht durch Timer"); break;
    default: Serial.printf("Aufgewacht durch unbekannte Ursache: %d\n", cause); break;
  }
}

void wake_up()
{
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  if(cause == ESP_SLEEP_WAKEUP_GPIO)
  {
    ring();
  }
  else if(cause == ESP_SLEEP_WAKEUP_TIMER)
  {
    delay(2000);
  }

  measureBattery();
}

void ring() 
{
    sensorValue = true;
    lastRing = millis();
    zbBinaryDevice.setBinaryInput(sensorValue);
    Serial.println("Klingel ausgelöst");
  //Klingel wird losgelassen
}

void setup() {
  Serial.begin(115200);
  
  // Configure GPIO for Optocoupler Wake-up (e.g., GPIO 0)
  pinMode(wakeupPin, INPUT);
  // Init button switch
  pinMode(button, INPUT_PULLUP);

  zbBinaryDevice.setManufacturerAndModel("Fambego Industries", "Zigbee Doorbell Sensor");
  zbBinaryDevice.setPowerSource(ZB_POWER_SOURCE_BATTERY, 100, 42);

  zbBinaryDevice.addBinaryInput();
  zbBinaryDevice.setBinaryInputApplication(BINARY_INPUT_APPLICATION_TYPE_SECURITY_OTHER);
  zbBinaryDevice.setBinaryInputDescription("Doorbell");
  
  zbBinaryDevice.addBinaryOutput();
  zbBinaryDevice.setBinaryOutputApplication(BINARY_OUTPUT_APPLICATION_TYPE_SECURITY_OTHER );
  zbBinaryDevice.setBinaryOutputDescription("Test Doorbell");

  zbBinaryDevice.onBinaryOutputChange(testDoorbell);

  Zigbee.addEndpoint(&zbBinaryDevice);

  // When all EPs are registered, start Zigbee in End Device mode
  if (!Zigbee.begin()) {
    Serial.println("Zigbee failed to start!");
    Serial.println("Rebooting...");
    ESP.restart();
  } else {
    Serial.println("Zigbee started successfully!");
  }
  Serial.println("Connecting to network");
  while (!Zigbee.connected()) {
    Serial.print(".");
    delay(100);
  }
  Serial.println("Connected");

  wake_up();
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
    }
  }

  if(millis()-lastRing >= 5000)
  {
    if(sensorValue)
    {
      Serial.println("Unring!");
      sensorValue = false;
      zbBinaryDevice.setBinaryInput(sensorValue);
    }

    delay(5000);
    // Enter Deep Sleep
    Serial.println("Entering Deep Sleep...");
    esp_deep_sleep_enable_gpio_wakeup(BUTTON_PIN_BITMASK(wakeupPin), ESP_GPIO_WAKEUP_GPIO_HIGH);
    esp_sleep_enable_timer_wakeup(DEEP_SLEEP_TIMER);
    esp_deep_sleep_start();
  }
  
  
}